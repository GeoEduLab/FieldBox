/*
 * Field Box (formerly "Cutiuța de Teren") — autonomous field data logger
 * for the Pâclele Mari campaign.
 *
 * The box logs continuously from power-on: one CSV row every 5 s, whether
 * or not the app has told it to "record" — Record/Stop only drop marker
 * rows into the running log. See docs/superpowers/specs/
 * 2026-09-11-session-flow-0.4-design.md §0.
 *
 * The ESP32 exposes a Bluetooth Classic SPP serial port named BOX_XXXX.
 * The Field Box BT tablet app pairs once, then sends one-line JSON commands
 * and receives the same JSON bodies the WiFi firmware served over HTTP.
 * Wire contract: docs/superpowers/specs/2026-08-02-fieldbox-bt-design.md
 * in the fieldbox repo.
 *
 * Sensors : BME280 (T/H/P) · SCD41 (CO₂, ~5 s cycle) · VEML7700 (lux,
 *           auto-ranged from 0.4.0: ~0.004 lx to direct sunlight)
 *           and, from 0.4.0, the optional sensors of the EASE MagicBox
 *           firmware 2.4.4, each found at boot or left out ("not fitted"
 *           on Serial, nothing blocks): BH1750 (lux, when no VEML7700),
 *           SGP30 (TVOC/eCO2), SGP41 (VOC/NOx index), BMV080 (PM1/PM2.5/
 *           PM10, also on crossed SDA/SCL), STEMMA soil sensor (seesaw).
 *           Their values travel only in the "live" reply, as extra keys an
 *           older app ignores; the CSV files on the box keep their 13
 *           columns, because the app up to 0.9.0 rejects any other count.
 * Wiring  : SDA=GPIO21  SCL=GPIO22  VCC=3.3V  GND=GND · I2C 100 kHz
 * Board   : ESP32 Dev Module. Ships its own partitions.csv (app 1.375 MB /
 *           LittleFS 2.56 MB). The ESP32 core (3.x) copies a partitions.csv
 *           found next to the .ino over the menu's table, so the Arduino IDE
 *           and firmware/compile.sh both apply it. In the IDE, set Tools →
 *           Partition Scheme → "No OTA (2MB APP/2MB SPIFFS)": that menu only
 *           sets the size check, and the default 1.2 MB one would refuse the
 *           app long before it outgrows its real 1.375 MB slot (0.3.1: 1.19 MB).
 * Libraries: Adafruit BME280 · Adafruit VEML7700 · Adafruit Unified Sensor
 *            SparkFun SCD4x · BH1750 (claws) · Adafruit seesaw ·
 *            Adafruit SGP30 · Sensirion Gas Index Algorithm ·
 *            DFRobot_BMV080 (ZIP from github.com/DFRobot/DFRobot_BMV080)
 *            (BluetoothSerial/LittleFS/Preferences: ESP32 core)
 *
 * Status LED (GPIO2): solid 2 s at boot = sensors OK · 500 ms blink =
 * recording · 100 ms blink = storage full.
 *
 * Upgrading from 0.2: the LittleFS partition moves, so the first boot after
 * flashing this firmware prints "[FS] mount failed twice — formatting" and
 * every CSV already on the box is lost. Download all before upgrading.
 */
#include <esp_system.h>
#include <Wire.h>
#include <FS.h>
#include <LittleFS.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <SparkFun_SCD4x_Arduino_Library.h>
#include <BH1750.h>
#include <Adafruit_seesaw.h>
#include <Adafruit_SGP30.h>
#include <DFRobot_BMV080.h>
#undef ERR_OK   // DFRobot's macro clashes with lwip's err_enum_t
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>
#include <Preferences.h>
#include "BluetoothSerial.h"
#include <esp_bt.h>          // esp_bredr_tx_power_set
#include "esp32-hal-bt.h"    // btStart

// Paired with the Field Box BT app: bump the two together whenever the BT
// wire contract changes.
#define FW_VERSION  "0.4.1-bt"
#define BT_PREFIX   "BOX_"
#define LED_PIN     2
#define SLOW_MS     1000      // slow sensors at 1 Hz (CSV rows every LOG_MS)
#define SCD_MS      1000      // poll SCD41 at 1 s: the sensor produces every ~5 s,
                              // but polled at exactly 5 s the phase can lock just
                              // before data-ready → permanent miss (the CO₂ card
                              // showed "–" even though the sensor was fine)
#define FLUSH_EVERY 30        // flush LittleFS every 30 rows
#define MIN_FREE_B  102400UL  // below 100 KB free → roll off the oldest log
#define LOG_MS      5000      // one CSV row every 5 s, from power-on
#define PRE_N       720       // rows buffered before the clock is known (1 h at 5 s)

char DEVICE_ID[12];
BluetoothSerial SerialBT;
static char btLine[512];
static size_t btLen = 0;

// ── Sensors ─────────────────────────────────────────────────────
Adafruit_VEML7700 veml;  bool veml_ok  = false;
Adafruit_BME280   bme;   bool bme_ok   = false;
SCD4x             scd41; bool scd41_ok = false;

// Optional sensors (0.4.0), driven as in the EASE MagicBox firmware 2.4.4.
// Each is probed once at boot; absent = left out, nothing waits on it.
BH1750 bh;               bool bh_ok    = false;   // lux, only without a VEML7700
static const uint8_t SOIL_ADDRS[] = {0x36, 0x37, 0x38, 0x39};
Adafruit_seesaw   soil;  bool soil_ok  = false;
Adafruit_SGP30    sgp;   bool sgp_ok   = false;
#define SGP_WARMUP_MS     15000UL
#define SGP_HUM_MS        60000UL
#define SGP_BASE_FIRST_MS (12UL * 3600000UL)
#define SGP_BASE_EVERY_MS ( 1UL * 3600000UL)
#define HW_NAMESPACE      "fieldbox_hw"
static unsigned long t_sgp_start = 0, t_sgp_hum = 0, t_sgp_base = 0;
static uint8_t sgp_fail = 0;

#define SGP41_ADDR        0x59
#define SGP41_CMD_COND    0x2612
#define SGP41_CMD_MEAS    0x2619
#define SGP41_CMD_TEST    0x280E
#define SGP41_CMD_SERIAL  0x3682
#define SGP41_COND_S      10
VOCGasIndexAlgorithm voc_algo;
NOxGasIndexAlgorithm nox_algo;
bool sgp41_ok = false;
static uint8_t sgp41_cond = SGP41_COND_S, sgp41_pending = 0, sgp41_fail = 0;

static const uint8_t BMV_ADDRS[] = {0x57, 0x56, 0x55, 0x54};
#define BMV_SERVE_MS     200UL
#define BMV_WARMUP_MS  15000UL
#define BMV_STALE_MS    3000UL
#define BMV_DEAD_MS    30000UL
DFRobot_BMV080_I2C* bmv = nullptr;
bool    bmv_ok = false, bmv_swapped = false;
uint8_t bmv_addr = 0;
static unsigned long t_bmv_start = 0, t_bmv_serve = 0, t_bmv_data = 0;
static struct { float pm1, pm25, pm10; bool obstructed; } c_bmv = {0, 0, 0, false};

// Last valid values; *_valid = false ⇒ empty cell in CSV.
struct Readings {
  bool  bme_valid = false, scd_valid = false, veml_valid = false;
  float T = NAN, H = NAN, P = NAN, alt = NAN;
  uint16_t co2 = 0;
  float t_scd = NAN, h_scd = NAN;
  float lux = NAN;
} cur;

// 0.4.0 optional sensors: live reply only, never in the CSV files. Kept out
// of Readings on purpose: the 720-row pre-sync ring copies Readings, and
// these fields there would cost 29 KB of RAM the Bluetooth stack needs.
struct ExtraReadings {
  bool  pm_valid = false, pm_obs = false, voc_valid = false, nox_valid = false;
  bool  sgp30_valid = false, soil_valid = false;
  int   pm1 = 0, pm25 = 0, pm10 = 0;
  int32_t voc = 0, nox = 0;
  uint16_t tvoc = 0, eco2 = 0, soil = 0;
  float soil_t = NAN;
} ext;

// Rows sampled before the phone synced the clock: kept with their uptime and
// written with real timestamps the moment the first /api/time arrives.
struct PreRow { uint32_t up_s; Readings r; };
static PreRow pre[PRE_N];
static int pre_head = 0, pre_len = 0;

// ── Time and station ───────────────────────────────────────────────
// The internal clock is set to LOCAL time (UTC epoch from the phone minus
// getTimezoneOffset — Romania in summer: tz_min=-180 ⇒ local = UTC+3h),
// so gmtime_r() returns wall-clock time for the CSV directly.
static bool time_synced = false;
// Epoch of power-on, known only after the first time sync (the box has no
// RTC): the app uses it to tell a power cycle from a Bluetooth drop.
static time_t boot_epoch = 0;
static char station[24] = "P1";
static char lat_s[16] = "", lon_s[16] = "";

// Bosch's precompiled BMV080 driver needs a large stack on the loop task
// (the default 8 KB crashes); DFRobot's examples use 60 KB. Measured on
// BOX_A445 (25.10.2026, BME280 + SGP41 + BMV080 continuous, BT up): the loop
// used at most ~12.5 KB, and 60 KB left only 8 KB of heap next to BT Classic
// -> crash loop at boot. 28 KB = 2x the measured peak. Placed here, after
// the types: the macro defines a function, and the IDE inserts its generated
// prototypes above the first function of the sketch.
SET_LOOP_TASK_STACK_SIZE(28 * 1024);

static void fmtT(char* buf, size_t n, const char* fmt) {
  time_t t = time(nullptr);
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(buf, n, fmt, &tm);
}
static void fmtDate(char* b, size_t n)    { fmtT(b, n, "%Y-%m-%d"); }
static void fmtTime(char* b, size_t n)    { fmtT(b, n, "%H:%M:%S"); }
static void fmtDay(char* b, size_t n)     { fmtT(b, n, "%Y%m%d"); }

// commas/newlines in user input would break the CSV
static void sanitize(char* s) {
  for (; *s; s++)
    if (*s == ',' || *s == '\n' || *s == '\r' || *s == '"' || *s == '\\') *s = ' ';
}

// ── CSV storage on LittleFS ─────────────────────────────────────
static bool     fs_ok        = false;
static bool     recording    = false;
static bool     storage_full = false;
static File     logFile;
static char     logName[40] = "";
static uint32_t rowCount    = 0;
static time_t   rec_start   = 0;

static const char CSV_HEADER[] =
  "date,time,station,lat,lon,temp_C,rh_pct,press_hPa,"
  "alt_m,co2_ppm,temp_scd_C,rh_scd_pct,light_lux\n";

static uint32_t freeBytes() {
  return fs_ok ? (uint32_t)(LittleFS.totalBytes() - LittleFS.usedBytes()) : 0;
}

// snprintf returns the desired length, not the written one — on truncation
// we stop accumulating, otherwise n - len would underflow and write past the buffer.
static size_t addf(char* buf, size_t len, size_t n, const char* fmt, ...) {
  if (len >= n) return n - 1;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(buf + len, n - len, fmt, ap);
  va_end(ap);
  if (w < 0) return len;
  len += (size_t)w;
  return len < n ? len : n - 1;
}

static void fmtAt(char* buf, size_t n, const char* fmt, time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(buf, n, fmt, &tm);
}

// One CSV row for readings `r` at wall-clock `at`. `label` goes in the
// station column (a station name on Read-now rows, ">REC"/"<REC" on marker
// rows, "" on ordinary log rows); coordinates only with a real station.
static size_t buildCsvRowAt(char* buf, size_t n, const char* label, bool with_coords,
                            time_t at, const Readings& r) {
  char d[12], t2[10];
  fmtAt(d, sizeof d, "%Y-%m-%d", at); fmtAt(t2, sizeof t2, "%H:%M:%S", at);
  size_t len = addf(buf, 0, n, "%s,%s,%s,%s,%s,", d, t2, label,
                    with_coords ? lat_s : "", with_coords ? lon_s : "");
  if (r.bme_valid)
    len = addf(buf, len, n, "%.2f,%.2f,%.2f,%.1f,", r.T, r.H, r.P, r.alt);
  else
    len = addf(buf, len, n, ",,,,");
  if (r.scd_valid)
    len = addf(buf, len, n, "%u,%.1f,%.1f,", r.co2, r.t_scd, r.h_scd);
  else
    len = addf(buf, len, n, ",,,");
  if (r.veml_valid)
    len = addf(buf, len, n, "%.1f\n", r.lux);
  else
    len = addf(buf, len, n, "\n");
  return len;
}

static size_t buildCsvRow(char* buf, size_t n, bool with_station) {
  return buildCsvRowAt(buf, n, with_station ? station : "", with_station, time(nullptr), cur);
}

// Set by writeMark(): the next row in the continuous log gets the station
// label, so the point is visible directly in the log (not just in points_*.csv).
static bool log_tag_pending = false;

static char log_day[10] = "";     // yyyymmdd of the open file, for the midnight roll

// Opens /log_<stamp>.csv for `at` (boot time on first open, midnight on a
// roll), writes the header. false if the FS is not usable.
static bool openLogAt(time_t at) {
  if (!fs_ok) return false;
  ensureFree();   // a box powered on with almost no free space rolls off before opening
  if (logFile) { logFile.flush(); logFile.close(); }
  char c[20];
  fmtAt(c, sizeof c, "%Y%m%d_%H%M%S", at);
  snprintf(logName, sizeof logName, "/log_%s.csv", c);
  logFile = LittleFS.open(logName, FILE_WRITE);
  if (!logFile) { Serial.printf("[LOG] open %s FAILED\n", logName); return false; }
  logFile.print(CSV_HEADER);
  logFile.flush();   // a power cut in the first 30 rows must not leave a 0-byte file
  fmtAt(log_day, sizeof log_day, "%Y%m%d", at);
  rowCount = 0;
  Serial.printf("[LOG] open %s\n", logName);
  return true;
}

// Keeps MIN_FREE_B free by deleting the oldest log_*.csv that is not the
// open one. points_*.csv are the teacher's readings and are never touched.
// If nothing is left to delete, the log is closed and storage_full is set —
// Read now keeps working until the flash is truly full.
static void ensureFree() {
  while (fs_ok && freeBytes() < MIN_FREE_B) {
    String oldest;
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while (f) {
      String p = f.path();
      // A closed log (storage-full) is just a name at this point and is
      // fair game; only the actively-open handle is protected.
      if (!f.isDirectory() && p.startsWith("/log_") && p.endsWith(".csv") &&
          !(logFile && p == String(logName)) && (oldest.isEmpty() || p < oldest))
        oldest = p;                           // names sort by timestamp
      f = root.openNextFile();
    }
    root.close();
    // Nothing to delete, or the delete itself failed (a stuck/locked file):
    // either way, retrying the same candidate forever would hang loop(), so
    // close the log and give up rather than loop.
    if (oldest.isEmpty() || !LittleFS.remove(oldest)) {
      if (logFile) { logFile.flush(); logFile.close(); }
      storage_full = true;
      if (oldest.isEmpty())
        Serial.println("[FS] storage full — log closed, points still accepted");
      else
        Serial.printf("[FS] remove failed for %s — log closed, points still accepted\n",
                       oldest.c_str());
      return;
    }
    Serial.printf("[FS] rolled off %s\n", oldest.c_str());
  }
  storage_full = false;
}

static void appendLogRow(const char* row, size_t len) {
  if (!logFile) return;
  logFile.write((const uint8_t*)row, len);
  rowCount++;
  if (rowCount % FLUSH_EVERY == 0) {
    logFile.flush();     // on power loss, at most 30 rows (2.5 min) are lost
    ensureFree();
  }
}

// Called once, from cmdTime(), when the clock becomes known: opens the log
// file named by boot time. The pre-sync ring itself is left untouched —
// logTick() drains it incrementally (a few dozen rows per LOG_MS tick) so a
// BT command handler is never blocked writing hundreds of rows synchronously.
static void openBootLog() {
  openLogAt(boot_epoch);
}

// Writes up to maxRows of the oldest buffered pre-sync rows (stamped with
// their real time, boot_epoch + up_s) and advances the ring by what was
// actually written. Consuming from the oldest end means only pre_len shrinks;
// pre_head (the next write slot) is untouched. If ensureFree() closes the log
// mid-drain (storage full), the loop stops and only the rows actually written
// are dropped from the ring — nothing is skipped.
static void drainPreSync(int maxRows) {
  int p0 = pre_len;                   // fixed snapshot: indices below are relative to it
  int n = p0 < maxRows ? p0 : maxRows;
  char row[200];
  int written = 0;
  for (int i = 0; i < n; i++) {
    if (!logFile) break;
    int idx = (pre_head - p0 + written + PRE_N) % PRE_N;   // oldest-still-buffered row
    const PreRow& p = pre[idx];
    size_t len = buildCsvRowAt(row, sizeof row, "", false, boot_epoch + p.up_s, p.r);
    appendLogRow(row, len);
    written++;
  }
  pre_len -= written;
  if (pre_len == 0) Serial.println("[LOG] pre-sync drained");
}

// Every LOG_MS: before the clock is known, remember the row; after, write it.
static void logTick() {
  if (!time_synced) {
    PreRow& p = pre[pre_head];
    p.up_s = millis() / 1000; p.r = cur;
    pre_head = (pre_head + 1) % PRE_N;
    if (pre_len < PRE_N) pre_len++;
    return;
  }
  if (!logFile) {
    // Not storage_full: the log was closed for some other reason (e.g. the
    // very first tick after sync, or a transient open failure) — try to
    // reopen so logging recovers on its own instead of staying dark forever.
    if (!storage_full) openLogAt(time(nullptr));
    return;
  }
  if (pre_len > 0) drainPreSync(60);   // pre-sync rows always precede live rows —
                                        // drain into the boot-named file even if
                                        // boot and first sync straddle midnight
  char day[10];
  fmtDay(day, sizeof day);
  if (strcmp(day, log_day) != 0) {
    openLogAt(time(nullptr));                  // midnight roll
    if (recording) writeMarker(">REC");         // recording continues into the new file
  }
  char row[200];
  size_t len = buildCsvRow(row, sizeof row, log_tag_pending);
  log_tag_pending = false;
  appendLogRow(row, len);
}

// Record/Stop are markers in the running log, not file open/close.
static void writeMarker(const char* tag) {
  char row[200];
  size_t len = buildCsvRowAt(row, sizeof row, tag, false, time(nullptr), cur);
  appendLogRow(row, len);
  logFile.flush();
}

static bool startRecording() {
  if (recording || !fs_ok || !time_synced || !logFile) return false;
  writeMarker(">REC");
  rec_start = time(nullptr);
  recording = true;
  Serial.printf("[REC] start marker in %s\n", logName);
  return true;
}

static void stopRecording() {
  if (!recording) return;
  writeMarker("<REC");
  recording = false;
  Serial.printf("[REC] stop marker (%u rows so far)\n", rowCount);
}

// "Read now" → row in the day's file (all of the day's points together).
// Opened/closed on every call — calls are rare, robustness matters.
static bool writeMark() {
  if (!fs_ok || !time_synced) return false;
  // freeBytes() is a multiple of the 4096-byte block size, so a "< 4096"
  // check only ever trips at zero free blocks; check a few blocks early, and
  // before opening the file, so a fresh day file is never created empty.
  if (freeBytes() < 4 * 4096) { storage_full = true; return false; }
  char day[10];
  fmtDay(day, sizeof day);
  char name[32];
  snprintf(name, sizeof name, "/points_%s.csv", day);
  bool fresh = !LittleFS.exists(name);
  File f = LittleFS.open(name, FILE_APPEND);
  if (!f) return false;
  if (fresh && f.print(CSV_HEADER) != sizeof(CSV_HEADER) - 1) {
    f.close(); storage_full = true; return false;
  }
  char row[200];
  size_t len = buildCsvRow(row, sizeof row, true);
  if (f.write((const uint8_t*)row, len) != len) {
    f.close(); storage_full = true; return false;
  }
  f.close();
  if (logFile) log_tag_pending = true;     // the point also appears in the continuous log
  Serial.printf("[POINT] %s -> %s\n", station, name);
  return true;
}

// ── History for chart backfill (10 min × 1 Hz) ─────────────
#define HIST_N  600
#define MARKS_N 24
struct Sample {
  uint32_t ep;
  float T, H, P, lux;
  uint16_t co2;
  bool bme, scd, veml;
};
static Sample hist[HIST_N];
static int hist_head = 0, hist_len = 0;

struct MarkEntry { uint32_t ep; char st[16]; };
static MarkEntry hmarks[MARKS_N];
static int hmarks_len = 0;

static void pushSample() {
  if (!time_synced) return;   // without a real clock, the points wouldn't make sense
  Sample& s = hist[hist_head];
  s.ep = (uint32_t)time(nullptr);
  s.T = cur.T; s.H = cur.H; s.P = cur.P; s.lux = cur.lux; s.co2 = cur.co2;
  s.bme = cur.bme_valid; s.scd = cur.scd_valid; s.veml = cur.veml_valid;
  hist_head = (hist_head + 1) % HIST_N;
  if (hist_len < HIST_N) hist_len++;
}

static void addMarkToHistory() {
  if (!time_synced) return;
  if (hmarks_len == MARKS_N) {
    memmove(hmarks, hmarks + 1, sizeof(MarkEntry) * (MARKS_N - 1));
    hmarks_len--;
  }
  hmarks[hmarks_len].ep = (uint32_t)time(nullptr);
  strlcpy(hmarks[hmarks_len].st, station, sizeof hmarks[hmarks_len].st);
  hmarks_len++;
}

static void appendNum(String& o, float v, int dec, bool valid) {
  if (!valid || isnan(v)) { o += "null"; return; }
  char b[20];
  snprintf(b, sizeof b, "%.*f", dec, v);
  o += b;
}

static void btSendLine(const String& s) {
  SerialBT.print(s);
  SerialBT.print('\n');
}

static void btErr(int status, const char* err) {
  char b[96];
  snprintf(b, sizeof b, "{\"ok\":false,\"status\":%d,\"err\":\"%s\"}", status, err);
  SerialBT.print(b);
  SerialBT.print('\n');
}

static String liveJson() {
  char d[12] = "", t2[10] = "--:--:--";
  if (time_synced) { fmtDate(d, sizeof d); fmtTime(t2, sizeof t2); }
  String o;
  o.reserve(900);
  o += "{\"synced\":";  o += time_synced ? "true" : "false";
  o += ",\"rec\":";     o += recording ? "true" : "false";
  o += ",\"full\":";    o += storage_full ? "true" : "false";
  o += ",\"rec_s\":";   o += recording ? String((uint32_t)(time(nullptr) - rec_start)) : String(0);
  o += ",\"free_kb\":"; o += String(freeBytes() / 1024);
  o += ",\"rssi\":0";   // SPP has no per-client RSSI; 0 is the firmware's existing "nothing to report" value
  o += ",\"ep\":";      o += String((uint32_t)time(nullptr));
  o += ",\"boot\":";    o += String((uint32_t)boot_epoch);
  o += ",\"date\":\"";  o += d;  o += "\",\"time\":\"";  o += t2; o += "\"";
  o += ",\"station\":\""; o += station; o += "\",\"lat\":\""; o += lat_s;
  o += "\",\"lon\":\"";   o += lon_s;   o += "\"";
  o += ",\"log\":\"";   o += recording ? logName : ""; o += "\"";
  o += ",\"T\":";     appendNum(o, cur.T,   2, cur.bme_valid);
  o += ",\"H\":";     appendNum(o, cur.H,   2, cur.bme_valid);
  o += ",\"P\":";     appendNum(o, cur.P,   2, cur.bme_valid);
  o += ",\"alt\":";   appendNum(o, cur.alt, 1, cur.bme_valid);
  o += ",\"co2\":";   if (cur.scd_valid) o += String(cur.co2); else o += "null";
  o += ",\"t_scd\":"; appendNum(o, cur.t_scd, 1, cur.scd_valid);
  o += ",\"h_scd\":"; appendNum(o, cur.h_scd, 1, cur.scd_valid);
  o += ",\"lux\":";   appendNum(o, cur.lux, 1, cur.veml_valid);
  o += ",\"bme\":";   o += bme_ok ? "true" : "false";
  o += ",\"scd\":";   o += scd41_ok ? "true" : "false";
  o += ",\"veml\":";  o += veml_ok ? "true" : "false";
  // 0.4.0: optional sensors. Flags always present; values null when the
  // sensor is absent, warming up or (PM) blocked. "pm_obs" true = the
  // BMV080 sees something static in front of its laser.
  o += ",\"bh\":";    o += bh_ok ? "true" : "false";
  o += ",\"pm\":";    o += bmv_ok ? "true" : "false";
  o += ",\"pm_obs\":"; o += ext.pm_obs ? "true" : "false";
  o += ",\"pm1\":";   if (ext.pm_valid) o += String(ext.pm1);  else o += "null";
  o += ",\"pm25\":";  if (ext.pm_valid) o += String(ext.pm25); else o += "null";
  o += ",\"pm10\":";  if (ext.pm_valid) o += String(ext.pm10); else o += "null";
  o += ",\"sgp41\":"; o += sgp41_ok ? "true" : "false";
  o += ",\"voc\":";   if (ext.voc_valid) o += String(ext.voc); else o += "null";
  o += ",\"nox\":";   if (ext.nox_valid) o += String(ext.nox); else o += "null";
  o += ",\"sgp30\":"; o += sgp_ok ? "true" : "false";
  o += ",\"tvoc\":";  if (ext.sgp30_valid) o += String(ext.tvoc); else o += "null";
  o += ",\"eco2\":";  if (ext.sgp30_valid) o += String(ext.eco2); else o += "null";
  o += ",\"soil_ok\":"; o += soil_ok ? "true" : "false";
  o += ",\"soil\":";  if (ext.soil_valid) o += String(ext.soil); else o += "null";
  o += ",\"soil_t\":"; appendNum(o, ext.soil_t, 1, ext.soil_valid);
  o += ",\"fw\":\"" FW_VERSION "\",\"id\":\""; o += DEVICE_ID; o += "\"";
  o += "}";
  return o;
}

// Streams the history ring straight to the link in small pieces. The
// earlier version built the whole ~26 KB JSON in one String: once the ring
// grew past ~10 minutes the allocation failed next to the Bluetooth stack
// and the box answered with an empty line, so the Live charts went blank
// exactly when there was the most to show.
static void writeNum(char* b, size_t n, float v, int dec, bool valid) {
  if (!valid || isnan(v)) strlcpy(b, "null", n);
  else snprintf(b, n, "%.*f", dec, v);
}

static void sendHistory() {
  char t[20], h[20], pr[20], lx[20], line[128];
  SerialBT.print("{\"samples\":[");
  for (int i = 0; i < hist_len; i++) {
    const Sample& s = hist[(hist_head - hist_len + i + HIST_N) % HIST_N];
    writeNum(t, sizeof t, s.T, 2, s.bme);
    writeNum(h, sizeof h, s.H, 2, s.bme);
    writeNum(pr, sizeof pr, s.P, 2, s.bme);
    writeNum(lx, sizeof lx, s.lux, 1, s.veml);
    if (s.scd) snprintf(line, sizeof line, "%s[%lu,%s,%s,%s,%u,%s]", i ? "," : "", (unsigned long)s.ep, t, h, pr, s.co2, lx);
    else       snprintf(line, sizeof line, "%s[%lu,%s,%s,%s,null,%s]", i ? "," : "", (unsigned long)s.ep, t, h, pr, lx);
    SerialBT.print(line);
    if ((i & 31) == 31) delay(1);   // let the SPP TX task drain between bursts
  }
  SerialBT.print("],\"marks\":[");
  for (int i = 0; i < hmarks_len; i++) {
    snprintf(line, sizeof line, "%s[%lu,\"%s\"]", i ? "," : "", (unsigned long)hmarks[i].ep, hmarks[i].st);
    SerialBT.print(line);
  }
  SerialBT.print("]}\n");
}

static bool validCsvName(const String& f) {
  return f.length() > 5 && f[0] == '/' && f.indexOf("..") < 0 &&
         f.endsWith(".csv");
}

static String filesJson() {
  String o;
  o.reserve(1024);
  o += "{\"total_kb\":";
  o += String(LittleFS.totalBytes() / 1024);
  o += ",\"used_kb\":";
  o += String(LittleFS.usedBytes() / 1024);
  o += ",\"rec\":";
  o += recording ? "true" : "false";
  o += ",\"files\":[";
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  bool first = true;
  while (f) {
    if (!f.isDirectory()) {
      if (!first) o += ",";
      first = false;
      o += "[\"";
      o += f.path();
      o += "\",";
      o += String((uint32_t)f.size());
      o += "]";
    }
    f = root.openNextFile();
  }
  o += "]}";
  return o;
}

// Extracts "key":"value" (unescaping \" and \\) or "key":<bare number> from
// a flat one-line JSON command built by the app's BtProto — the only sender.
// Returns false when the key is absent.
static bool jsonField(const char* line, const char* key, char* out, size_t n) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char* p = strstr(line, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t i = 0;
  if (*p == '"') {
    p++;
    while (*p && *p != '"' && i + 1 < n) {
      if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
      out[i++] = *p++;
    }
  } else {
    while (*p && *p != ',' && *p != '}' && i + 1 < n) out[i++] = *p++;
  }
  out[i] = '\0';
  return true;
}

static void cmdTime(const char* line) {
  char ms_s[24] = "", tz_s[8] = "";
  jsonField(line, "epoch_ms", ms_s, sizeof ms_s);
  jsonField(line, "tz_min", tz_s, sizeof tz_s);
  long long ms = atoll(ms_s);
  long tz_min  = atol(tz_s);
  if (ms < 1000000000000LL)          { btErr(400, "bad epoch"); return; }
  if (tz_min < -840 || tz_min > 840) { btErr(400, "bad tz"); return; }
  // First sync names the log and stamps the pre-sync rows; a later sync is
  // acknowledged but ignored so timestamps in the open file stay monotonic.
  if (time_synced) { btSendLine("{\"ok\":true}"); return; }
  struct timeval tv = { (time_t)(ms / 1000) - tz_min * 60, 0 };
  settimeofday(&tv, nullptr);
  time_synced = true;
  boot_epoch = time(nullptr) - (time_t)(millis() / 1000);
  openBootLog();
  char d[12], t2[10];
  fmtDate(d, sizeof d); fmtTime(t2, sizeof t2);
  Serial.printf("[TIME] synced: %s %s\n", d, t2);
  btSendLine("{\"ok\":true}");
}

static void cmdStation(const char* line) {
  char s[24];
  if (jsonField(line, "s", s, sizeof s) && s[0]) {
    strlcpy(station, s, sizeof station);
    sanitize(station);
  }
  jsonField(line, "lat", lat_s, sizeof lat_s); sanitize(lat_s);
  jsonField(line, "lon", lon_s, sizeof lon_s); sanitize(lon_s);
  Serial.printf("[STATION] %s (%s,%s)\n", station, lat_s, lon_s);
  btSendLine("{\"ok\":true}");
}

static void cmdRecord(const char* line) {
  char a[8] = "";
  jsonField(line, "action", a, sizeof a);
  bool ok = false;
  if (!strcmp(a, "start"))     ok = startRecording();
  else if (!strcmp(a, "stop")) { stopRecording(); ok = true; }
  char resp[112];
  if (ok)
    snprintf(resp, sizeof resp, "{\"ok\":true,\"rec\":%s,\"synced\":%s,\"full\":%s}",
             recording ? "true" : "false", time_synced ? "true" : "false",
             storage_full ? "true" : "false");
  else
    snprintf(resp, sizeof resp,
             "{\"ok\":false,\"status\":409,\"rec\":%s,\"synced\":%s,\"full\":%s}",
             recording ? "true" : "false", time_synced ? "true" : "false",
             storage_full ? "true" : "false");
  btSendLine(String(resp));
}

static void cmdMark() {
  bool ok = writeMark();
  if (ok) addMarkToHistory();
  if (ok) btSendLine("{\"ok\":true}");
  else    btErr(409, "mark failed");
}

static void cmdDownload(const char* line) {
  char fn[48] = "";
  jsonField(line, "f", fn, sizeof fn);
  String f = String(fn);
  if (!validCsvName(f) || !LittleFS.exists(f)) { btErr(404, "not found"); return; }
  if (logFile && f == String(logName)) logFile.flush();   // the open log is readable after a flush
  File file = LittleFS.open(f, FILE_READ);
  if (!file) { btErr(500, "open failed"); return; }
  char hdr[48];
  snprintf(hdr, sizeof hdr, "{\"ok\":true,\"size\":%u}", (uint32_t)file.size());
  btSendLine(String(hdr));
  uint8_t buf[512];
  size_t n;
  while ((n = file.read(buf, sizeof buf)) > 0) SerialBT.write(buf, n);
  file.close();
}

static void cmdDelete(const char* line) {
  char fn[48] = "";
  jsonField(line, "f", fn, sizeof fn);
  String f = String(fn);
  if (logFile && f == String(logName)) {
    if (recording) { btErr(409, "log open"); return; }
    // Idle: the user explicitly asked for the open log — close it, remove
    // it, and open a fresh one so logging resumes without a gap.
    logFile.flush(); logFile.close();
    LittleFS.remove(f);
    openLogAt(time(nullptr));
    btSendLine("{\"ok\":true}");
    return;
  }
  if (validCsvName(f) && LittleFS.exists(f) && LittleFS.remove(f)) {
    if (storage_full && freeBytes() >= MIN_FREE_B) { storage_full = false; if (time_synced && !logFile) openLogAt(time(nullptr)); }
    btSendLine("{\"ok\":true}");
  } else {
    btErr(404, "not found");
  }
}

static void cmdDeleteAll() {
  if (recording) { btErr(409, "recording active"); return; }
  int deleted = 0;
  bool more = true;
  while (more) {
    more = false;
    String names[40];
    int cnt = 0;
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while (f && cnt < 40) {
      String path = f.path();
      // A closed (storage-full) log is just a name and is deletable too;
      // only the actively-open handle is protected.
      if (!f.isDirectory() && path.endsWith(".csv")
          && !(logFile && path == String(logName)))
        names[cnt++] = path;
      f = root.openNextFile();
    }
    root.close();
    int removed_this_pass = 0;
    for (int i = 0; i < cnt; i++)
      if (LittleFS.remove(names[i])) { deleted++; removed_this_pass++; }
    if (cnt == 40 && removed_this_pass > 0) more = true;
  }
  if (storage_full && freeBytes() >= MIN_FREE_B) { storage_full = false; if (time_synced && !logFile) openLogAt(time(nullptr)); }
  char resp[48];
  snprintf(resp, sizeof resp, "{\"ok\":true,\"deleted\":%d}", deleted);
  Serial.printf("[FS] deleted %d files (new day)\n", deleted);
  btSendLine(String(resp));
}

static unsigned long t_slow = 0, t_scd_read = 0, t_log = 0;
static unsigned long t_scd_ok = 0;   // last successful readMeasurement

// ── Status LED ────────────────────────────────────────────────
enum LedMode { LED_OFF, LED_ON, LED_BLINK_1HZ, LED_BLINK_FAST };
static LedMode led_mode = LED_OFF;

static void ledLoop(unsigned long now) {
  static unsigned long t = 0;
  static bool on = false;
  if (led_mode == LED_OFF) { digitalWrite(LED_PIN, LOW);  return; }
  if (led_mode == LED_ON)  { digitalWrite(LED_PIN, HIGH); return; }
  unsigned long period = (led_mode == LED_BLINK_1HZ) ? 500 : 100;
  if (now - t >= period) { t = now; on = !on; digitalWrite(LED_PIN, on); }
}

// ── I2C bus stuck recovery (9-clock method, from fw 2.2.4) ─────
// SCD41 in clock-stretching can hold SDA low across a soft reset;
// we pulse SCL manually until SDA releases, then generate a STOP.
static void i2cBusUnstickPins(uint8_t sda, uint8_t scl) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(scl, HIGH); delayMicroseconds(5);
    digitalWrite(scl, LOW);  delayMicroseconds(5);
    if (digitalRead(sda)) break;
  }
  pinMode(sda, OUTPUT);
  digitalWrite(sda, LOW);  delayMicroseconds(5);
  digitalWrite(scl, HIGH); delayMicroseconds(5);
  digitalWrite(sda, HIGH); delayMicroseconds(5);
}
static void i2cBusUnstick() { i2cBusUnstickPins(21, 22); }

// BMV080 wired with SDA and SCL crossed (EASE 2.4.3): the bus is turned
// round for each driver call and back; the STOP on the way back clears
// anything the other sensors took for a start.
static void bmvBus(bool crossed) {
  Wire.end();
  if (crossed) { i2cBusUnstickPins(22, 21); Wire.begin(22, 21); }
  else         { i2cBusUnstickPins(21, 22); Wire.begin(21, 22); }
  Wire.setClock(100000);
}
static uint8_t bmvProbe() {
  for (uint8_t a : BMV_ADDRS) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return a;
  }
  return 0;
}

// SGP41: plain Wire, Sensirion framing (word + CRC-8, poly 0x31, init 0xFF).
static uint8_t sgp41Crc(uint16_t w) {
  uint8_t crc = 0xFF, b[2] = { (uint8_t)(w >> 8), (uint8_t)w };
  for (uint8_t i = 0; i < 2; i++) {
    crc ^= b[i];
    for (uint8_t k = 0; k < 8; k++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}
static bool sgp41Send(uint16_t cmd, const uint16_t* args, uint8_t n) {
  Wire.beginTransmission(SGP41_ADDR);
  Wire.write((uint8_t)(cmd >> 8)); Wire.write((uint8_t)cmd);
  for (uint8_t i = 0; i < n; i++) {
    Wire.write((uint8_t)(args[i] >> 8)); Wire.write((uint8_t)args[i]);
    Wire.write(sgp41Crc(args[i]));
  }
  return Wire.endTransmission() == 0;
}
static bool sgp41Read(uint16_t* out, uint8_t n) {
  uint8_t len = n * 3;
  if (Wire.requestFrom((uint8_t)SGP41_ADDR, len, (uint8_t)true) != len) return false;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t hi = Wire.read(), lo = Wire.read(), c = Wire.read();
    out[i] = (uint16_t)(hi << 8 | lo);
    if (sgp41Crc(out[i]) != c) return false;
  }
  return true;
}
// T/RH ticks for compensation; 50 %RH / 25 °C when no humidity sensor.
static void sgp41Comp(float t, float h, uint16_t comp[2]) {
  if (isnan(t) || isnan(h)) { comp[0] = 0x8000; comp[1] = 0x6666; return; }
  h = constrain(h, 0.0f, 100.0f);
  t = constrain(t, -45.0f, 130.0f);
  comp[0] = (uint16_t)lroundf(h * 65535.0f / 100.0f);
  comp[1] = (uint16_t)lroundf((t + 45.0f) * 65535.0f / 175.0f);
}

// ── VEML7700: non-blocking auto-range (from MagicBox EASE 2.4.4) ──
// With the fixed gain 1 / 100 ms of 0.3.x the lux saturated at ~3.8 klx,
// i.e. in any daylight outdoors. This ladder walks gain and integration
// time from loop() with short register reads only (the library's own auto
// mode calls delay() for every step, up to seconds in the dark, which would
// stall the Bluetooth link). Resolution in lux/count =
// 0.0036 · (800 ms / IT) · (2 / gain), Vishay app note 84323.
struct VemlRung { uint8_t gain; uint8_t it; uint16_t it_ms; float res; };
static const VemlRung VEML_LADDER[] = {
  { VEML7700_GAIN_1_8, VEML7700_IT_25MS,   25, 1.8432f },  // direct sun
  { VEML7700_GAIN_1_8, VEML7700_IT_50MS,   50, 0.9216f },
  { VEML7700_GAIN_1_8, VEML7700_IT_100MS, 100, 0.4608f },
  { VEML7700_GAIN_1_4, VEML7700_IT_100MS, 100, 0.2304f },
  { VEML7700_GAIN_1,   VEML7700_IT_100MS, 100, 0.0576f },  // = fixed setting of 0.3.x
  { VEML7700_GAIN_2,   VEML7700_IT_100MS, 100, 0.0288f },
  { VEML7700_GAIN_2,   VEML7700_IT_200MS, 200, 0.0144f },
  { VEML7700_GAIN_2,   VEML7700_IT_400MS, 400, 0.0072f },
  { VEML7700_GAIN_2,   VEML7700_IT_800MS, 800, 0.0036f },  // near darkness
};
#define VEML_RUNGS    (sizeof(VEML_LADDER) / sizeof(VEML_LADDER[0]))
#define VEML_START    4
#define VEML_HIGH     50000      // counts: above → one rung less sensitive
#define VEML_SAT      65000      // saturated → two rungs at once
#define VEML_LOW      1000       // below → more sensitive
#define VEML_AIM      20000      // counts to aim for when stepping up
#define VEML_LUX_MAX  150000.0f
static uint8_t       veml_rung = VEML_START;
static unsigned long veml_t_ready = 0, c_veml_at = 0;
static float         c_veml_lux = 0;
static bool          c_veml_ok = false;

static void vemlSetRung(uint8_t rung, unsigned long now) {
  uint16_t old_ms = VEML_LADDER[veml_rung].it_ms;
  veml_rung = rung;
  veml.setGain(VEML_LADDER[rung].gain);
  veml.setIntegrationTime(VEML_LADDER[rung].it, false);   // false: no delay()
  veml_t_ready = now + old_ms + 2UL * VEML_LADDER[rung].it_ms;
}

// Vishay non-linearity correction, trusted up to 10 klx; above, its tangent
// (the quartic explodes past ~20 klx). Same as MagicBox 2.2.5+.
#define VEML_POLY_KNEE 10000.0f
static float vemlPoly(float lux) {
  return (((6.0135e-13f * lux - 9.3924e-9f) * lux + 8.1488e-5f) * lux + 1.0023f) * lux;
}
static float vemlCorrect(float lux) {
  if (lux <= VEML_POLY_KNEE) return vemlPoly(lux);
  const float k = VEML_POLY_KNEE;
  const float slope = ((4 * 6.0135e-13f * k - 3 * 9.3924e-9f) * k + 2 * 8.1488e-5f) * k + 1.0023f;
  return vemlPoly(k) + slope * (lux - k);
}

static void vemlService(unsigned long now) {
  if ((long)(now - veml_t_ready) < 0) return;
  uint16_t als = veml.readALS(false);
  const VemlRung& r = VEML_LADDER[veml_rung];
  if (als > VEML_HIGH && veml_rung > 0) {
    uint8_t down = (als >= VEML_SAT && veml_rung >= 2) ? 2 : 1;
    vemlSetRung(veml_rung - down, now);
    return;
  }
  if (als < VEML_LOW && veml_rung < VEML_RUNGS - 1) {
    float lin = als * r.res;
    uint8_t up = veml_rung + 1;
    while (up + 1 < VEML_RUNGS && lin / VEML_LADDER[up + 1].res <= VEML_AIM) up++;
    vemlSetRung(up, now);
    return;
  }
  float lux = vemlCorrect(als * r.res);
  c_veml_lux = lux > VEML_LUX_MAX ? VEML_LUX_MAX : lux;
  c_veml_ok = true;
  c_veml_at = now;
  veml_t_ready = now + r.it_ms;
}

static void sensorsInit() {
  i2cBusUnstick();          // safety: release a bus left stuck
  Wire.begin(21, 22);
  Wire.setClock(100000);    // SCD41 supports max 100 kHz

  veml_ok = veml.begin();
  for (int r = 0; !veml_ok && r < 2; r++) { delay(50); veml_ok = veml.begin(); }
  if (veml_ok) {
    vemlSetRung(VEML_START, millis());
  }
  Serial.printf("[VEML7700] %s\n", veml_ok ? "OK" : "FAIL");

  bme_ok = bme.begin(0x76) || bme.begin(0x77);
  for (int r = 0; !bme_ok && r < 2; r++) {
    delay(100);
    bme_ok = bme.begin(0x76) || bme.begin(0x77);
  }
  if (bme_ok)
    bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::FILTER_X2,
                    Adafruit_BME280::STANDBY_MS_1000);
  Serial.printf("[BME280] %s\n", bme_ok ? "OK" : "FAIL");

  scd41_ok = scd41.begin(Wire);
  if (scd41_ok) scd41.startPeriodicMeasurement();
  Serial.printf("[SCD41] %s\n", scd41_ok ? "OK" : "FAIL");
  optionalInit();
}

// 0.4.0: the EASE sensors, each probed once; "not fitted" never blocks.
static void optionalInit() {
  if (!veml_ok) {
    for (uint8_t a : {(uint8_t)0x23, (uint8_t)0x5C}) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0 && bh.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, a, &Wire)) {
        bh_ok = true; break;
      }
    }
    Serial.printf("[BH1750] %s\n", bh_ok ? "OK" : "not fitted");
  }

  // A quick probe first: seesaw.begin() spends ~100 ms per silent address.
  for (uint8_t a : SOIL_ADDRS) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0) continue;
    if (soil.begin(a)) { soil_ok = true; Serial.printf("[SOIL] OK at 0x%02X\n", a); break; }
  }
  if (!soil_ok) Serial.println("[SOIL] not fitted");

  Wire.beginTransmission(0x58);
  if (Wire.endTransmission() == 0) sgp_ok = sgp.begin(&Wire);
  if (sgp_ok) {
    t_sgp_start = millis();
    Preferences hw;
    hw.begin(HW_NAMESPACE, true);
    uint16_t be = hw.getUShort("sgp_eco2", 0), bt = hw.getUShort("sgp_tvoc", 0);
    hw.end();
    bool restored = be && bt && sgp.setIAQBaseline(be, bt);
    Serial.printf("[SGP30] OK%s\n", restored ? ", baseline restored" : ", no baseline yet");
  } else {
    Serial.println("[SGP30] not fitted");
  }

  {
    uint16_t sn[3], tr = 0;
    bool found = sgp41Send(SGP41_CMD_SERIAL, nullptr, 0);
    if (found) { delay(2); found = sgp41Read(sn, 3); }
    if (found) {
      bool test = sgp41Send(SGP41_CMD_TEST, nullptr, 0);
      if (test) { delay(330); test = sgp41Read(&tr, 1); }
      sgp41_ok = test && (tr & 0x0F) == 0;
      Serial.printf("[SGP41] %s\n", sgp41_ok ? "OK, self-test passed" : "self-test FAILED: SGP41 off");
    } else {
      Serial.println("[SGP41] not fitted");
    }
  }

  bmv_addr = bmvProbe();
  if (!bmv_addr) {                       // a crossed cable answers only this way
    bmvBus(true);
    bmv_addr = bmvProbe();
    bmv_swapped = bmv_addr != 0;
    if (!bmv_swapped) bmvBus(false);
  }
  if (bmv_addr) {
    bmv = new DFRobot_BMV080_I2C(&Wire, bmv_addr);
    uint16_t st = bmv->openBmv080();
    if (st == 0) {
      bmv->setObstructionDetection(true);
      bmv->setDoVibrationFiltering(true);       // the box rides in a backpack
      bmv->setMeasurementAlgorithm(BALANCED);
      bmv_ok = bmv->setBmv080Mode(CONTINUOUS_MODE) == 0;
    }
    if (!bmv_ok) Serial.printf("[BMV080] open/start failed at 0x%02X\n", bmv_addr);
  }
  if (bmv_swapped) bmvBus(false);
  if (bmv_ok) {
    t_bmv_start = t_bmv_data = millis();
    Serial.printf("[BMV080] OK at 0x%02X%s\n", bmv_addr, bmv_swapped ? " (SDA/SCL swapped)" : "");
  } else if (!bmv_addr) {
    Serial.println("[BMV080] not fitted");
  }
}

static void readSlowSensors() {
  if (bme_ok) {
    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure() / 100.0f;
    if (t > -40 && t < 85 && h >= 0 && h <= 110 && p > 300 && p < 1100) {
      // 2 decimals: BME280 resolves 0.01° — with 1 decimal the chart shows steps
      cur.T   = roundf(t * 100) / 100.0f;
      cur.H   = roundf(min(h, 100.0f) * 100) / 100.0f;
      cur.P   = roundf(p * 100) / 100.0f;
      cur.alt = roundf(bme.readAltitude(1013.25f) * 10) / 10.0f;
      cur.bme_valid = true;
    } else {
      cur.bme_valid = false;
    }
  }
  if (veml_ok) {
    // Auto-ranged in vemlService(); here only the last valid result, kept
    // at most 3 s (a sensor that stops answering leaves the cell empty).
    if (c_veml_ok && millis() - c_veml_at <= 3000) {
      cur.lux = roundf(c_veml_lux * 10) / 10.0f;
      cur.veml_valid = true;
    } else {
      cur.veml_valid = false;
    }
  } else if (bh_ok) {
    // Same lux column: a box has one light sensor or the other.
    float lx = bh.readLightLevel();
    if (lx < 0 || lx > 120000) cur.veml_valid = false;
    else { cur.lux = roundf(lx * 10) / 10.0f; cur.veml_valid = true; }
  }
  readOptional1Hz();
}

// The 1 Hz part of the optional sensors (SGP30 and SGP41 want exactly one
// sample per second; soil is cheap). A sensor that fails 10 s in a row is
// switched off until the next reset.
static void readOptional1Hz() {
  unsigned long now = millis();
  float ht = cur.bme_valid ? cur.T : (cur.scd_valid ? cur.t_scd : NAN);
  float hh = cur.bme_valid ? cur.H : (cur.scd_valid ? cur.h_scd : NAN);

  if (soil_ok) {
    static uint8_t soil_fail = 0;
    uint16_t c = soil.touchRead(0);
    if (c != 65535 && c < 4096) {
      soil_fail = 0;
      ext.soil = c;
      float st = soil.getTemp();
      ext.soil_t = (st > -40 && st < 85) ? roundf(st * 10) / 10.0f : NAN;
      ext.soil_valid = true;
    } else {
      ext.soil_valid = false;
      if (++soil_fail >= 10) { soil_ok = false; Serial.println("[SOIL] no answer 10 s: off"); }
    }
  }

  if (sgp_ok) {
    if ((now - t_sgp_hum >= SGP_HUM_MS || t_sgp_hum == 0) && !isnan(ht)) {
      float ah = 216.7f * (hh / 100.0f * 6.112f * expf(17.62f * ht / (243.12f + ht)) / (273.15f + ht));
      sgp.setHumidity((uint32_t)(ah * 1000.0f));
      t_sgp_hum = now;
    }
    if (sgp.IAQmeasure()) {
      sgp_fail = 0;
      ext.sgp30_valid = now - t_sgp_start >= SGP_WARMUP_MS;
      ext.tvoc = sgp.TVOC; ext.eco2 = sgp.eCO2;
      unsigned long run = now - t_sgp_start;
      if (run >= SGP_BASE_FIRST_MS && (t_sgp_base == 0 || now - t_sgp_base >= SGP_BASE_EVERY_MS)) {
        uint16_t be, bt;
        if (sgp.getIAQBaseline(&be, &bt)) {
          Preferences hw;
          hw.begin(HW_NAMESPACE, false);
          hw.putUShort("sgp_eco2", be); hw.putUShort("sgp_tvoc", bt);
          hw.end();
        }
        t_sgp_base = now;
      }
    } else {
      ext.sgp30_valid = false;
      if (++sgp_fail >= 10) { sgp_ok = false; Serial.println("[SGP30] no answer 10 s: off"); }
    }
  }

  if (sgp41_ok) {
    // Read last second's result (ready after 50 ms), then send the next
    // command: nothing waits. Index 0 = algorithm still starting (~45 s).
    if (sgp41_pending) {
      uint16_t raw[2];
      if (sgp41Read(raw, sgp41_pending == 2 ? 2 : 1)) {
        sgp41_fail = 0;
        int32_t vi = voc_algo.process(raw[0]);
        if (sgp41_pending == 2) {
          int32_t ni = nox_algo.process(raw[1]);
          ext.voc = vi; ext.voc_valid = vi > 0;
          ext.nox = ni; ext.nox_valid = ni > 0;
        }
      } else if (++sgp41_fail >= 10) {
        sgp41_ok = false; ext.voc_valid = ext.nox_valid = false;
        Serial.println("[SGP41] no answer 10 s: off");
      }
    }
    if (sgp41_ok) {
      uint16_t comp[2];
      sgp41Comp(ht, hh, comp);
      bool cond = sgp41_cond > 0;
      if (sgp41Send(cond ? SGP41_CMD_COND : SGP41_CMD_MEAS, comp, 2)) {
        sgp41_pending = cond ? 1 : 2;
        if (cond) sgp41_cond--;
      } else {
        sgp41_pending = 0;
        if (++sgp41_fail >= 10) { sgp41_ok = false; ext.voc_valid = ext.nox_valid = false; }
      }
    }
  }

  // PM: the cached driver result, sent only after warm-up and while fresh.
  bool fresh = bmv_ok && now - t_bmv_start >= BMV_WARMUP_MS && now - t_bmv_data <= BMV_STALE_MS;
  ext.pm_obs   = fresh && c_bmv.obstructed;
  ext.pm_valid = fresh && !c_bmv.obstructed;
  if (ext.pm_valid) {
    ext.pm1 = (int)lroundf(c_bmv.pm1); ext.pm25 = (int)lroundf(c_bmv.pm25); ext.pm10 = (int)lroundf(c_bmv.pm10);
  }
}

// BMV080 driver served every 200 ms (it reads its FIFO here, 100-200 ms).
static void serveBmv() {
  unsigned long now = millis();
  if (!bmv_ok || now - t_bmv_serve < BMV_SERVE_MS) return;
  t_bmv_serve = now;
  float p1, p25, p10;
  if (bmv_swapped) bmvBus(true);
  bool got = bmv->getBmv080Data(&p1, &p25, &p10);
  bool obs = got && bmv->ifObstructed();
  if (bmv_swapped) bmvBus(false);
  if (got) {
    t_bmv_data = now;
    if (obs != c_bmv.obstructed)
      Serial.println(obs ? "[BMV080] something static in front of the laser" : "[BMV080] view clear again");
    c_bmv = { p1, p25, p10, obs };
  } else if (now - t_bmv_data >= BMV_DEAD_MS) {
    bmv_ok = false;
    Serial.println("[BMV080] no result for 30 s: PM sensor off");
  }
}

// SCD41 delivers every ~5 s; between reads the last valid value is kept
// (repeated in the log — behavior documented in the spec).
static void readScd() {
  if (scd41_ok && scd41.readMeasurement()) {
    cur.co2   = scd41.getCO2();
    cur.t_scd = roundf(scd41.getTemperature() * 10) / 10.0f;
    cur.h_scd = roundf(scd41.getHumidity() * 10) / 10.0f;
    cur.scd_valid = true;
    t_scd_ok = millis();
  }
}

static void handleCommand(const char* line) {
  char cmd[16] = "";
  if (!jsonField(line, "cmd", cmd, sizeof cmd)) { btErr(400, "bad_cmd"); return; }
  if      (!strcmp(cmd, "live"))       btSendLine(liveJson());
  else if (!strcmp(cmd, "history"))    sendHistory();
  else if (!strcmp(cmd, "files"))      btSendLine(filesJson());
  else if (!strcmp(cmd, "time"))       cmdTime(line);
  else if (!strcmp(cmd, "station"))    cmdStation(line);
  else if (!strcmp(cmd, "mark"))       cmdMark();
  else if (!strcmp(cmd, "record"))     cmdRecord(line);
  else if (!strcmp(cmd, "download"))   cmdDownload(line);
  else if (!strcmp(cmd, "delete"))     cmdDelete(line);
  else if (!strcmp(cmd, "delete_all")) cmdDeleteAll();
  else btErr(400, "bad_cmd");
}

// Non-blocking line assembly: the 1 Hz sensor/logging beat must never wait
// on the radio. An oversized line (no sender we know builds one) is dropped.
static void btPump() {
  while (SerialBT.available()) {
    char c = (char)SerialBT.read();
    if (c == '\n') {
      btLine[btLen] = '\0';
      if (btLen) handleCommand(btLine);
      btLen = 0;
    } else if (c != '\r') {
      if (btLen < sizeof btLine - 1) btLine[btLen++] = c;
      else btLen = 0;
    }
  }
}

static bool sensors_ok = false;
static unsigned long boot_ms = 0;   // 0 until setup() finishes

// ────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  snprintf(DEVICE_ID, sizeof(DEVICE_ID), BT_PREFIX "%04X",
           (uint16_t)(ESP.getEfuseMac() >> 32));
  Serial.printf("[BOOT] Field Box %s fw v%s (%s)\n", DEVICE_ID, FW_VERSION, __DATE__);
  // tells us instantly whether a "dead box" in the field was a brownout, a crash,
  // or a normal power-up — no more guessing after the fact
  static const char* RST[] = {"unknown", "power-on", "external", "software",
                              "panic/crash", "interrupt wdt", "task wdt", "other wdt",
                              "deep sleep", "BROWNOUT (weak power!)", "SDIO"};
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[BOOT] reset reason: %s\n",
                rr < (sizeof RST / sizeof *RST) ? RST[rr] : "?");
  pinMode(LED_PIN, OUTPUT);

  // On air ~300 ms after boot, same priority the WiFi AP had: the radio
  // comes up before the sensors and the filesystem.
  // +9 dBm, the chip's maximum, up from the +3 dBm default (esp_bt.h). The
  // box rides in a backpack with a body between it and the phone, and a
  // torso costs 20 dB at 2.4 GHz; every dB towards the phone counts and
  // the 10 h battery does not notice. Only the box→phone direction gains —
  // the phone transmits at whatever it likes.
  //
  // Ordering is the whole trick: esp_bt.h wants the level set after the
  // controller is enabled and BEFORE profile initialisation. Set after
  // SerialBT.begin() it is silently ignored (the read-back stayed at the
  // default), so the controller is started here first; begin() sees
  // btStarted() and skips its own start. The read-back proves it in the
  // boot log: max=7 is ESP_PWR_LVL_P9.
  // 0.4.0: the box speaks BT Classic (SPP) only, never BLE. Releasing the
  // BLE half of the controller before it starts gives that RAM back to the
  // heap; with the BMV080's 60 KB loop stack the first bench run had only
  // 8 KB free and crashed in a 2 s loop (25.10.2026).
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  btStart();
  esp_err_t pw = esp_bredr_tx_power_set(ESP_PWR_LVL_P9, ESP_PWR_LVL_P9);
  SerialBT.begin(DEVICE_ID);   // SPP, discoverable + connectable
  Serial.printf("[BT] \"%s\" SPP up — pair once, then connect from the app\n", DEVICE_ID);
  {
    esp_power_level_t lo = ESP_PWR_LVL_N0, hi = ESP_PWR_LVL_N0;
    esp_bredr_tx_power_get(&lo, &hi);
    Serial.printf("[BT] tx power set rc=%d, levels min=%d max=%d (7 = +9 dBm)\n",
                  (int)pw, (int)lo, (int)hi);
  }

  sensorsInit();
  sensors_ok = bme_ok || scd41_ok || veml_ok || bh_ok || bmv_ok || sgp41_ok || sgp_ok || soil_ok;
  // 0.4.1: same summary as the MagicBox firmware, so the Serial Monitor shows
  // which box this is and what it found, without the app.
  {
    String l;
    if (bme_ok)   l += "BME280,";
    if (scd41_ok) l += "SCD41,";
    if (veml_ok)  l += "VEML7700,";
    if (bh_ok)    l += "BH1750,";
    if (bmv_ok)   l += "BMV080,";
    if (sgp41_ok) l += "SGP41,";
    if (sgp_ok)   l += "SGP30,";
    if (soil_ok)  l += "SOIL,";
    if (l.length()) l.remove(l.length() - 1);
    Serial.printf("DEVICE: %s\nTYPE: FIELDBOX\nSENSORS: %s\nFW: %s\n",
                  DEVICE_ID, l.length() ? l.c_str() : "NONE", FW_VERSION);
  }

  // begin(false), twice, then format: begin(true)'s format-on-fail meant a
  // single transient mount hiccup (brownout at exactly the wrong read) could
  // silently erase every CSV on the box. A retry absorbs the transient case;
  // a mount that fails twice in a row means the filesystem is genuinely
  // unusable — or was never created (first boot) — and only then is
  // formatting the right recovery, same end state the old code reached on a
  // first boot, minus the data-loss path.
  fs_ok = LittleFS.begin(false);
  if (!fs_ok) { delay(100); fs_ok = LittleFS.begin(false); }
  if (!fs_ok) {
    Serial.println("[FS] mount failed twice — formatting");
    fs_ok = LittleFS.format() && LittleFS.begin(false);
  }
  Serial.printf("[FS] %s — free %u KB\n", fs_ok ? "OK" : "FAIL",
                freeBytes() / 1024);

  boot_ms = millis();   // starts the 2 s "sensors OK" LED, without blocking
  // 0.4.0: BT Classic + the BMV080's 60 KB loop stack share one heap; the
  // first bench run must show what is left (expected order: 60-100 KB).
  Serial.printf("[MEM] free heap %u B, largest block %u B, min ever %u B, loop stack free %u B\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap(),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

// 0.4.1: the optional EASE sensors on Serial too (0.4.0 sent them only to the
// app, so the Serial Monitor showed T/H/P/CO2/lux and nothing else). One line
// per second, only the sensors found at boot; "--" = fitted but no value yet
// (warm-up, SGP41 conditioning, PM blocked or still starting).
static void printExtraLine() {
  if (!(bmv_ok || sgp41_ok || sgp_ok || soil_ok || bh_ok)) return;
  char b[200]; size_t n = 0;
  n += snprintf(b + n, sizeof b - n, "[1Hz+]");
  if (bmv_ok) {
    if (ext.pm_valid) n += snprintf(b + n, sizeof b - n, " PM1=%d PM2.5=%d PM10=%d", ext.pm1, ext.pm25, ext.pm10);
    else n += snprintf(b + n, sizeof b - n, " PM=%s", ext.pm_obs ? "BLOCKED" : "--");
  }
  if (sgp41_ok) {
    n += snprintf(b + n, sizeof b - n, ext.voc_valid ? " VOC=%d" : " VOC=--", (int)ext.voc);
    n += snprintf(b + n, sizeof b - n, ext.nox_valid ? " NOx=%d" : " NOx=--", (int)ext.nox);
  }
  if (sgp_ok) {
    if (ext.sgp30_valid) n += snprintf(b + n, sizeof b - n, " TVOC=%u eCO2=%u", (unsigned)ext.tvoc, (unsigned)ext.eco2);
    else n += snprintf(b + n, sizeof b - n, " TVOC=-- (warm-up)");
  }
  if (soil_ok) {
    if (ext.soil_valid) n += snprintf(b + n, sizeof b - n, " SOIL=%d SOIL_T=%.1f", (int)ext.soil, ext.soil_t);
    else n += snprintf(b + n, sizeof b - n, " SOIL=--");
  }
  if (bh_ok) n += snprintf(b + n, sizeof b - n, " (lux from BH1750)");
  Serial.println(b);
}

void loop() {
  btPump();
  unsigned long now = millis();

  if (scd41_ok && now - t_scd_read >= SCD_MS) { t_scd_read = now; readScd(); }
  serveBmv();
  if (veml_ok) vemlService(millis());

  // SCD41 dying mid-walk (loose wire/brownout): no successful read for 15 s →
  // empty CSV cells instead of the last value repeated for hours.
  // Fresh millis(), NOT `now`: t_scd_ok is set AFTER `now` was read in the
  // same pass, and `now - t_scd_ok` underflowed unsigned, invalidating the
  // reading in the very second it succeeded.
  if (cur.scd_valid && millis() - t_scd_ok > 15000) cur.scd_valid = false;

  if (now - t_slow >= SLOW_MS) {
    t_slow = now;
    readSlowSensors();
    Serial.printf("[1Hz] T=%.2f H=%.2f P=%.2f CO2=%u(%s age=%lus) lux=%.1f\n",
                  cur.T, cur.H, cur.P, cur.co2,
                  cur.scd_valid ? "valid" : "INVALID",
                  (unsigned long)((millis() - t_scd_ok) / 1000), cur.lux);
    printExtraLine();
  }

  if (now - t_log >= LOG_MS) {
    t_log = now;
    logTick();
    pushSample();          // history ring now spans 600 × 5 s = 50 min
  }

  // The boot confirmation (solid 2 s = sensors OK) used to be a delay(2000) in
  // setup(), which froze everything. Same behaviour for the user, nothing blocked.
  bool booting = sensors_ok && boot_ms && (now - boot_ms < 2000);
  led_mode = booting        ? LED_ON
           : storage_full   ? LED_BLINK_FAST
           : recording      ? LED_BLINK_1HZ
                            : LED_OFF;

  ledLoop(now);
}
