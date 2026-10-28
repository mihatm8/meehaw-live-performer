// meehaw card drive — the SECOND firmware of the Cardputer, living in the spare OTA slot (SPEC §9.22; docs/RESEARCH_LIBRARY_EXPANSION.md).
// It exposes the microSD card to the computer as a USB mass-storage device: TinyUSB MSC over raw 512-byte sectors.
// FatFs is NEVER mounted on the device while this runs — the host owns the file system, the device only moves sectors.
//
// Slots (8 MB `default_8MB` table): app0 = ota_0 at 0x10000 holds the player, app1 = ota_1 at 0x340000 holds this
// sketch (both 3.3 MB). tools/flash_drive.sh builds it and writes ONLY the app image at 0x340000. The player gets here
// by pointing the boot partition at ota_1 and restarting (SHIFT + gen pattern held 1 s, or the serial command `drive`).
// The FIRST thing this sketch does on boot is to point the boot partition back at ota_0, so that ANY reset (ENTER here,
// the serial command `player`, the reset button, a crash, a power cycle) returns to the player.
//
// Build: esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled,USBMode=default
// ("USBMode=default" = USB-OTG / TinyUSB; the player is built in the board's default "Hardware CDC and JTAG" mode).
//
// How the MSC is wired (esp32 core 3.3.12, cores/esp32/USBMSC.h):
// - `g_msc` is a GLOBAL USBMSC: its constructor registers the MSC interface with TinyUSB (tinyusb_enable_interface),
//   which only works BEFORE the stack starts. With CDC-on-boot (the board default) cores/esp32/main.cpp runs
//   `Serial.begin(); USB.begin();` before setup(), so an MSC object created in setup() would be refused
//   ("TinyUSB has already started"). The CDC interface is registered the same way by the core's global USBSerial.
// - The composite device (CDC + MSC) therefore enumerates BEFORE setup() runs, with `mediaPresent(false)`: the host
//   sees a drive with no medium and polls TEST UNIT READY (about once a second) until setup() has initialised the
//   card and calls `g_msc.begin(sectors, 512)` + `mediaPresent(true)` — that is the first-enumeration delay (1–2 s
//   from plugging in / switching until the volume mounts).
// - Callbacks run on the TinyUSB device task ("usbd", created by tinyusb_init), NOT on loop(). onRead / onWrite get
//   (lba, byte offset within the lba, buffer, bufsize) with bufsize up to CFG_TUD_MSC_BUFSIZE = 4096 (sdkconfig
//   CONFIG_TINYUSB_MSC_BUFSIZE), i.e. 8 sectors per call; they loop over whole sectors with the SD library's
//   sd_read_raw / sd_write_raw and handle a partial sector (never seen with a 4 KB buffer) through a 512 B scratch,
//   read-modify-write for a partial write. Returning -1 makes TinyUSB stall the command with a sense code; returning
//   0 would make it retry forever, so an out-of-range lba also returns -1.
// - The card is initialised WITHOUT a FatFs mount: sdcard_init() (SD library, sd_diskio.h) registers the SPI driver
//   for a FatFs drive number and disk_initialize(pdrv) runs the SD handshake through it (CMD0 / CMD8 / ACMD41, the CSD
//   gives the sector count); SD.begin() — which would f_mount — is never called. SPI pins SCK 40 MISO 39 MOSI 14 CS 12
//   at 25 MHz, the same as the player. The display is on SPI3_HOST (M5GFX), the card on the Arduino SPI object
//   (SPI2 / FSPI), so loop() may draw while the usbd task moves sectors.
// - onStartStop with (start = 0, load_eject = 1) is the host's Eject: the screen shows "EJECTED" and ENTER returns at
//   once. ENTER before an eject asks for a second press (unflushed host caches would be lost).
//
// TinyUSB caveats: the CDC serial port still works in this mode (it is `Serial`; the port gets a NEW name on the
// computer because the device is now the TinyUSB composite device, not the chip's hardware CDC/JTAG), so the serial
// command `player` and the boot log work; USB re-enumerates on every switch (the computer sees an unplug / replug);
// a sketch upload with esptool into this firmware's CDC port needs the TinyUSB "1200 bps touch" path — return to the
// player first and flash through its hardware-CDC port (tools/flash_drive.sh does that).
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>             // pulls the SD library in for sd_diskio.h; SD.begin() (a FatFs mount) is never called
#include <sd_diskio.h>      // sdcard_init / sdcard_uninit / sdcard_num_sectors / sd_read_raw / sd_write_raw
#include "ff.h"             // FatFs types for diskio.h (BYTE, DSTATUS)
#include "diskio.h"         // disk_initialize: the SD handshake through the driver sdcard_init registered, no mount
#include <USB.h>
#include <USBMSC.h>
#include "soc/rtc_cntl_reg.h"       // RTC_CNTL_USB_CONF_REG: the PHY selection that survives a soft reset (returnToPlayer)
#include "soc/usb_serial_jtag_reg.h"
#include "soc/usb_pins.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>

static constexpr int kPinSck = 40, kPinMiso = 39, kPinMosi = 14, kPinCs = 12;
static constexpr uint32_t kSdHz = 25000000;
static constexpr uint32_t kSector = 512;
static constexpr int kEnterX = 13, kEnterY = 2;       // ENTER on the 4 × 14 matrix (docs/KEYMAP.md)

static USBMSC g_msc;                                  // GLOBAL on purpose: registers the MSC interface before main.cpp starts TinyUSB (see the header)
static uint8_t g_pdrv = 0xFF;                         // FatFs drive number of the card (0xFF = no card)
static uint32_t g_sectors = 0;
static uint8_t g_partial[kSector];                    // one sector for partial transfers (usbd task only)
static volatile uint32_t g_reads = 0, g_writes = 0, g_errors = 0;   // sectors moved (usbd task), shown by loop()
static volatile bool g_ejected = false;               // the host sent START STOP UNIT (eject)
static esp_err_t g_bootBack = ESP_FAIL;               // result of pointing the boot partition back at the player
static uint32_t g_enterArmedUntil = 0;                // ENTER before an eject arms a second press for 3 s

// ---- MSC callbacks (TinyUSB device task) --------------------------------------------------------------------------
static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (g_pdrv == 0xFF || lba >= g_sectors) return -1;
  uint8_t* dst = (uint8_t*)buffer; uint32_t done = 0;
  if (offset) {                                                     // partial head (offset inside the first sector)
    if (offset >= kSector || !sd_read_raw(g_pdrv, g_partial, lba)) { g_errors++; return -1; }
    uint32_t n = kSector - offset; if (n > bufsize) n = bufsize;
    memcpy(dst, g_partial + offset, n); done += n; lba++;
  }
  while (bufsize - done >= kSector) {                               // whole sectors straight into the host buffer
    if (lba >= g_sectors || !sd_read_raw(g_pdrv, dst + done, lba)) { g_errors++; return -1; }
    done += kSector; lba++; g_reads++;
  }
  if (done < bufsize) {                                             // partial tail
    if (lba >= g_sectors || !sd_read_raw(g_pdrv, g_partial, lba)) { g_errors++; return -1; }
    memcpy(dst + done, g_partial, bufsize - done); done = bufsize;
  }
  return (int32_t)done;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (g_pdrv == 0xFF || lba >= g_sectors) return -1;
  uint32_t done = 0;
  if (offset) {                                                     // partial head: read-modify-write
    if (offset >= kSector || !sd_read_raw(g_pdrv, g_partial, lba)) { g_errors++; return -1; }
    uint32_t n = kSector - offset; if (n > bufsize) n = bufsize;
    memcpy(g_partial + offset, buffer, n);
    if (!sd_write_raw(g_pdrv, g_partial, lba)) { g_errors++; return -1; }
    done += n; lba++;
  }
  while (bufsize - done >= kSector) {
    if (lba >= g_sectors || !sd_write_raw(g_pdrv, buffer + done, lba)) { g_errors++; return -1; }
    done += kSector; lba++; g_writes++;
  }
  if (done < bufsize) {                                             // partial tail: read-modify-write
    if (lba >= g_sectors || !sd_read_raw(g_pdrv, g_partial, lba)) { g_errors++; return -1; }
    memcpy(g_partial, buffer + done, bufsize - done);
    if (!sd_write_raw(g_pdrv, g_partial, lba)) { g_errors++; return -1; }
    done = bufsize;
  }
  return (int32_t)done;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;
  if (load_eject) g_ejected = !start;                               // start = 0 + eject = the host unmounted the volume; start = 1 = loaded again
  return true;
}

// ---- screen (loop task) -----------------------------------------------------------------------------------------
static void drawScreen(const char* status) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextSize(2); d.setTextColor(0x07FF, TFT_BLACK); d.drawString("USB DRIVE", 10, 8);
  d.setTextSize(1); d.setTextColor(TFT_WHITE, TFT_BLACK);
  char l[64];
  if (g_pdrv == 0xFF) snprintf(l, sizeof l, "NO SD CARD");
  else { const double gb = (double)g_sectors * kSector / 1e9; snprintf(l, sizeof l, "%.1f GB card, %u sectors", gb, (unsigned)g_sectors); }
  d.drawString(l, 10, 40);
  d.drawString("eject on the computer,", 10, 60);
  d.drawString("then press ENTER to return", 10, 72);
  d.setTextColor(g_bootBack == ESP_OK ? 0x07E0 : 0xF800, TFT_BLACK);
  d.drawString(g_bootBack == ESP_OK ? "any reset returns to the player" : "WARNING: player slot not set", 10, 92);
  d.setTextColor(TFT_WHITE, TFT_BLACK); d.drawString(status, 10, 116);
}
static void drawActivity() {
  static uint32_t lastR = ~0u, lastW = ~0u, lastE = ~0u; static bool lastEj = false;
  const uint32_t r = g_reads, w = g_writes, e = g_errors; const bool ej = g_ejected;
  if (r == lastR && w == lastW && e == lastE && ej == lastEj) return;
  lastR = r; lastW = w; lastE = e; lastEj = ej;
  char l[64]; snprintf(l, sizeof l, "%s rd %u wr %u%s%u", ej ? "EJECTED" : "mounted", (unsigned)r, (unsigned)w, e ? " ERR " : "", (unsigned)e);
  auto& d = M5Cardputer.Display; d.setTextSize(1); d.setTextColor(ej ? 0x07E0 : TFT_WHITE, TFT_BLACK);
  d.fillRect(0, 116, 240, 12, TFT_BLACK); d.drawString(l, 10, 116);
}

static void returnToPlayer(const char* why) {
  Serial.printf("[drive] %s: restarting into the player (ota_0)\n", why);
  auto& d = M5Cardputer.Display; d.fillScreen(TFT_BLACK); d.setTextSize(2); d.setTextColor(0x07E0, TFT_BLACK); d.drawString("BACK TO PLAYER", 10, 50);
  delay(150);                                                       // let the text and the serial line out
  // The USB PHY selection lives in RTC_CNTL_USB_CONF and SURVIVES a software reset: TinyUSB pointed the PHY at the
  // USB-OTG controller, so after a plain esp_restart() the player boots with its hardware CDC/JTAG port dead until a
  // power cycle (observed twice: the device vanished from the host after `player`). Point the PHY back at the
  // integrated CDC+JTAG and pull D+/D- low for a moment so the host sees a disconnect — the same steps the core's
  // usb_switch_to_cdc_jtag() performs for a bootloader restart (esp32-hal-tinyusb.c).
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, (RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE));
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  pinMode(USBPHY_DM_NUM, OUTPUT_OPEN_DRAIN); pinMode(USBPHY_DP_NUM, OUTPUT_OPEN_DRAIN);
  digitalWrite(USBPHY_DM_NUM, LOW); digitalWrite(USBPHY_DP_NUM, LOW);
  delay(60);
  esp_restart();
}

void setup() {
  // 1. Before anything that can fail: the NEXT boot is the player again, whatever ends this one. esp_ota_set_boot_partition
  //    verifies the ota_0 image and rewrites otadata (state NEW; every Arduino sketch marks itself valid at boot through
  //    initArduino → esp_ota_mark_app_valid_cancel_rollback, so the core's CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE never ping-pongs).
  const esp_partition_t* player = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  g_bootBack = player ? esp_ota_set_boot_partition(player) : ESP_ERR_NOT_FOUND;

  auto cfg = M5.config(); cfg.internal_mic = false; cfg.fallback_board = m5::board_t::board_M5CardputerADV;
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  drawScreen("starting the card...");

  // 2. The card, raw: driver registration + SD handshake, no FatFs mount (the host owns the file system).
  SPI.begin(kPinSck, kPinMiso, kPinMosi, kPinCs);
  g_pdrv = sdcard_init(kPinCs, &SPI, kSdHz);
  if (g_pdrv != 0xFF) {
    if (disk_initialize(g_pdrv) & STA_NOINIT) { sdcard_uninit(g_pdrv); g_pdrv = 0xFF; }
    else g_sectors = sdcard_num_sectors(g_pdrv);
  }

  // 3. The LUN: strings, callbacks, capacity; media present only with a card. USB.begin() ran in main.cpp already
  //    (CDC on boot) — the call here is a no-op safety net for a build without CDC on boot.
  g_msc.vendorID("meehaw"); g_msc.productID("CARD DRIVE"); g_msc.productRevision("1.0");
  g_msc.onStartStop(onStartStop); g_msc.onRead(onRead); g_msc.onWrite(onWrite);
  g_msc.isWritable(true);
  const bool lunOk = g_pdrv != 0xFF && g_sectors > 0 && g_msc.begin(g_sectors, (uint16_t)kSector);
  g_msc.mediaPresent(lunOk);
  USB.begin();

  Serial.printf("[drive] card=%s sectors=%u (%.1f GB) msc=%d bootback=%d\n", g_pdrv == 0xFF ? "none" : "ok", (unsigned)g_sectors,
                (double)g_sectors * kSector / 1e9, (int)lunOk, (int)g_bootBack);
  drawScreen(lunOk ? "mounted" : "no medium offered");
}

void loop() {
  M5Cardputer.update();
  for (int i = 0; i < 7; i++) M5Cardputer.Keyboard.updateKeyList();   // drain the TCA8418 FIFO (one event per call on the ADV)
  static bool enterWas = false; bool enter = false;
  for (auto& p : M5Cardputer.Keyboard.keyList()) if (p.x == kEnterX && p.y == kEnterY) enter = true;
  const uint32_t now = millis();
  if (enter && !enterWas) {                                           // ENTER pressed (edge)
    if (g_ejected || g_pdrv == 0xFF || now < g_enterArmedUntil) returnToPlayer("ENTER");
    g_enterArmedUntil = now + 3000;
    auto& d = M5Cardputer.Display; d.setTextSize(1); d.setTextColor(0xFFE0, TFT_BLACK); d.fillRect(0, 104, 240, 12, TFT_BLACK); d.drawString("not ejected: ENTER again to force", 10, 104);
  }
  if (g_enterArmedUntil && now >= g_enterArmedUntil) { g_enterArmedUntil = 0; M5Cardputer.Display.fillRect(0, 104, 240, 12, TFT_BLACK); }
  enterWas = enter;

  static char line[32]; static int len = 0;                           // serial: `player` restarts into ota_0
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') { if (len) { line[len] = 0; if (!strncmp(line, "player", 6)) returnToPlayer("serial"); else if (!strncmp(line, "stat", 4)) Serial.printf("[drive] sectors=%u rd=%u wr=%u err=%u ejected=%d\n", (unsigned)g_sectors, (unsigned)g_reads, (unsigned)g_writes, (unsigned)g_errors, (int)g_ejected); } len = 0; }
    else if (len < (int)sizeof line - 1) line[len++] = ch;
  }
  static uint32_t lastDraw = 0;
  if (now - lastDraw >= 250) { lastDraw = now; drawActivity(); }
  delay(5);
}
