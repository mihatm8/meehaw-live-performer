// meehaw card loader — writes files to the Cardputer's microSD over the USB serial port (no disk mounting needed).
// Protocol (lines end with \n, responses are lines):
//   PING                 -> PONG
//   STAT <path>          -> S <size> | S -1 | D
//   MKDIR <path>         -> OK | ERR
//   RM <path>            -> OK | ERR
//   LS <path>            -> F <size> <name> / D <name> ... END
//   CRC <path>           -> C <crc32 hex>
//   MV <a> <b>           -> OK | ERR (rename, also directories)
//   RMALL <dir>          -> OK <n> (removes the files directly inside <dir>)
//   PUT <size> <path>    -> GO, then the host sends <size> bytes in chunks of 4096 (last one shorter); the device
//                           answers A after each chunk and OK <bytes> at the end (ERR <why> on failure).
//   GET <path>           -> G <size> then exactly <size> raw bytes (sent in 4096-byte chunks, binary-safe), or
//                           G -1 when the path is missing or a directory. Same reply as the player's `get <path>`
//                           (SPEC §9.22); tools/card_pull.py pulls a directory through either firmware.
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>

static bool sdOk = false;
static uint8_t chunk[4096];
static uint32_t crcTable[256];
static uint32_t filesDone = 0, bytesDone = 0;

static void crcInit() { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcTable[i] = c; } }
static uint32_t crcUpdate(uint32_t crc, const uint8_t* p, size_t n) { crc = ~crc; while (n--) crc = crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8); return ~crc; }

static void show(const char* l1, const char* l2) {
  auto& d = M5Cardputer.Display; d.fillScreen(TFT_BLACK); d.setTextSize(2); d.setTextColor(0x07E0, TFT_BLACK); d.drawString("CARD LOADER", 10, 10);
  d.setTextSize(1); d.setTextColor(TFT_WHITE, TFT_BLACK); d.drawString(l1, 10, 44); d.drawString(l2, 10, 60);
}
static void progress() {
  char l[64]; snprintf(l, sizeof l, "files %u  %u KB", (unsigned)filesDone, (unsigned)(bytesDone / 1024));
  auto& d = M5Cardputer.Display; d.setTextSize(1); d.setTextColor(TFT_WHITE, TFT_BLACK); d.fillRect(0, 80, 240, 12, TFT_BLACK); d.drawString(l, 10, 80);
}

static bool readExact(uint8_t* dst, size_t n, uint32_t timeoutMs) {
  size_t got = 0; uint32_t t0 = millis();
  while (got < n) {
    int avail = Serial.available();
    if (avail > 0) { int r = Serial.read(dst + got, n - got); if (r > 0) { got += r; t0 = millis(); continue; } }
    if (millis() - t0 > timeoutMs) return false;
    delay(1);
  }
  return true;
}

static void cmdPut(char* args) {
  uint32_t size = 0; char path[200];
  if (sscanf(args, "%u %199s", &size, path) != 2) { Serial.println("ERR args"); return; }
  if (!sdOk) { Serial.println("ERR nosd"); return; }
  File f = SD.open(path, FILE_WRITE);              // "w" truncates; no separate remove (one directory scan less)
  if (!f) { Serial.println("ERR open"); return; }
  Serial.println("GO");
  uint32_t left = size; bool ok = true;
  while (left > 0) {
    size_t n = left < sizeof chunk ? left : sizeof chunk;
    if (!readExact(chunk, n, 5000)) { ok = false; Serial.println("ERR timeout"); break; }
    if (f.write(chunk, n) != n) { ok = false; Serial.println("ERR write"); break; }
    left -= n; bytesDone += n;
    Serial.println("A");
  }
  f.close();
  if (!ok) { SD.remove(path); return; }
  filesDone++; progress();
  Serial.printf("OK %u\n", (unsigned)size);
}

static void cmdGet(const char* path) {
  File f = SD.open(path, FILE_READ); if (!f || f.isDirectory()) { if (f) f.close(); Serial.print("G -1\n"); return; }
  uint32_t left = f.size(); Serial.printf("G %u\n", (unsigned)left);
  while (left > 0) {
    const size_t n = left < sizeof chunk ? left : sizeof chunk;
    const int r = f.read(chunk, n); if (r <= 0) break;              // card error: the host sees a short stream
    left -= (uint32_t)r;
    if (Serial.write(chunk, (size_t)r) != (size_t)r) break;         // the host stopped reading
  }
  f.close();
}

static void cmdCrc(const char* path) {
  File f = SD.open(path, FILE_READ); if (!f || f.isDirectory()) { if (f) f.close(); Serial.println("C -"); return; }
  uint32_t crc = 0; int r;
  while ((r = f.read(chunk, sizeof chunk)) > 0) crc = crcUpdate(crc, chunk, (size_t)r);
  f.close(); Serial.printf("C %08x\n", (unsigned)crc);
}

static void handle(char* line) {
  if (!strncmp(line, "PING", 4)) { Serial.println("PONG"); return; }
  if (!strncmp(line, "STAT ", 5)) { File f = SD.open(line + 5); if (!f) Serial.println("S -1"); else { if (f.isDirectory()) Serial.println("D"); else Serial.printf("S %u\n", (unsigned)f.size()); f.close(); } return; }
  if (!strncmp(line, "MKDIR ", 6)) { Serial.println(SD.exists(line + 6) || SD.mkdir(line + 6) ? "OK" : "ERR"); return; }
  if (!strncmp(line, "RM ", 3)) { Serial.println(SD.remove(line + 3) || SD.rmdir(line + 3) ? "OK" : "ERR"); return; }
  if (!strncmp(line, "LS ", 3)) { File d = SD.open(line + 3); if (d && d.isDirectory()) { File e; while ((e = d.openNextFile())) { const char* nm = e.name(); const char* b = nm; for (const char* p = nm; *p; p++) if (*p == '/') b = p + 1; if (e.isDirectory()) Serial.printf("D %s\n", b); else Serial.printf("F %u %s\n", (unsigned)e.size(), b); e.close(); } } if (d) d.close(); Serial.println("END"); return; }
  if (!strncmp(line, "CRC ", 4)) { cmdCrc(line + 4); return; }
  if (!strncmp(line, "MV ", 3)) { char a[200], b[200]; if (sscanf(line + 3, "%199s %199s", a, b) == 2) Serial.println(SD.rename(a, b) ? "OK" : "ERR"); else Serial.println("ERR args"); return; }
  if (!strncmp(line, "RMALL ", 6)) {                           // remove every file directly inside a directory (keeps subdirectories)
    File d = SD.open(line + 6); int n = 0; if (d && d.isDirectory()) { for (;;) { File e = d.openNextFile(); if (!e) break; bool dir = e.isDirectory(); char full[260]; snprintf(full, sizeof full, "%s/%s", line + 6, (const char*)(strrchr(e.name(), '/') ? strrchr(e.name(), '/') + 1 : e.name())); e.close(); if (!dir && SD.remove(full)) n++; } } if (d) d.close(); Serial.printf("OK %d\n", n); return; }
  if (!strncmp(line, "PUT ", 4)) { cmdPut(line + 4); return; }
  if (!strncmp(line, "GET ", 4)) { cmdGet(line + 4); return; }
  Serial.println("ERR unknown");
}

void setup() {
  auto cfg = M5.config(); cfg.internal_mic = false; cfg.fallback_board = m5::board_t::board_M5CardputerADV;
  Serial.setRxBufferSize(16384);
  Serial.begin(115200);
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  crcInit();
  SPI.begin(40, 39, 14, 12);
  sdOk = SD.begin(12, SPI, 25000000);
  show(sdOk ? "serial file mode ready" : "NO SD CARD", "tools/card_sync.py PORT sdcard");
}

void loop() {
  static char line[256]; static int len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') { if (len) { line[len] = 0; handle(line); } len = 0; }
    else if (len < (int)sizeof line - 1) line[len++] = ch;
  }
  delay(1);
}
