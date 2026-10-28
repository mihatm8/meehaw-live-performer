// Device probe: heap after each init step, DSP micro-benchmarks, SD timing, IMU and keyboard dump.
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <math.h>

static void heapLine(const char* tag) {
  Serial.printf("[heap] %-20s free=%7u largest=%7u\n", tag, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

static int16_t table[4096];
static uint8_t utable[4096];
static int16_t ulaw[256];
static int16_t delayLine[16384];
static int16_t out[256];

static void buildUlaw() {
  for (int i = 0; i < 256; i++) {
    int u = ~i & 0xFF; int sign = u & 0x80; int e = (u >> 4) & 7; int m = u & 15;
    int s = (((m << 3) + 0x84) << e) - 0x84;
    ulaw[i] = sign ? -s : s;
  }
}
static uint8_t encUlaw(int16_t pcm) {
  const int BIAS = 0x84; int sign = (pcm >> 8) & 0x80; if (sign) pcm = -pcm; if (pcm > 32635) pcm = 32635;
  pcm += BIAS; int e = 7; for (int mask = 0x4000; (pcm & mask) == 0 && e > 0; e--, mask >>= 1) {}
  int m = (pcm >> (e + 3)) & 0x0F; return ~(sign | (e << 4) | m);
}

struct Voice { uint32_t pos = 0; uint32_t inc = 0; int16_t gain = 20000; };

static void benchSampler(const char* name, bool mu, int nv) {
  Voice v[16]; for (int i = 0; i < nv; i++) { v[i].inc = (uint32_t)((0.7f + 0.1f * i) * 65536.0f); }
  uint32_t t0 = micros(); volatile int32_t sink = 0; const int blocks = 22050 * 2 / 256;
  for (int b = 0; b < blocks; b++) {
    for (int n = 0; n < 256; n++) {
      int32_t acc = 0;
      for (int i = 0; i < nv; i++) {
        uint32_t idx = v[i].pos >> 16; uint32_t fr = v[i].pos & 0xFFFF;
        int32_t s0, s1;
        if (mu) { s0 = ulaw[utable[idx & 4095]]; s1 = ulaw[utable[(idx + 1) & 4095]]; }
        else { s0 = table[idx & 4095]; s1 = table[(idx + 1) & 4095]; }
        int32_t s = s0 + (((s1 - s0) * (int32_t)fr) >> 16);
        acc += (s * v[i].gain) >> 15;
        v[i].pos += v[i].inc;
      }
      out[n] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }
    sink += out[0];
  }
  uint32_t dt = micros() - t0;
  Serial.printf("[bench] %-22s %d voices: %.1f ns/sample/voice (%.2f%% of 22050 Hz core time for %d voices)\n", name, nv,
                dt * 1000.0f / (blocks * 256.0f) / nv, 100.0f * dt / 2e6f, nv);
}

static void benchSvf(int nv) {
  float z1[16] = {0}, z2[16] = {0}; float g = tanf(3.14159f * 2000.0f / 22050.0f); float k = 1.0f; float a1 = 1.0f / (1.0f + g * (g + k)); float a2 = g * a1; float a3 = g * a2;
  uint32_t t0 = micros(); volatile float sink = 0; float x = 0.1f;
  for (int n = 0; n < 22050 * 2; n++) {
    float acc = 0;
    for (int i = 0; i < nv; i++) {
      float v3 = x - z2[i]; float v1 = a1 * z1[i] + a2 * v3; float v2 = z2[i] + a2 * z1[i] + a3 * v3;
      z1[i] = 2 * v1 - z1[i]; z2[i] = 2 * v2 - z2[i]; acc += v2;
    }
    x = -x; sink += acc;
  }
  uint32_t dt = micros() - t0;
  Serial.printf("[bench] %-22s %d voices: %.1f ns/sample/voice\n", "svf float", nv, dt * 1000.0f / (22050 * 2.0f) / nv);
}

static void benchSynth(int nv) {
  // wavetable osc (int phase) + float svf + exponential env per voice
  uint32_t ph[16] = {0}, inc[16]; float env[16]; float z1[16] = {0}, z2[16] = {0};
  for (int i = 0; i < nv; i++) { inc[i] = 1000000u * (i + 3); env[i] = 1.0f; }
  float g = tanf(3.14159f * 1500.0f / 22050.0f); float k = 0.8f; float a1 = 1.0f / (1.0f + g * (g + k)); float a2 = g * a1; float a3 = g * a2;
  uint32_t t0 = micros(); volatile float sink = 0;
  for (int n = 0; n < 22050 * 2; n++) {
    float acc = 0;
    for (int i = 0; i < nv; i++) {
      ph[i] += inc[i]; float x = table[ph[i] >> 20] * (1.0f / 32768.0f) * env[i]; env[i] *= 0.99995f;
      float v3 = x - z2[i]; float v1 = a1 * z1[i] + a2 * v3; float v2 = z2[i] + a2 * z1[i] + a3 * v3;
      z1[i] = 2 * v1 - z1[i]; z2[i] = 2 * v2 - z2[i]; acc += v2;
    }
    sink += acc;
  }
  uint32_t dt = micros() - t0;
  Serial.printf("[bench] %-22s %d voices: %.1f ns/sample/voice (%.2f%% core)\n", "synth osc+svf+env", nv, dt * 1000.0f / (22050 * 2.0f) / nv, 100.0f * dt / 2e6f);
}

static void benchMisc() {
  uint32_t t0 = micros(); volatile float sink = 0; float x = 0.3f;
  for (int n = 0; n < 22050 * 2; n++) { sink += tanhf(x); x = -x * 1.01f; if (x > 3) x = 0.3f; }
  Serial.printf("[bench] %-22s %.1f ns/sample\n", "tanhf", (micros() - t0) * 1000.0f / (22050 * 2.0f));
  t0 = micros(); x = 0.3f;
  for (int n = 0; n < 22050 * 2; n++) { float y = x * (27 + x * x) / (27 + 9 * x * x); sink += y; x = -x * 1.01f; if (x > 3) x = 0.3f; }
  Serial.printf("[bench] %-22s %.1f ns/sample\n", "rational softclip", (micros() - t0) * 1000.0f / (22050 * 2.0f));
  t0 = micros(); uint32_t w = 0; int32_t fb = 0;
  for (int n = 0; n < 22050 * 2; n++) { int32_t r = delayLine[(w + 16384 - 11025) & 16383]; int32_t in = (n & 1 ? 3000 : -3000) + ((r * 20000) >> 15); delayLine[w & 16383] = (int16_t)in; w++; fb += r; }
  Serial.printf("[bench] %-22s %.1f ns/sample (fb %d)\n", "delay int16", (micros() - t0) * 1000.0f / (22050 * 2.0f), (int)(fb & 1));
  t0 = micros(); for (int i = 0; i < 4096 * 10; i++) utable[i & 4095] = encUlaw(table[i & 4095]);
  Serial.printf("[bench] %-22s %.1f ns/sample\n", "ulaw encode", (micros() - t0) * 1000.0f / (4096 * 10.0f));
  t0 = micros(); for (int n = 0; n < 1000; n++) { M5Cardputer.Display.fillRect((n % 8) * 28 + 4, 60, 24, 24, n & 1 ? TFT_MAGENTA : TFT_BLACK); }
  Serial.printf("[bench] %-22s %.1f us/call\n", "fillRect 24x24", (micros() - t0) / 1000.0f);
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = false;
  cfg.fallback_board = m5::board_t::board_M5CardputerADV;
  Serial.begin(115200);
  delay(1500);
  heapLine("boot");
  M5Cardputer.begin(cfg, true);
  heapLine("after begin");
  Serial.printf("[board] id=%d adv=%d imu=%d\n", (int)M5.getBoard(), (int)m5::board_t::board_M5CardputerADV, (int)M5.Imu.getType());
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.drawString("liveplayer probe", 10, 10);
  heapLine("after display");
  auto spk = M5Cardputer.Speaker.config();
  spk.sample_rate = 22050; spk.stereo = false; spk.dma_buf_len = 256; spk.dma_buf_count = 4; spk.task_priority = 3; spk.task_pinned_core = 1;
  M5Cardputer.Speaker.config(spk);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(96);
  heapLine("after speaker");
  SPI.begin(40, 39, 14, 12);
  uint32_t t0 = millis();
  bool sd = SD.begin(12, SPI, 25000000);
  heapLine("after sd");
  Serial.printf("[sd] ok=%d in %u ms type=%d size=%llu MB\n", sd, (unsigned)(millis() - t0), (int)SD.cardType(), (unsigned long long)(SD.cardSize() / (1024 * 1024)));
  if (sd) {
    const char* dirs[] = {"/", "/lp", "/lp/midi/Bass/Amin", "/lp/midi/Chord/Amin", "/lp/packs/House/drum/kick"};
    for (auto d : dirs) {
      t0 = millis(); File dir = SD.open(d); int n = 0;
      if (dir) { File f; while ((f = dir.openNextFile())) { n++; f.close(); } dir.close(); }
      Serial.printf("[sd] list %-28s %4d entries %4u ms\n", d, n, (unsigned)(millis() - t0));
    }
    t0 = millis(); File f = SD.open("/lp/packs/House/drum/kick"); if (f && f.isDirectory()) { File g = f.openNextFile(); if (g) { static uint8_t rb[4096]; size_t tot = 0; while (tot < 44100 * 2) { int r = g.read(rb, sizeof rb); if (r <= 0) break; tot += r; } Serial.printf("[sd] read %u bytes of %s in %u ms\n", (unsigned)tot, g.name(), (unsigned)(millis() - t0)); g.close(); } f.close(); }
  }
  for (int i = 0; i < 4096; i++) table[i] = (int16_t)(sinf(i * 6.2831853f / 4096.0f) * 20000.0f);
  buildUlaw();
  for (int i = 0; i < 4096; i++) utable[i] = encUlaw(table[i]);
  benchSampler("sampler int16 lerp", false, 8);
  benchSampler("sampler ulaw lerp", true, 8);
  benchSampler("sampler ulaw lerp", true, 16);
  benchSvf(8);
  benchSynth(8);
  benchSynth(16);
  benchMisc();
  heapLine("after bench");
  // audio path check: 0.5 s of 440 Hz
  uint32_t ph = 0; for (int b = 0; b < 43; b++) { for (int n = 0; n < 256; n++) { ph += (uint32_t)(440.0f / 22050.0f * 4096.0f * 65536.0f); out[n] = table[(ph >> 16) & 4095] / 4; } while (!M5Cardputer.Speaker.playRaw(out, 256, 22050, false, 1, 0)) vTaskDelay(1); }
  Serial.println("[audio] tone sent");
  heapLine("after audio");
  Serial.println("[ready] press keys / tilt; BtnA prints heap");
}

void loop() {
  M5Cardputer.update();
  static uint32_t last = 0;
  if (millis() - last > 1000) {
    last = millis();
    float ax, ay, az, gx, gy, gz;
    bool a = M5.Imu.getAccel(&ax, &ay, &az); bool g = M5.Imu.getGyro(&gx, &gy, &gz);
    Serial.printf("[imu] acc=%d %.2f %.2f %.2f  gyro=%d %.1f %.1f %.1f\n", a, ax, ay, az, g, gx, gy, gz);
  }
  if (M5Cardputer.Keyboard.isChange()) {
    auto& ks = M5Cardputer.Keyboard.keysState();
    Serial.printf("[key] n=%d fn=%d shift=%d ctrl=%d opt=%d alt=%d tab=%d del=%d enter=%d space=%d word=", (int)M5Cardputer.Keyboard.isPressed(), ks.fn, ks.shift, ks.ctrl, ks.opt, ks.alt, ks.tab, ks.del, ks.enter, ks.space);
    for (auto c : ks.word) Serial.printf("%c", c);
    Serial.print(" coords=");
    for (auto& p : M5Cardputer.Keyboard.keyList()) Serial.printf("(%d,%d)", p.x, p.y);
    Serial.println();
  }
  if (M5Cardputer.BtnA.wasPressed()) heapLine("btnA");
  delay(2);
}
