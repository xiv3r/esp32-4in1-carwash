// ======================================================
// ESP32 38Pin 4IN1 CARWASH SYSTEM 
// ======================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <new>

// ======================================================
// BUILD CONFIGURATION
// ======================================================

#define FW_NAME                 "Carwash 4-in-1 Controller"
#define FW_VERSION              "1.2.0"
#define FW_BUILD_TAG            "2026.11.21"

#define NVS_NAMESPACE           "cw4in1"
#define NVS_KEY_CFG_A           "cfgA"
#define NVS_KEY_CFG_B           "cfgB"
#define NVS_KEY_META            "meta"
#define NVS_KEY_STATS           "stat"

#define CONFIG_MAGIC            0x34574331UL
#define CONFIG_VERSION          3u
#define META_MAGIC              0x4D455441UL
#define STATS_MAGIC             0x53544154UL
#define STATS_VERSION           3u

#define CHANNEL_COUNT           4
#define COIN_MAP_MAX            8
#define DAY_RING_LEN            32
#define SESSION_RING_LEN        16
#define CMD_QUEUE_LEN           8
#define JSON_BUFFER_LEN         6144
#define FORM_BODY_LIMIT         4096
#define WEB_TASK_STACK          12288
#define WEB_TASK_PRIORITY       1
#define WEB_TASK_CORE           0
#define CONTROL_SLICE_US        1000ULL
#define RUNNING_WINDOW_US       60000000ULL
#define HEALTH_INTERVAL_US      30000000ULL
#define AP_GUARD_INTERVAL_US    5000000ULL
#define LCD_PROBE_INTERVAL_US   15000000ULL
#define SESSION_SAVE_MIN_US     2000000ULL
#define LCD_MAX_SECONDS         3599999ULL
#define RELAY_SETTLE_MS         0

// ======================================================
// DEFAULT PIN MAP
// ======================================================

#define PIN_DEFAULT_RELAY_WATER   19
#define PIN_DEFAULT_RELAY_BLOWER  18
#define PIN_DEFAULT_RELAY_SOAP    17
#define PIN_DEFAULT_RELAY_FOAM    16
#define PIN_DEFAULT_BTN_WATER     32
#define PIN_DEFAULT_BTN_BLOWER    33
#define PIN_DEFAULT_BTN_SOAP      27
#define PIN_DEFAULT_BTN_FOAM      14
#define PIN_DEFAULT_COIN          25
#define PIN_DEFAULT_PULSE_OUT     26
#define PIN_DEFAULT_BUZZER        23
#define PIN_DEFAULT_BOOT          0
#define PIN_DEFAULT_LCD_SDA       21
#define PIN_DEFAULT_LCD_SCL       22

// ======================================================
// CHANNEL IDENTITY
// ======================================================

enum ChannelId : uint8_t {
  CH_WATER = 0,
  CH_BLOWER = 1,
  CH_SOAP = 2,
  CH_FOAM = 3
};

static const char* const CHANNEL_NAME[CHANNEL_COUNT] = { "WATER", "BLOWER", "SOAP", "FOAM" };

// ======================================================
// ENUMERATED STATES
// ======================================================

enum ChannelState : uint8_t {
  CHS_IDLE = 0,
  CHS_RUNNING = 1,
  CHS_PAUSED = 2
};

enum TimeModel : uint8_t {
  TMO_SHARED_SINGLE = 0,
  TMO_SHARED_PER_CHANNEL = 1,
  TMO_ALLOCATED = 2
};

enum BuzzerPattern : uint8_t {
  BUZ_NONE = 0,
  BUZ_BOOT = 1,
  BUZ_COIN = 2,
  BUZ_START = 3,
  BUZ_PAUSE = 4,
  BUZ_STOP = 5,
  BUZ_ERROR = 6,
  BUZ_CLICK = 7,
  BUZ_FACTORY = 8,
  BUZ_RESET = 9,
  BUZ_WIFI = 10
};

enum CommandType : uint8_t {
  CMD_NONE = 0,
  CMD_CH_START = 1,
  CMD_CH_PAUSE = 2,
  CMD_CH_RESUME = 3,
  CMD_CH_STOP = 4,
  CMD_CH_TOGGLE = 5,
  CMD_CH_ADD = 6,
  CMD_ADD_CREDIT = 7,
  CMD_SIM_COIN = 8,
  CMD_RESET_STATS = 9,
  CMD_FACTORY_RESET = 10,
  CMD_REBOOT = 11,
  CMD_BUZZER_TEST = 12,
  CMD_SAVE_STATS = 13,
  CMD_CLEAR_CREDIT = 14,
  CMD_CLEAR_SESSIONS = 15
};

// ======================================================
// PERSISTENT CONFIGURATION STRUCTURES
// ======================================================

struct CoinPreset {
  uint16_t pulses;
  uint16_t minutes;
  uint32_t valueCentavos;
};

struct Config {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved0;
  char     apSsid[33];
  char     apPass[65];
  char     adminPin[9];
  uint8_t  apChannel;
  uint8_t  apHidden;
  uint8_t  apMaxClients;
  uint8_t  timeModel;
  uint8_t  relayPin[CHANNEL_COUNT];
  uint8_t  btnPin[CHANNEL_COUNT];
  uint8_t  relayActiveLow;
  uint8_t  coinPin;
  uint8_t  pulseOutPin;
  uint8_t  pulseOutEnabled;
  uint8_t  buzzerPin;
  uint8_t  buzzerEnabled;
  uint8_t  bootPin;
  uint8_t  lcdAddr;
  uint8_t  lcdCols;
  uint8_t  lcdRows;
  uint8_t  lcdBacklight;
  uint8_t  lcdSda;
  uint8_t  lcdScl;
  uint8_t  bonusEnabled;
  uint8_t  coinMapCount;
  uint8_t  lowHeapRestart;
  uint16_t pulseOutWidthMs;
  uint16_t buzzerFreq;
  uint16_t buzzerBeepMs;
  uint16_t buzzerErrorFreq;
  uint16_t factoryHoldMs;
  uint16_t longPressMs;
  uint16_t doubleClickMs;
  uint16_t debounceMs;
  uint16_t longPressAddSeconds;
  uint16_t coinMinPulseGapMs;
  uint16_t coinGroupGapMs;
  uint16_t coinMaxPulses;
  uint16_t lcdRefreshMs;
  uint16_t lcdBlinkMs;
  uint16_t statsSaveIntervalSec;
  uint32_t maxBonusSeconds;
  uint32_t maxRunSeconds;
  uint32_t maxCreditSeconds;
  uint32_t lowHeapBytes;
  CoinPreset coinMap[COIN_MAP_MAX];
  uint8_t  reserved1[8];
};

// ======================================================
// STATISTICS AND JOURNAL STRUCTURES
// ======================================================

struct SessionRecord {
  uint32_t startSec;
  uint32_t durationSec;
  uint8_t  channel;
  uint8_t  reason;
  uint8_t  pad[2];
};

struct DayRecord {
  uint64_t revenueCentavos;
  uint64_t seconds;
  uint32_t coins;
  uint32_t pad;
};

struct Stats {
  uint32_t magic;
  uint16_t version;
  uint16_t pad0;
  uint64_t revenueCentavos;
  uint64_t coins;
  uint64_t pulses;
  uint64_t bonusSeconds;
  uint64_t uptimeSeconds;
  uint64_t channelSeconds[CHANNEL_COUNT];
  uint64_t channelSessions[CHANNEL_COUNT];
  uint32_t boots;
  uint32_t dayIndex;
  uint64_t dayStartUptime;
  DayRecord days[DAY_RING_LEN];
  SessionRecord sessions[SESSION_RING_LEN];
  uint8_t  sessionHead;
  uint8_t  sessionCount;
  uint8_t  pad1[2];
};

// ======================================================
// PERSISTENT META STRUCTURE
// ======================================================

struct Meta {
  uint32_t magic;
  uint32_t configSeq;
  uint8_t  activeSlot;
  uint8_t  pad[3];
  uint32_t nvsWrites;
  uint32_t crc;
};

// ======================================================
// RUNTIME CHANNEL STRUCTURE
// ======================================================

struct ChannelRuntime {
  uint8_t  state;
  uint8_t  relayOn;
  uint8_t  relayPin;
  uint8_t  buttonPin;
  uint64_t remainMs;
  uint64_t usedMs;
  uint64_t sessionStartUs;
  uint64_t lastBlinkToggleUs;
  uint8_t  blinkPhase;
};

// ======================================================
// BUTTON GESTURE STRUCTURE
// ======================================================

struct ButtonRuntime {
  uint8_t  rawLevel;
  uint8_t  stableLevel;
  uint8_t  candidateLevel;
  uint8_t  clicks;
  uint8_t  longSent;
  uint8_t  shortPending;
  uint64_t candidateUs;
  uint64_t pressUs;
  uint64_t releaseUs;
  uint64_t shortDueUs;
};

// ======================================================
// BUZZER STRUCTURE
// ======================================================

struct BeepStep {
  uint16_t freq;
  uint16_t onMs;
  uint16_t offMs;
};

struct BuzzerRuntime {
  const BeepStep* steps;
  uint8_t  count;
  uint8_t  index;
  uint8_t  phase;
  uint8_t  active;
  uint64_t nextUs;
};

// ======================================================
// COMMAND STRUCTURE
// ======================================================

struct Command {
  uint8_t  type;
  uint8_t  channel;
  uint16_t seconds;
  uint32_t value;
  uint64_t executeAtUs;
};

// ======================================================
// SHARED STATE STRUCTURE
// ======================================================

struct SharedState {
  uint64_t poolMs;
  uint64_t bonusUsedSeconds;
  uint64_t sessionStartUs;
  uint64_t creditAddedSeconds;
  uint64_t lastCoinUs;
  uint64_t bootUs;
  uint32_t coinPulsesPending;
  uint32_t coinPulsesTotal;
  uint32_t coinGroupsTotalBurst;
  uint32_t cfgRevision;
  uint8_t  activeChannels;
  uint8_t  lastButtonChannel;
  uint8_t  lcdPresent;
  uint8_t  apRunning;
  uint8_t  factoryHoldActive;
  uint8_t  restartRequired;
  uint8_t  channelState[CHANNEL_COUNT];
  uint8_t  channelRelay[CHANNEL_COUNT];
  uint8_t  blinkPhase[CHANNEL_COUNT];
  uint64_t channelRemainMs[CHANNEL_COUNT];
  uint64_t channelUsedMs[CHANNEL_COUNT];
  char     lcdLine[4][21];
};

// ======================================================
// GLOBAL DECLARATIONS
// ======================================================

static Config          g_cfg;
static Config          g_cfgShadow;
static Stats           g_stats;
static Meta            g_meta;
static SharedState     g_share;

static ChannelRuntime  g_ch[CHANNEL_COUNT];
static ButtonRuntime   g_btn[CHANNEL_COUNT];
static BuzzerRuntime   g_buzz;

static portMUX_TYPE    g_mux = portMUX_INITIALIZER_UNLOCKED;
static Command         g_cmdQueue[CMD_QUEUE_LEN];
static uint8_t         g_cmdHead;
static uint8_t         g_cmdTail;
static uint8_t         g_cmdCount;

static Config          g_pendingCfg;
static uint8_t         g_pendingCfgFlag;
static uint32_t        g_pendingCfgSeq;

static WebServer       g_server(80);
static DNSServer       g_dns;
static Preferences     g_prefs;

alignas(16) static uint8_t g_lcdStorage[sizeof(LiquidCrystal_I2C)];
static LiquidCrystal_I2C* g_lcd = nullptr;

static char            g_json[JSON_BUFFER_LEN];
static char            g_formBuf[FORM_BODY_LIMIT];

static char            g_lcdShadow[4][24];
static char            g_lcdRendered[4][24];
static uint64_t        g_lcdNextRefreshUs;
static uint64_t        g_lcdNextProbeUs;
static uint8_t         g_lcdInitAttempts;
static uint8_t         g_lcdFailCount;

static char            g_overlay[4][36];
static uint8_t         g_overlayActive;
static uint64_t        g_overlayUntilUs;

static uint64_t        g_lastTickUs;
static uint64_t        g_lastHealthUs;
static uint64_t        g_lastApGuardUs;
static uint64_t        g_lastStatsSaveUs;
static uint64_t        g_bootUs;
static uint64_t        g_apRestartAtUs;
static uint8_t         g_apRestartPending;

static uint8_t         g_statsDirty;
static uint8_t         g_statsForceSave;
static uint8_t         g_nvsReady;
static uint32_t        g_nvsWrites;
static uint8_t         g_bootComplete;

static uint8_t         g_coinPinAttached;
static uint8_t         g_coinPinCurrent;
static uint32_t        g_coinIsrCount;
static uint32_t        g_coinIsrSeen;
static uint64_t        g_coinGroupStartUs;
static uint64_t        g_coinLastPulseUs;
static uint32_t        g_coinGroupPulses;
static uint8_t         g_coinGroupActive;
static uint8_t         g_pulseOutActive;
static uint32_t        g_pulseOutPending;
static uint64_t        g_pulseOutOffUs;

static uint8_t         g_lowHeapStrikes;
static uint8_t         g_pinsAttached;
static uint8_t         g_factoryHoldArmed;
static uint64_t        g_factoryHoldUs;
static uint8_t         g_factoryConfirmed;
static uint8_t         g_webReady;
static uint8_t         g_statsSaveSuspended;
static uint8_t         g_cfgLoadedSlot;
static uint8_t         g_apStationCount;
static uint32_t        g_apRestartCount;
static uint32_t        g_wifiFailCount;

// ======================================================
// ADVANCED TYPES USED BY FUNCTION SIGNATURES
// ======================================================

struct SlotRecord {
  uint32_t magic;
  uint32_t seq;
  uint32_t cfgCrc;
  uint16_t version;
  uint16_t cfgSize;
  uint32_t reserved;
  Config   cfg;
};

struct Publish {
  SharedState state;
  Stats       stats;
  Config      cfg;
};

struct CoinAward {
  uint32_t seconds;
  uint32_t valueCentavos;
  uint8_t  matched;
};

enum FieldType : uint8_t {
  FT_STR = 0,
  FT_U8 = 1,
  FT_U16 = 2,
  FT_U32 = 3
};

struct FieldDef {
  const char* key;
  uint8_t     type;
  uint8_t     len;
  uint16_t    off;
  uint32_t    minv;
  uint32_t    maxv;
};

// ======================================================
// CROSS SECTION FORWARD DECLARATIONS
// ======================================================

static void lcdInit();
static void lcdShutdown();
static void lcdTick(uint64_t nowUs);
static void lcdRenderRows(uint64_t nowUs);
static void lcdFlushRows();
static void lcdFlushOverlayNow();
static void buzzerPlay(uint8_t pattern);
static void buzzerSilence();
static void statsMarkDirty();
static bool statsSaveNow();
static void buttonResetRuntime(uint8_t ch);
static void coinInputAttach();
static void coinInputDetach();
static void relayForceOffAll();


// ======================================================
// 64-BIT MONOTONIC TIME BASE
// ======================================================

static inline uint64_t timeNowUs() {
  return (uint64_t)esp_timer_get_time();
}

static inline uint64_t timeNowMs() {
  return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static inline uint64_t timeSinceUs(uint64_t sinceUs) {
  uint64_t now = (uint64_t)esp_timer_get_time();
  return (now >= sinceUs) ? (now - sinceUs) : 0ULL;
}

static inline uint64_t timeSinceMs(uint64_t sinceMs) {
  uint64_t now = (uint64_t)esp_timer_get_time() / 1000ULL;
  return (now >= sinceMs) ? (now - sinceMs) : 0ULL;
}

static uint64_t satSubMs(uint64_t value, uint64_t amount) {
  return (value > amount) ? (value - amount) : 0ULL;
}

// ======================================================
// CHECKSUM AND STRING UTILITIES
// ======================================================

static uint32_t crc32Compute(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint32_t)data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1UL));
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

static void safeCopyString(char* dst, size_t dstLen, const char* src) {
  if (dstLen == 0) {
    return;
  }
  size_t i = 0;
  if (src != nullptr) {
    for (; i + 1 < dstLen && src[i] != '\0'; ++i) {
      dst[i] = src[i];
    }
  }
  dst[i] = '\0';
}

static bool isPrintableAscii(const char* value, size_t maxLen) {
  if (value == nullptr) {
    return true;
  }
  size_t i = 0;
  for (; value[i] != '\0'; ++i) {
    if (i >= maxLen) {
      return false;
    }
    unsigned char c = (unsigned char)value[i];
    if (c < 0x20 || c > 0x7E) {
      return false;
    }
  }
  return true;
}

static bool isDigitsOnly(const char* value) {
  if (value == nullptr) {
    return true;
  }
  for (size_t i = 0; value[i] != '\0'; ++i) {
    if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }
  return true;
}

// ======================================================
// FACTORY DEFAULTS
// ======================================================

static void configSetDefaults(Config& cfg) {
  memset(&cfg, 0, sizeof(Config));
  cfg.magic = CONFIG_MAGIC;
  cfg.version = CONFIG_VERSION;
  safeCopyString(cfg.apSsid, sizeof(cfg.apSsid), "ESP32_4in1_Carwash");
  safeCopyString(cfg.apPass, sizeof(cfg.apPass), "12345678");
  safeCopyString(cfg.adminPin, sizeof(cfg.adminPin), "");
  cfg.apChannel = 1;
  cfg.apHidden = 0;
  cfg.apMaxClients = 4;
  cfg.timeModel = TMO_ALLOCATED;
  cfg.relayPin[CH_WATER] = PIN_DEFAULT_RELAY_WATER;
  cfg.relayPin[CH_BLOWER] = PIN_DEFAULT_RELAY_BLOWER;
  cfg.relayPin[CH_SOAP] = PIN_DEFAULT_RELAY_SOAP;
  cfg.relayPin[CH_FOAM] = PIN_DEFAULT_RELAY_FOAM;
  cfg.btnPin[CH_WATER] = PIN_DEFAULT_BTN_WATER;
  cfg.btnPin[CH_BLOWER] = PIN_DEFAULT_BTN_BLOWER;
  cfg.btnPin[CH_SOAP] = PIN_DEFAULT_BTN_SOAP;
  cfg.btnPin[CH_FOAM] = PIN_DEFAULT_BTN_FOAM;
  cfg.relayActiveLow = 1;
  cfg.coinPin = PIN_DEFAULT_COIN;
  cfg.pulseOutPin = PIN_DEFAULT_PULSE_OUT;
  cfg.pulseOutEnabled = 1;
  cfg.buzzerPin = PIN_DEFAULT_BUZZER;
  cfg.buzzerEnabled = 1;
  cfg.bootPin = PIN_DEFAULT_BOOT;
  cfg.lcdAddr = 0x27;
  cfg.lcdCols = 20;
  cfg.lcdRows = 4;
  cfg.lcdBacklight = 1;
  cfg.lcdSda = PIN_DEFAULT_LCD_SDA;
  cfg.lcdScl = PIN_DEFAULT_LCD_SCL;
  cfg.bonusEnabled = 1;
  cfg.coinMapCount = 4;
  cfg.lowHeapRestart = 1;
  cfg.pulseOutWidthMs = 60;
  cfg.buzzerFreq = 2400;
  cfg.buzzerBeepMs = 70;
  cfg.buzzerErrorFreq = 700;
  cfg.factoryHoldMs = 5000;
  cfg.longPressMs = 900;
  cfg.doubleClickMs = 350;
  cfg.debounceMs = 30;
  cfg.longPressAddSeconds = 60;
  cfg.coinMinPulseGapMs = 30;
  cfg.coinGroupGapMs = 350;
  cfg.coinMaxPulses = 60;
  cfg.lcdRefreshMs = 250;
  cfg.lcdBlinkMs = 500;
  cfg.statsSaveIntervalSec = 300;
  cfg.maxBonusSeconds = 600;
  cfg.maxRunSeconds = 7200;
  cfg.maxCreditSeconds = 35999;
  cfg.lowHeapBytes = 12288;
  cfg.coinMap[0].pulses = 1;
  cfg.coinMap[0].minutes = 5;
  cfg.coinMap[0].valueCentavos = 100;
  cfg.coinMap[1].pulses = 5;
  cfg.coinMap[1].minutes = 25;
  cfg.coinMap[1].valueCentavos = 500;
  cfg.coinMap[2].pulses = 10;
  cfg.coinMap[2].minutes = 50;
  cfg.coinMap[2].valueCentavos = 1000;
  cfg.coinMap[3].pulses = 20;
  cfg.coinMap[3].minutes = 60;
  cfg.coinMap[3].valueCentavos = 2000;
  for (uint8_t i = 4; i < COIN_MAP_MAX; ++i) {
    cfg.coinMap[i].pulses = 0;
    cfg.coinMap[i].minutes = 0;
    cfg.coinMap[i].valueCentavos = 0;
  }
}

static void statsSetDefaults(Stats& st) {
  memset(&st, 0, sizeof(Stats));
  st.magic = STATS_MAGIC;
  st.version = STATS_VERSION;
  st.boots = 0;
  st.dayIndex = 0;
  st.dayStartUptime = 0;
  st.sessionHead = 0;
  st.sessionCount = 0;
}

static void metaSetDefaults(Meta& meta) {
  memset(&meta, 0, sizeof(Meta));
  meta.magic = META_MAGIC;
  meta.configSeq = 0;
  meta.activeSlot = 0;
  meta.nvsWrites = 0;
  meta.crc = crc32Compute((const uint8_t*)&meta, sizeof(Meta) - sizeof(uint32_t));
}

// ======================================================
// PIN CAPABILITY VALIDATION
// ======================================================

static bool pinIsUsable(uint8_t pin) {
  switch (pin) {
    case 4: case 5: case 13: case 14: case 15: case 16:
    case 17: case 18: case 19: case 21: case 22: case 23:
    case 25: case 26: case 27: case 32: case 33:
      return true;
    default:
      return false;
  }
}

// ======================================================
// CONFIGURATION VALIDATION
// ======================================================

static bool validateConfig(const Config& cfg, char* err, size_t errLen) {
  if (err == nullptr || errLen == 0) {
    return false;
  }
  err[0] = '\0';

  if (cfg.timeModel > TMO_ALLOCATED) {
    safeCopyString(err, errLen, "timeModel");
    return false;
  }
  if (!isPrintableAscii(cfg.apSsid, 32) || strlen(cfg.apSsid) < 1 || strlen(cfg.apSsid) > 32) {
    safeCopyString(err, errLen, "apSsid");
    return false;
  }
  size_t passLen = strlen(cfg.apPass);
  if (!isPrintableAscii(cfg.apPass, 63) || (passLen != 0 && (passLen < 8 || passLen > 63))) {
    safeCopyString(err, errLen, "apPass");
    return false;
  }
  if (strlen(cfg.adminPin) > 8 || !isDigitsOnly(cfg.adminPin)) {
    safeCopyString(err, errLen, "adminPin");
    return false;
  }
  if (cfg.apChannel < 1 || cfg.apChannel > 13) {
    safeCopyString(err, errLen, "apChannel");
    return false;
  }
  if (cfg.apMaxClients < 1 || cfg.apMaxClients > 8) {
    safeCopyString(err, errLen, "apMaxClients");
    return false;
  }
  if (cfg.apHidden > 1 || cfg.relayActiveLow > 1 || cfg.pulseOutEnabled > 1 ||
      cfg.buzzerEnabled > 1 || cfg.lcdBacklight > 1 || cfg.bonusEnabled > 1 ||
      cfg.lowHeapRestart > 1) {
    safeCopyString(err, errLen, "switch");
    return false;
  }
  if (cfg.lcdAddr < 0x08 || cfg.lcdAddr > 0x77) {
    safeCopyString(err, errLen, "lcdAddr");
    return false;
  }
  if (cfg.lcdCols < 16 || cfg.lcdCols > 20) {
    safeCopyString(err, errLen, "lcdCols");
    return false;
  }
  if (cfg.lcdRows < 2 || cfg.lcdRows > 4) {
    safeCopyString(err, errLen, "lcdRows");
    return false;
  }
  if (cfg.lcdRefreshMs < 50 || cfg.lcdRefreshMs > 2000) {
    safeCopyString(err, errLen, "lcdRefreshMs");
    return false;
  }
  if (cfg.lcdBlinkMs < 120 || cfg.lcdBlinkMs > 2000) {
    safeCopyString(err, errLen, "lcdBlinkMs");
    return false;
  }
  if (cfg.pulseOutWidthMs < 10 || cfg.pulseOutWidthMs > 1000) {
    safeCopyString(err, errLen, "pulseOutWidthMs");
    return false;
  }
  if (cfg.buzzerFreq < 100 || cfg.buzzerFreq > 10000) {
    safeCopyString(err, errLen, "buzzerFreq");
    return false;
  }
  if (cfg.buzzerErrorFreq < 100 || cfg.buzzerErrorFreq > 10000) {
    safeCopyString(err, errLen, "buzzerErrorFreq");
    return false;
  }
  if (cfg.buzzerBeepMs < 10 || cfg.buzzerBeepMs > 1000) {
    safeCopyString(err, errLen, "buzzerBeepMs");
    return false;
  }
  if (cfg.factoryHoldMs < 1000 || cfg.factoryHoldMs > 30000) {
    safeCopyString(err, errLen, "factoryHoldMs");
    return false;
  }
  if (cfg.longPressMs < 200 || cfg.longPressMs > 5000) {
    safeCopyString(err, errLen, "longPressMs");
    return false;
  }
  if (cfg.doubleClickMs < 100 || cfg.doubleClickMs > 2000) {
    safeCopyString(err, errLen, "doubleClickMs");
    return false;
  }
  if (cfg.debounceMs < 5 || cfg.debounceMs > 200) {
    safeCopyString(err, errLen, "debounceMs");
    return false;
  }
  if (cfg.longPressAddSeconds > 3600) {
    safeCopyString(err, errLen, "longPressAddSeconds");
    return false;
  }
  if (cfg.bonusEnabled == 1 && cfg.longPressAddSeconds == 0) {
    safeCopyString(err, errLen, "longPressAddSeconds");
    return false;
  }
  if (cfg.maxBonusSeconds > 86400UL) {
    safeCopyString(err, errLen, "maxBonusSeconds");
    return false;
  }
  if (cfg.coinMinPulseGapMs < 5 || cfg.coinMinPulseGapMs > 500) {
    safeCopyString(err, errLen, "coinMinPulseGapMs");
    return false;
  }
  if (cfg.coinGroupGapMs < 50 || cfg.coinGroupGapMs > 5000) {
    safeCopyString(err, errLen, "coinGroupGapMs");
    return false;
  }
  if (cfg.coinGroupGapMs < (cfg.coinMinPulseGapMs + 20)) {
    safeCopyString(err, errLen, "coinGroupGapMs");
    return false;
  }
  if (cfg.coinMaxPulses < 1 || cfg.coinMaxPulses > 200) {
    safeCopyString(err, errLen, "coinMaxPulses");
    return false;
  }
  if (cfg.coinMapCount < 1 || cfg.coinMapCount > COIN_MAP_MAX) {
    safeCopyString(err, errLen, "coinMapCount");
    return false;
  }
  for (uint8_t i = 0; i < cfg.coinMapCount; ++i) {
    if (cfg.coinMap[i].pulses < 1 || cfg.coinMap[i].pulses > 200) {
      safeCopyString(err, errLen, "coinPulses");
      return false;
    }
    if (cfg.coinMap[i].minutes < 1 || cfg.coinMap[i].minutes > 1000) {
      safeCopyString(err, errLen, "coinMinutes");
      return false;
    }
    if (cfg.coinMap[i].valueCentavos > 1000000UL) {
      safeCopyString(err, errLen, "coinValue");
      return false;
    }
    for (uint8_t j = (uint8_t)(i + 1); j < cfg.coinMapCount; ++j) {
      if (cfg.coinMap[j].pulses == cfg.coinMap[i].pulses) {
        safeCopyString(err, errLen, "coinPulses");
        return false;
      }
    }
  }
  if (cfg.statsSaveIntervalSec < 30 || cfg.statsSaveIntervalSec > 3600) {
    safeCopyString(err, errLen, "statsSaveIntervalSec");
    return false;
  }
  if (cfg.maxRunSeconds < 60 || cfg.maxRunSeconds > 86400UL) {
    safeCopyString(err, errLen, "maxRunSeconds");
    return false;
  }
  if (cfg.maxCreditSeconds < 60 || cfg.maxCreditSeconds > 86400UL) {
    safeCopyString(err, errLen, "maxCreditSeconds");
    return false;
  }
  if (cfg.lowHeapBytes < 4096 || cfg.lowHeapBytes > 131072UL) {
    safeCopyString(err, errLen, "lowHeapBytes");
    return false;
  }
  if (cfg.bootPin != PIN_DEFAULT_BOOT) {
    safeCopyString(err, errLen, "bootPin");
    return false;
  }

  uint8_t used[CHANNEL_COUNT * 2 + 6];
  uint8_t usedCount = 0;
  used[usedCount++] = cfg.coinPin;
  used[usedCount++] = cfg.lcdSda;
  used[usedCount++] = cfg.lcdScl;
  used[usedCount++] = cfg.buzzerPin;
  if (cfg.pulseOutEnabled == 1) {
    used[usedCount++] = cfg.pulseOutPin;
  }
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    used[usedCount++] = cfg.relayPin[i];
  }
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    used[usedCount++] = cfg.btnPin[i];
  }
  for (uint8_t i = 0; i < usedCount; ++i) {
    if (!pinIsUsable(used[i])) {
      safeCopyString(err, errLen, "pin");
      return false;
    }
    for (uint8_t j = (uint8_t)(i + 1); j < usedCount; ++j) {
      if (used[j] == used[i]) {
        safeCopyString(err, errLen, "pin");
        return false;
      }
    }
  }
  return true;
}

// ======================================================
// DUAL SLOT NVS PERSISTENCE
// ======================================================

static const char* slotKeyName(uint8_t slot) {
  return (slot == 0) ? NVS_KEY_CFG_A : NVS_KEY_CFG_B;
}

static bool slotRead(uint8_t slot, SlotRecord& rec) {
  size_t want = sizeof(SlotRecord);
  size_t got = g_prefs.getBytesLength(slotKeyName(slot));
  if (got != want) {
    return false;
  }
  if (g_prefs.getBytes(slotKeyName(slot), &rec, want) != want) {
    return false;
  }
  if (rec.magic != CONFIG_MAGIC || rec.version != CONFIG_VERSION) {
    return false;
  }
  if (rec.cfgSize != (uint16_t)sizeof(Config)) {
    return false;
  }
  uint32_t crc = crc32Compute((const uint8_t*)&rec.cfg, sizeof(Config));
  if (crc != rec.cfgCrc) {
    return false;
  }
  return true;
}

static void metaSave() {
  if (!g_nvsReady) {
    return;
  }
  g_meta.crc = crc32Compute((const uint8_t*)&g_meta, sizeof(Meta) - sizeof(uint32_t));
  g_prefs.putBytes(NVS_KEY_META, &g_meta, sizeof(Meta));
  g_nvsWrites++;
  g_meta.nvsWrites = g_nvsWrites;
}

static void metaLoad() {
  Meta meta;
  size_t got = g_prefs.getBytesLength(NVS_KEY_META);
  if (got == sizeof(Meta) && g_prefs.getBytes(NVS_KEY_META, &meta, sizeof(Meta)) == sizeof(Meta)) {
    uint32_t crc = crc32Compute((const uint8_t*)&meta, sizeof(Meta) - sizeof(uint32_t));
    if (meta.magic == META_MAGIC && meta.crc == crc) {
      g_meta = meta;
      g_nvsWrites = meta.nvsWrites;
      return;
    }
  }
  metaSetDefaults(g_meta);
  g_nvsWrites = 0;
}

static bool configSave(const Config& cfg) {
  if (!g_nvsReady) {
    return false;
  }
  SlotRecord recA;
  SlotRecord recB;
  bool okA = slotRead(0, recA);
  bool okB = slotRead(1, recB);
  uint32_t nextSeq = 1;
  if (okA && recA.seq >= nextSeq) {
    nextSeq = recA.seq + 1;
  }
  if (okB && recB.seq >= nextSeq) {
    nextSeq = recB.seq + 1;
  }
  uint8_t target;
  if (okA && okB) {
    target = (recA.seq <= recB.seq) ? 0 : 1;
  } else if (okA) {
    target = 1;
  } else if (okB) {
    target = 0;
  } else {
    target = 0;
  }
  SlotRecord out;
  memset(&out, 0, sizeof(SlotRecord));
  out.magic = CONFIG_MAGIC;
  out.seq = nextSeq;
  out.version = CONFIG_VERSION;
  out.cfgSize = (uint16_t)sizeof(Config);
  out.cfg = cfg;
  out.cfg.magic = CONFIG_MAGIC;
  out.cfg.version = CONFIG_VERSION;
  out.cfgCrc = crc32Compute((const uint8_t*)&out.cfg, sizeof(Config));

  size_t written = g_prefs.putBytes(slotKeyName(target), &out, sizeof(SlotRecord));
  g_nvsWrites++;
  if (written != sizeof(SlotRecord)) {
    g_meta.nvsWrites = g_nvsWrites;
    metaSave();
    return false;
  }
  SlotRecord verify;
  if (!slotRead(target, verify) || verify.seq != nextSeq) {
    g_meta.nvsWrites = g_nvsWrites;
    metaSave();
    return false;
  }
  g_meta.configSeq = nextSeq;
  g_meta.activeSlot = target;
  g_meta.nvsWrites = g_nvsWrites;
  metaSave();
  g_cfgLoadedSlot = target;
  return true;
}

static bool configLoad(Config& cfg, uint8_t& slotOut) {
  SlotRecord recA;
  SlotRecord recB;
  bool okA = slotRead(0, recA);
  bool okB = slotRead(1, recB);
  char err[16];
  if (okA && okB) {
    if (recA.seq >= recB.seq) {
      if (validateConfig(recA.cfg, err, sizeof(err))) {
        cfg = recA.cfg;
        slotOut = 0;
        return true;
      }
      if (validateConfig(recB.cfg, err, sizeof(err))) {
        cfg = recB.cfg;
        slotOut = 1;
        return true;
      }
    } else {
      if (validateConfig(recB.cfg, err, sizeof(err))) {
        cfg = recB.cfg;
        slotOut = 1;
        return true;
      }
      if (validateConfig(recA.cfg, err, sizeof(err))) {
        cfg = recA.cfg;
        slotOut = 0;
        return true;
      }
    }
    return false;
  }
  if (okA) {
    if (validateConfig(recA.cfg, err, sizeof(err))) {
      cfg = recA.cfg;
      slotOut = 0;
      return true;
    }
    return false;
  }
  if (okB) {
    if (validateConfig(recB.cfg, err, sizeof(err))) {
      cfg = recB.cfg;
      slotOut = 1;
      return true;
    }
    return false;
  }
  return false;
}

// ======================================================
// STATISTICS PERSISTENCE
// ======================================================

static void statsLoad() {
  Stats st;
  size_t got = g_prefs.getBytesLength(NVS_KEY_STATS);
  if (got == sizeof(Stats) && g_prefs.getBytes(NVS_KEY_STATS, &st, sizeof(Stats)) == sizeof(Stats)) {
    if (st.magic == STATS_MAGIC && st.version == STATS_VERSION &&
        st.sessionHead < SESSION_RING_LEN && st.sessionCount <= SESSION_RING_LEN) {
      g_stats = st;
      return;
    }
  }
  statsSetDefaults(g_stats);
}

static bool statsSaveNow() {
  if (!g_nvsReady) {
    return false;
  }
  g_stats.magic = STATS_MAGIC;
  g_stats.version = STATS_VERSION;
  size_t written = g_prefs.putBytes(NVS_KEY_STATS, &g_stats, sizeof(Stats));
  g_nvsWrites++;
  bool ok = (written == sizeof(Stats));
  g_statsDirty = ok ? 0 : 1;
  g_lastStatsSaveUs = timeNowUs();
  g_meta.nvsWrites = g_nvsWrites;
  return ok;
}

static void statsMarkDirty() {
  g_statsDirty = 1;
}


// ======================================================
// PUBLISHED STATE SNAPSHOT
// ======================================================

static Publish g_pub;
static Publish g_pubLocal;

static void publishState() {
  portENTER_CRITICAL(&g_mux);
  g_pub.state = g_share;
  portEXIT_CRITICAL(&g_mux);
}

static void publishStatsAndConfig() {
  portENTER_CRITICAL(&g_mux);
  g_pub.stats = g_stats;
  g_pub.cfg = g_cfg;
  portEXIT_CRITICAL(&g_mux);
}

static void readPublished(Publish& out) {
  portENTER_CRITICAL(&g_mux);
  out = g_pub;
  portEXIT_CRITICAL(&g_mux);
}

// ======================================================
// COMMAND QUEUE
// ======================================================

static bool cmdPush(uint8_t type, uint8_t channel, uint16_t seconds, uint32_t value, uint32_t delayMs) {
  bool ok = false;
  portENTER_CRITICAL(&g_mux);
  if (g_cmdCount < CMD_QUEUE_LEN) {
    uint8_t slot = g_cmdTail;
    g_cmdQueue[slot].type = type;
    g_cmdQueue[slot].channel = channel;
    g_cmdQueue[slot].seconds = seconds;
    g_cmdQueue[slot].value = value;
    g_cmdQueue[slot].executeAtUs = timeNowUs() + ((uint64_t)delayMs * 1000ULL);
    g_cmdTail = (uint8_t)((g_cmdTail + 1) % CMD_QUEUE_LEN);
    g_cmdCount++;
    ok = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return ok;
}

static bool cmdPop(Command& out) {
  bool ok = false;
  portENTER_CRITICAL(&g_mux);
  if (g_cmdCount > 0) {
    out = g_cmdQueue[g_cmdHead];
    g_cmdHead = (uint8_t)((g_cmdHead + 1) % CMD_QUEUE_LEN);
    g_cmdCount--;
    ok = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return ok;
}

static void cmdQueueReset() {
  portENTER_CRITICAL(&g_mux);
  g_cmdHead = 0;
  g_cmdTail = 0;
  g_cmdCount = 0;
  portEXIT_CRITICAL(&g_mux);
}

static bool configPushIfCurrent(const Config& cfg, uint32_t revision) {
  bool ok = false;
  portENTER_CRITICAL(&g_mux);
  if (g_pendingCfgFlag == 0 && g_pub.state.cfgRevision == revision) {
    g_pendingCfg = cfg;
    g_pendingCfgSeq = g_pendingCfgSeq + 1;
    g_pendingCfgFlag = 1;
    ok = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return ok;
}

static bool configPush(const Config& cfg) {
  uint32_t revision;
  portENTER_CRITICAL(&g_mux);
  revision = g_pub.state.cfgRevision;
  portEXIT_CRITICAL(&g_mux);
  return configPushIfCurrent(cfg, revision);
}

static bool configTakePending(Config& out) {
  bool ok = false;
  portENTER_CRITICAL(&g_mux);
  if (g_pendingCfgFlag == 1) {
    out = g_pendingCfg;
    g_pendingCfgFlag = 2;
    ok = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return ok;
}

static void configFinishPending() {
  portENTER_CRITICAL(&g_mux);
  g_pendingCfgFlag = 0;
  portEXIT_CRITICAL(&g_mux);
}

// ======================================================
// CREDIT POOL ENGINE
// ======================================================

static uint64_t g_poolMs;
static uint64_t g_usResidual;
static uint8_t  g_drainCursor;

static uint64_t creditCapMs() {
  return (uint64_t)g_cfg.maxCreditSeconds * 1000ULL;
}

static uint64_t poolAddMs(uint64_t addMs) {
  uint64_t cap = creditCapMs();
  uint64_t room = (g_poolMs < cap) ? (cap - g_poolMs) : 0ULL;
  uint64_t granted = (addMs < room) ? addMs : room;
  g_poolMs += granted;
  return granted;
}

static uint64_t creditAddSeconds(uint32_t seconds) {
  uint64_t granted = poolAddMs((uint64_t)seconds * 1000ULL);
  if (granted > 0) {
    g_share.creditAddedSeconds += granted / 1000ULL;
  }
  statsMarkDirty();
  return granted / 1000ULL;
}

static uint64_t channelRemainingMs(uint8_t ch) {
  if (g_cfg.timeModel == TMO_ALLOCATED) {
    return g_ch[ch].remainMs;
  }
  return g_poolMs;
}

static uint8_t countActive() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    if (g_ch[i].state == CHS_RUNNING) {
      n++;
    }
  }
  return n;
}

// ======================================================
// DAY ROLLOVER AND SESSION JOURNAL
// ======================================================

static uint32_t dayIndexFromUptime(uint64_t uptimeSeconds) {
  return (uint32_t)(uptimeSeconds / 86400ULL);
}

static void dayAdvance(uint64_t uptimeSeconds) {
  uint32_t idx = dayIndexFromUptime(uptimeSeconds);
  if (idx == g_stats.dayIndex) {
    return;
  }
  uint32_t steps = idx - g_stats.dayIndex;
  if (steps > DAY_RING_LEN) {
    steps = DAY_RING_LEN;
  }
  for (uint32_t i = 0; i < steps; ++i) {
    uint32_t slot = (g_stats.dayIndex + 1 + i) % DAY_RING_LEN;
    memset(&g_stats.days[slot], 0, sizeof(DayRecord));
  }
  g_stats.dayIndex = idx;
  statsMarkDirty();
}

static void dayAccumulate(uint64_t seconds, uint64_t revenueCentavos, uint32_t coins) {
  uint32_t slot = g_stats.dayIndex % DAY_RING_LEN;
  g_stats.days[slot].seconds += seconds;
  g_stats.days[slot].revenueCentavos += revenueCentavos;
  g_stats.days[slot].coins += coins;
  statsMarkDirty();
}

static void sessionJournalPush(uint8_t channel, uint32_t durationSec, uint8_t reason) {
  SessionRecord& rec = g_stats.sessions[g_stats.sessionHead];
  rec.startSec = (uint32_t)(g_stats.uptimeSeconds);
  rec.durationSec = durationSec;
  rec.channel = channel;
  rec.reason = reason;
  rec.pad[0] = 0;
  rec.pad[1] = 0;
  g_stats.sessionHead = (uint8_t)((g_stats.sessionHead + 1) % SESSION_RING_LEN);
  if (g_stats.sessionCount < SESSION_RING_LEN) {
    g_stats.sessionCount++;
  }
  statsMarkDirty();
}

// ======================================================
// RELAY DRIVER
// ======================================================

static inline uint8_t relayActiveLevel() {
  return (g_cfg.relayActiveLow == 1) ? LOW : HIGH;
}

static inline uint8_t relayInactiveLevel() {
  return (g_cfg.relayActiveLow == 1) ? HIGH : LOW;
}

static void relayWriteRaw(uint8_t pin, bool active) {
  if (pin > 39) {
    return;
  }
  digitalWrite(pin, active ? relayActiveLevel() : relayInactiveLevel());
}

static void relayApplyChannel(uint8_t ch) {
  bool want = (g_ch[ch].state == CHS_RUNNING) && (channelRemainingMs(ch) > 0);
  uint8_t pin = g_ch[ch].relayPin;
  if (pin > 39) {
    want = false;
  }
  bool current = (g_ch[ch].relayOn != 0);
  if (want != current) {
    if (pin <= 39) {
      relayWriteRaw(pin, want);
    }
    g_ch[ch].relayOn = want ? 1 : 0;
  }
}

static void relayApplyAll() {
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    relayApplyChannel(i);
  }
}

static void relayForceOffAll() {
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    uint8_t pin = g_ch[i].relayPin;
    if (pin <= 39) {
      relayWriteRaw(pin, false);
    }
    g_ch[i].relayOn = 0;
  }
}

// ======================================================
// CHANNEL STATE MACHINE
// ======================================================

static void buzzerPlay(uint8_t pattern);

static void channelFinish(uint8_t ch, uint8_t reason) {
  uint32_t durationSec = (uint32_t)(g_ch[ch].usedMs / 1000ULL);
  if (g_cfg.timeModel == TMO_ALLOCATED) {
    g_poolMs += g_ch[ch].remainMs;
  }
  g_ch[ch].state = CHS_IDLE;
  g_ch[ch].usedMs = 0;
  g_ch[ch].remainMs = 0;
  g_ch[ch].sessionStartUs = 0;
  relayApplyChannel(ch);
  g_stats.channelSeconds[ch] += (uint64_t)durationSec;
  g_stats.channelSessions[ch] += 1ULL;
  dayAccumulate((uint64_t)durationSec, 0ULL, 0);
  sessionJournalPush(ch, durationSec, reason);
  statsMarkDirty();
}

static bool channelStart(uint8_t ch) {
  if (g_ch[ch].state == CHS_RUNNING) {
    return true;
  }
  if (g_ch[ch].state == CHS_PAUSED) {
    if (channelRemainingMs(ch) == 0) {
      channelFinish(ch, 3);
      return false;
    }
    g_ch[ch].state = CHS_RUNNING;
    g_ch[ch].sessionStartUs = timeNowUs();
    g_share.lastButtonChannel = ch;
    relayApplyChannel(ch);
    buzzerPlay(BUZ_START);
    return true;
  }
  if (g_cfg.timeModel == TMO_ALLOCATED) {
    if (g_ch[ch].remainMs == 0 && g_poolMs > 0) {
      g_ch[ch].remainMs = g_poolMs;
      g_poolMs = 0;
    }
  }
  uint64_t remaining = channelRemainingMs(ch);
  if (remaining == 0) {
    buzzerPlay(BUZ_ERROR);
    return false;
  }
  g_ch[ch].state = CHS_RUNNING;
  g_ch[ch].usedMs = 0;
  g_ch[ch].sessionStartUs = timeNowUs();
  g_share.lastButtonChannel = ch;
  relayApplyChannel(ch);
  buzzerPlay(BUZ_START);
  return true;
}

static bool channelPause(uint8_t ch) {
  if (g_ch[ch].state != CHS_RUNNING) {
    return false;
  }
  g_ch[ch].state = CHS_PAUSED;
  g_ch[ch].lastBlinkToggleUs = timeNowUs();
  relayApplyChannel(ch);
  buzzerPlay(BUZ_PAUSE);
  return true;
}

static bool channelResume(uint8_t ch) {
  if (g_ch[ch].state != CHS_PAUSED) {
    return false;
  }
  if (channelRemainingMs(ch) == 0) {
    channelFinish(ch, 3);
    buzzerPlay(BUZ_ERROR);
    return false;
  }
  g_ch[ch].state = CHS_RUNNING;
  g_ch[ch].sessionStartUs = timeNowUs();
  relayApplyChannel(ch);
  buzzerPlay(BUZ_START);
  return true;
}

static bool channelStop(uint8_t ch, uint8_t reason) {
  if (g_ch[ch].state == CHS_IDLE) {
    return false;
  }
  channelFinish(ch, reason);
  buzzerPlay(BUZ_STOP);
  return true;
}

static bool channelToggle(uint8_t ch) {
  uint8_t state = g_ch[ch].state;
  if (state == CHS_RUNNING) {
    return channelPause(ch);
  }
  if (state == CHS_PAUSED) {
    return channelResume(ch);
  }
  return channelStart(ch);
}

static bool channelAddTime(uint8_t ch, uint32_t seconds) {
  if (seconds == 0) {
    return false;
  }
  uint64_t addMs = (uint64_t)seconds * 1000ULL;
  uint64_t granted = 0;
  if (g_cfg.timeModel == TMO_ALLOCATED) {
    uint64_t fromPool = (g_poolMs < addMs) ? g_poolMs : addMs;
    if (fromPool > 0) {
      uint64_t cap = creditCapMs();
      uint64_t room = (g_ch[ch].remainMs < cap) ? (cap - g_ch[ch].remainMs) : 0ULL;
      uint64_t applied = (fromPool < room) ? fromPool : room;
      g_poolMs -= applied;
      g_ch[ch].remainMs += applied;
      granted += applied;
    }
  }
  if (g_cfg.bonusEnabled == 1) {
    uint64_t bonusRoomSeconds = (g_cfg.maxBonusSeconds > g_share.bonusUsedSeconds)
                                  ? ((uint64_t)g_cfg.maxBonusSeconds - g_share.bonusUsedSeconds)
                                  : 0ULL;
    uint64_t bonusRoomMs = bonusRoomSeconds * 1000ULL;
    uint64_t stillWanted = (addMs > granted) ? (addMs - granted) : 0ULL;
    uint64_t bonusGrant = (stillWanted < bonusRoomMs) ? stillWanted : bonusRoomMs;
    if (bonusGrant > 0) {
      if (g_cfg.timeModel == TMO_ALLOCATED) {
        uint64_t cap = creditCapMs();
        uint64_t room = (g_ch[ch].remainMs < cap) ? (cap - g_ch[ch].remainMs) : 0ULL;
        uint64_t applied = (bonusGrant < room) ? bonusGrant : room;
        g_ch[ch].remainMs += applied;
        granted += applied;
        g_share.bonusUsedSeconds += applied / 1000ULL;
        g_stats.bonusSeconds += applied / 1000ULL;
      } else {
        uint64_t applied = poolAddMs(bonusGrant);
        granted += applied;
        g_share.bonusUsedSeconds += applied / 1000ULL;
        g_stats.bonusSeconds += applied / 1000ULL;
      }
    }
  }
  if (granted == 0) {
    buzzerPlay(BUZ_ERROR);
    return false;
  }
  g_share.lastButtonChannel = ch;
  buzzerPlay(BUZ_CLICK);
  statsMarkDirty();
  return true;
}

// ======================================================
// ENGINE TICK
// ======================================================

static void engineTick(uint64_t deltaUs) {
  uint64_t totalUs = deltaUs + g_usResidual;
  uint64_t deltaMs = totalUs / 1000ULL;
  g_usResidual = totalUs % 1000ULL;
  if (deltaMs == 0) {
    return;
  }
  uint8_t active = countActive();
  if (active == 0) {
    return;
  }
  if (g_cfg.timeModel == TMO_ALLOCATED) {
    for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
      if (g_ch[i].state != CHS_RUNNING) {
        continue;
      }
      uint64_t take = (g_ch[i].remainMs < deltaMs) ? g_ch[i].remainMs : deltaMs;
      g_ch[i].remainMs -= take;
      g_ch[i].usedMs += take;
      if (g_ch[i].remainMs == 0) {
        channelFinish(i, 1);
      }
    }
  } else {
    uint64_t drain = deltaMs;
    if (g_cfg.timeModel == TMO_SHARED_PER_CHANNEL) {
      drain = deltaMs * (uint64_t)active;
    }
    uint64_t take = (g_poolMs < drain) ? g_poolMs : drain;
    g_poolMs -= take;
    uint64_t share = take / (uint64_t)active;
    uint64_t extra = take % (uint64_t)active;
    uint64_t index = 0;
    for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
      if (g_ch[i].state == CHS_RUNNING) {
        uint64_t turn = (index + (uint64_t)active - (g_drainCursor % (uint64_t)active)) % (uint64_t)active;
        g_ch[i].usedMs += share + ((turn < extra) ? 1ULL : 0ULL);
        index++;
      }
    }
    g_drainCursor = (uint8_t)((g_drainCursor + 1U) % (uint8_t)active);
    if (g_poolMs == 0) {
      for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
        if (g_ch[i].state == CHS_RUNNING) {
          channelFinish(i, 1);
        }
      }
    }
  }
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    if (g_ch[i].state == CHS_RUNNING && g_ch[i].sessionStartUs > 0) {
      uint64_t runSec = (timeNowUs() - g_ch[i].sessionStartUs) / 1000000ULL;
      if (runSec >= (uint64_t)g_cfg.maxRunSeconds) {
        channelFinish(i, 2);
      }
    }
  }
  relayApplyAll();
}


// ======================================================
// BUZZER PATTERNS
// ======================================================

static const BeepStep PAT_BOOT[]    = { { 1800, 90, 60 }, { 2600, 110, 0 } };
static const BeepStep PAT_COIN[]    = { { 2600, 70, 50 }, { 2600, 70, 0 } };
static const BeepStep PAT_START[]   = { { 2200, 80, 0 } };
static const BeepStep PAT_PAUSE[]   = { { 1400, 110, 0 } };
static const BeepStep PAT_STOP[]    = { { 1200, 70, 40 }, { 900, 90, 0 } };
static const BeepStep PAT_ERROR[]   = { { 700, 160, 80 }, { 500, 160, 0 } };
static const BeepStep PAT_CLICK[]   = { { 3000, 45, 0 } };
static const BeepStep PAT_FACTORY[] = { { 500, 220, 120 }, { 500, 220, 120 }, { 500, 220, 0 } };
static const BeepStep PAT_RESET[]   = { { 2600, 90, 60 }, { 1800, 90, 60 }, { 1200, 140, 0 } };
static const BeepStep PAT_WIFI[]    = { { 2000, 60, 40 }, { 2400, 60, 40 }, { 2800, 80, 0 } };

static const BeepStep* buzzerPattern(uint8_t pattern, uint8_t& count) {
  count = 0;
  switch (pattern) {
    case BUZ_BOOT:    count = (uint8_t)(sizeof(PAT_BOOT) / sizeof(BeepStep));    return PAT_BOOT;
    case BUZ_COIN:    count = (uint8_t)(sizeof(PAT_COIN) / sizeof(BeepStep));    return PAT_COIN;
    case BUZ_START:   count = (uint8_t)(sizeof(PAT_START) / sizeof(BeepStep));   return PAT_START;
    case BUZ_PAUSE:   count = (uint8_t)(sizeof(PAT_PAUSE) / sizeof(BeepStep));   return PAT_PAUSE;
    case BUZ_STOP:    count = (uint8_t)(sizeof(PAT_STOP) / sizeof(BeepStep));    return PAT_STOP;
    case BUZ_ERROR:   count = (uint8_t)(sizeof(PAT_ERROR) / sizeof(BeepStep));   return PAT_ERROR;
    case BUZ_CLICK:   count = (uint8_t)(sizeof(PAT_CLICK) / sizeof(BeepStep));   return PAT_CLICK;
    case BUZ_FACTORY: count = (uint8_t)(sizeof(PAT_FACTORY) / sizeof(BeepStep)); return PAT_FACTORY;
    case BUZ_RESET:   count = (uint8_t)(sizeof(PAT_RESET) / sizeof(BeepStep));   return PAT_RESET;
    case BUZ_WIFI:    count = (uint8_t)(sizeof(PAT_WIFI) / sizeof(BeepStep));    return PAT_WIFI;
    default:          return nullptr;
  }
}

static void buzzerSilence() {
  if (g_cfg.buzzerPin > 39) {
    return;
  }
  noTone(g_cfg.buzzerPin);
  g_buzz.active = 0;
  g_buzz.phase = 0;
  g_buzz.index = 0;
}

static void buzzerPlay(uint8_t pattern) {
  if (g_cfg.buzzerEnabled != 1) {
    return;
  }
  uint8_t count = 0;
  const BeepStep* steps = buzzerPattern(pattern, count);
  if (steps == nullptr || count == 0) {
    return;
  }
  g_buzz.steps = steps;
  g_buzz.count = count;
  g_buzz.index = 0;
  g_buzz.phase = 0;
  g_buzz.active = 1;
  g_buzz.nextUs = timeNowUs();
}

static void buzzerTick(uint64_t nowUs) {
  if (g_buzz.active == 0) {
    return;
  }
  if (nowUs < g_buzz.nextUs) {
    return;
  }
  if (g_buzz.index >= g_buzz.count) {
    buzzerSilence();
    return;
  }
  const BeepStep& step = g_buzz.steps[g_buzz.index];
  if (g_buzz.phase == 0) {
    uint16_t freq = (step.freq == 0) ? g_cfg.buzzerFreq : step.freq;
    uint16_t onMs = (step.onMs == 0) ? g_cfg.buzzerBeepMs : step.onMs;
    tone(g_cfg.buzzerPin, freq);
    g_buzz.phase = 1;
    g_buzz.nextUs = nowUs + ((uint64_t)onMs * 1000ULL);
    return;
  }
  noTone(g_cfg.buzzerPin);
  g_buzz.index++;
  g_buzz.phase = 0;
  if (g_buzz.index >= g_buzz.count) {
    buzzerSilence();
    return;
  }
  uint16_t offMs = step.offMs;
  g_buzz.nextUs = nowUs + ((uint64_t)offMs * 1000ULL);
}

// ======================================================
// BUTTON GESTURE ENGINE
// ======================================================

static void buttonHandleSingle(uint8_t ch);
static void buttonHandleDouble(uint8_t ch);
static void buttonHandleLong(uint8_t ch);

static void buttonResetRuntime(uint8_t ch) {
  ButtonRuntime& b = g_btn[ch];
  memset(&b, 0, sizeof(ButtonRuntime));
  uint8_t pin = g_ch[ch].buttonPin;
  uint8_t level = (pin <= 39) ? (uint8_t)digitalRead(pin) : 1u;
  b.rawLevel = level;
  b.stableLevel = level;
  b.candidateLevel = level;
  uint64_t now = timeNowUs();
  b.candidateUs = now;
  b.pressUs = now;
  b.releaseUs = now - ((uint64_t)g_cfg.doubleClickMs * 4000ULL);
  b.shortDueUs = now;
}

static void buttonEdgePressed(uint8_t ch, uint64_t nowUs) {
  ButtonRuntime& b = g_btn[ch];
  uint64_t gapUs = (nowUs >= b.releaseUs) ? (nowUs - b.releaseUs) : 0ULL;
  uint64_t windowUs = (uint64_t)g_cfg.doubleClickMs * 1000ULL;
  if (gapUs <= windowUs && b.shortPending == 1) {
    b.shortPending = 0;
    b.clicks = 2;
  } else if (gapUs <= windowUs) {
    b.clicks = (uint8_t)((b.clicks < 2) ? (b.clicks + 1) : 2);
  } else {
    b.clicks = 1;
  }
  b.pressUs = nowUs;
  b.longSent = 0;
  if (b.clicks >= 2) {
    b.clicks = 0;
    b.longSent = 1;
    buttonHandleDouble(ch);
  }
}

static void buttonEdgeReleased(uint8_t ch, uint64_t nowUs) {
  ButtonRuntime& b = g_btn[ch];
  b.releaseUs = nowUs;
  if (b.longSent == 1) {
    b.clicks = 0;
    b.shortPending = 0;
    return;
  }
  if (b.clicks >= 2) {
    b.clicks = 0;
    b.shortPending = 0;
    return;
  }
  b.shortPending = 1;
  b.shortDueUs = nowUs + ((uint64_t)g_cfg.doubleClickMs * 1000ULL);
}

static void buttonPollChannel(uint8_t ch, uint64_t nowUs) {
  ButtonRuntime& b = g_btn[ch];
  uint8_t pin = g_ch[ch].buttonPin;
  if (pin > 39) {
    return;
  }
  uint8_t level = (uint8_t)digitalRead(pin);
  b.rawLevel = level;
  if (level != b.stableLevel) {
    if (level != b.candidateLevel) {
      b.candidateLevel = level;
      b.candidateUs = nowUs;
    } else {
      uint64_t heldUs = nowUs - b.candidateUs;
      if (heldUs >= ((uint64_t)g_cfg.debounceMs * 1000ULL)) {
        b.stableLevel = level;
        if (level == LOW) {
          buttonEdgePressed(ch, nowUs);
        } else {
          buttonEdgeReleased(ch, nowUs);
        }
      }
    }
  } else {
    b.candidateLevel = level;
    b.candidateUs = nowUs;
  }

  if (b.stableLevel == LOW && b.longSent == 0) {
    uint64_t heldUs = nowUs - b.pressUs;
    if (heldUs >= ((uint64_t)g_cfg.longPressMs * 1000ULL)) {
      b.longSent = 1;
      b.shortPending = 0;
      b.clicks = 0;
      buttonHandleLong(ch);
    }
  }

  if (b.shortPending == 1 && nowUs >= b.shortDueUs) {
    b.shortPending = 0;
    b.clicks = 0;
    buttonHandleSingle(ch);
  }
}

static void buttonPollAll(uint64_t nowUs) {
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    buttonPollChannel(i, nowUs);
  }
}

static void buttonHandleSingle(uint8_t ch) {
  g_share.lastButtonChannel = ch;
  if (g_cfg.timeModel == TMO_ALLOCATED && g_poolMs > 0) {
    uint64_t cap = creditCapMs();
    uint64_t room = (g_ch[ch].remainMs < cap) ? cap - g_ch[ch].remainMs : 0;
    uint64_t transfer = (g_poolMs < room) ? g_poolMs : room;
    g_ch[ch].remainMs += transfer;
    g_poolMs -= transfer;
  }
  if (g_ch[ch].state == CHS_RUNNING) {
    buzzerPlay(BUZ_CLICK);
    return;
  }
  if (g_ch[ch].state == CHS_PAUSED) {
    channelResume(ch);
    return;
  }
  channelStart(ch);
}

static void buttonHandleDouble(uint8_t ch) {
  g_share.lastButtonChannel = ch;
  if (g_ch[ch].state == CHS_RUNNING) {
    channelPause(ch);
    return;
  }
  if (g_ch[ch].state == CHS_PAUSED) {
    channelResume(ch);
    return;
  }
  channelStart(ch);
}

static void buttonHandleLong(uint8_t ch) {
  g_share.lastButtonChannel = ch;
  channelAddTime(ch, g_cfg.longPressAddSeconds);
}

// ======================================================
// COIN AND BILL ACCEPTOR INPUT
// ======================================================

#define COIN_RING_LEN 16

static volatile uint64_t g_coinRingTs[COIN_RING_LEN];
static volatile uint8_t  g_coinRingHead;
static volatile uint8_t  g_coinRingTail;
static volatile uint32_t g_coinRingDropped;

static void IRAM_ATTR coinPulseIsr() {
  uint8_t head = g_coinRingHead;
  uint8_t next = (uint8_t)((head + 1) % COIN_RING_LEN);
  if (next == g_coinRingTail) {
    g_coinRingDropped = g_coinRingDropped + 1;
    return;
  }
  g_coinRingTs[head] = (uint64_t)esp_timer_get_time();
  g_coinRingHead = next;
}

static void coinInputAttach() {
  if (g_cfg.coinPin > 39) {
    return;
  }
  pinMode(g_cfg.coinPin, INPUT_PULLUP);
  g_coinRingHead = 0;
  g_coinRingTail = 0;
  attachInterrupt(digitalPinToInterrupt(g_cfg.coinPin), coinPulseIsr, FALLING);
  g_coinPinAttached = 1;
  g_coinPinCurrent = g_cfg.coinPin;
}

static void coinInputDetach() {
  if (g_coinPinAttached == 1 && g_coinPinCurrent <= 39) {
    detachInterrupt(digitalPinToInterrupt(g_coinPinCurrent));
  }
  g_coinPinAttached = 0;
}

// ======================================================
// COIN PRESET RESOLUTION
// ======================================================

static CoinAward coinResolve(uint32_t pulses) {
  CoinAward award;
  award.seconds = 0;
  award.valueCentavos = 0;
  award.matched = 0;
  if (pulses == 0 || g_cfg.coinMapCount == 0) {
    return award;
  }
  for (uint8_t i = 0; i < g_cfg.coinMapCount; ++i) {
    if (g_cfg.coinMap[i].pulses == pulses) {
      award.seconds = (uint32_t)g_cfg.coinMap[i].minutes * 60UL;
      award.valueCentavos = g_cfg.coinMap[i].valueCentavos;
      award.matched = 1;
      return award;
    }
  }
  uint8_t bestIdx = 0;
  uint32_t bestPulses = 0;
  for (uint8_t i = 0; i < g_cfg.coinMapCount; ++i) {
    if (g_cfg.coinMap[i].pulses <= pulses && g_cfg.coinMap[i].pulses > bestPulses) {
      bestPulses = g_cfg.coinMap[i].pulses;
      bestIdx = i;
    }
  }
  if (bestPulses == 0) {
    return award;
  }
  uint64_t baseSec = (uint64_t)g_cfg.coinMap[bestIdx].minutes * 60ULL;
  uint64_t basePulses = (uint64_t)g_cfg.coinMap[bestIdx].pulses;
  uint64_t extraPulses = (uint64_t)pulses - basePulses;
  uint64_t perPulseSec = (basePulses > 0) ? (baseSec / basePulses) : 0ULL;
  uint64_t total = baseSec + (extraPulses * perPulseSec);
  if (total < baseSec) {
    total = UINT32_MAX;
  }
  uint64_t baseValue = (uint64_t)g_cfg.coinMap[bestIdx].valueCentavos;
  uint64_t perPulseValue = (basePulses > 0) ? (baseValue / basePulses) : 0ULL;
  uint64_t totalValue = baseValue + (extraPulses * perPulseValue);
  if (total > UINT32_MAX) {
    total = UINT32_MAX;
  }
  if (totalValue > 100000000ULL) {
    totalValue = 100000000ULL;
  }
  award.seconds = (uint32_t)total;
  award.valueCentavos = (uint32_t)totalValue;
  award.matched = 2;
  return award;
}

// ======================================================
// COIN PULSE OUTPUT DRIVER
// ======================================================

static void pulseOutputBegin() {
  g_pulseOutActive = 0;
  g_pulseOutPending = 0;
  g_pulseOutOffUs = 0;
  if (g_cfg.pulseOutPin > 39) {
    return;
  }
  digitalWrite(g_cfg.pulseOutPin, LOW);
  pinMode(g_cfg.pulseOutPin, OUTPUT);
  digitalWrite(g_cfg.pulseOutPin, LOW);
}

static void pulseOutputTick(uint64_t nowUs) {
  if (g_cfg.pulseOutEnabled != 1 || g_cfg.pulseOutPin > 39) {
    return;
  }
  if (g_pulseOutActive == 1) {
    if (nowUs >= g_pulseOutOffUs) {
      digitalWrite(g_cfg.pulseOutPin, LOW);
      g_pulseOutActive = 0;
    }
    return;
  }
  if (g_pulseOutPending > 0) {
    g_pulseOutPending--;
    digitalWrite(g_cfg.pulseOutPin, HIGH);
    g_pulseOutActive = 1;
    g_pulseOutOffUs = nowUs + ((uint64_t)g_cfg.pulseOutWidthMs * 1000ULL);
  }
}

// ======================================================
// COIN GROUP PROCESSING
// ======================================================

static void coinAcceptAward(const CoinAward& award, uint32_t pulses) {
  uint64_t grantedSec = creditAddSeconds(award.seconds);
  g_stats.coins += 1ULL;
  g_stats.pulses += (uint64_t)pulses;
  g_stats.revenueCentavos += (uint64_t)award.valueCentavos;
  g_share.coinPulsesTotal += pulses;
  dayAccumulate(0ULL, (uint64_t)award.valueCentavos, 1u);
  g_share.bonusUsedSeconds = 0;
  g_share.lastCoinUs = timeNowUs();
  buzzerPlay(BUZ_COIN);
  snprintf(g_overlay[0], sizeof(g_overlay[0]), "COIN  %02u PULSE", (unsigned)pulses);
  snprintf(g_overlay[1], sizeof(g_overlay[1]), "CREDIT +%02u:%02u",
           (unsigned)(grantedSec / 60ULL), (unsigned)(grantedSec % 60ULL));
  uint64_t totalSec = g_poolMs / 1000ULL;
  if (totalSec > 359999ULL) {
    totalSec = 359999ULL;
  }
  snprintf(g_overlay[2], sizeof(g_overlay[2]), "TOTAL   %u:%02u",
           (unsigned)(totalSec / 60ULL), (unsigned)(totalSec % 60ULL));
  snprintf(g_overlay[3], sizeof(g_overlay[3]), "SELECT  FUNCTION");
  g_overlayActive = 1;
  g_overlayUntilUs = timeNowUs() + 2500000ULL;
  g_lcdNextRefreshUs = timeNowUs();
  statsMarkDirty();
}

static void coinProcessRing(uint64_t nowUs) {
  uint64_t minGapUs = (uint64_t)g_cfg.coinMinPulseGapMs * 1000ULL;
  uint64_t groupGapUs = (uint64_t)g_cfg.coinGroupGapMs * 1000ULL;

  while (g_coinRingTail != g_coinRingHead) {
    uint8_t tail = g_coinRingTail;
    uint64_t ts = g_coinRingTs[tail];
    g_coinRingTail = (uint8_t)((tail + 1) % COIN_RING_LEN);
    if (g_coinGroupActive == 1 && g_coinLastPulseUs > 0 && ts >= g_coinLastPulseUs &&
        (ts - g_coinLastPulseUs) < minGapUs) {
      continue;
    }
    if (g_coinGroupActive == 1 && ts >= g_coinLastPulseUs &&
        (ts - g_coinLastPulseUs) >= groupGapUs) {
      uint32_t pulses = g_coinGroupPulses;
      g_coinGroupActive = 0;
      g_coinGroupPulses = 0;
      g_coinGroupStartUs = 0;
      CoinAward award = coinResolve(pulses);
      if (award.seconds > 0) {
        coinAcceptAward(award, pulses);
      } else {
        buzzerPlay(BUZ_ERROR);
      }
    }
    if (g_coinGroupActive == 0) {
      g_coinGroupActive = 1;
      g_coinGroupStartUs = ts;
      g_coinGroupPulses = 0;
    }
    if (g_coinGroupPulses < (uint32_t)g_cfg.coinMaxPulses) {
      g_coinGroupPulses++;
      if (g_cfg.pulseOutEnabled == 1 && g_pulseOutPending < 64) {
        g_pulseOutPending++;
      }
    }
    g_coinLastPulseUs = ts;
  }

  if (g_coinGroupActive == 1) {
    uint64_t reference = nowUs;
    if (g_coinLastPulseUs > reference) {
      reference = g_coinLastPulseUs;
    }
    if (reference >= g_coinLastPulseUs && (reference - g_coinLastPulseUs) >= groupGapUs) {
      uint32_t pulses = g_coinGroupPulses;
      g_coinGroupActive = 0;
      g_coinGroupPulses = 0;
      g_coinGroupStartUs = 0;
      if (pulses > 0) {
        CoinAward award = coinResolve(pulses);
        if (award.seconds > 0) {
          coinAcceptAward(award, pulses);
        } else {
          buzzerPlay(BUZ_ERROR);
        }
      }
    }
  }
}

// ======================================================
// FACTORY RESET SUPERVISOR
// ======================================================

static void factoryWipeAndRestart() {
  g_factoryHoldUs = 0;
  g_share.factoryHoldActive = 0;
  buzzerPlay(BUZ_FACTORY);
  snprintf(g_overlay[0], sizeof(g_overlay[0]), "FACTORY RESET");
  snprintf(g_overlay[1], sizeof(g_overlay[1]), "ERASING STORAGE");
  g_overlay[2][0] = '\0';
  snprintf(g_overlay[3], sizeof(g_overlay[3]), "RESTARTING");
  g_overlayActive = 1;
  g_overlayUntilUs = timeNowUs() + 1500000ULL;
  lcdFlushOverlayNow();
  relayForceOffAll();
  buzzerSilence();
  if (g_nvsReady) {
    g_prefs.clear();
    g_prefs.end();
    g_nvsReady = 0;
  }
  ESP.restart();
}

static void factoryResetPoll(uint64_t nowUs) {
  if (g_cfg.bootPin > 39) {
    return;
  }
  bool pressed = (digitalRead(g_cfg.bootPin) == LOW);
  if (g_bootComplete == 0) {
    if (pressed) {
      g_factoryHoldArmed = 0;
    } else {
      g_factoryHoldArmed = 1;
    }
    return;
  }
  if (g_factoryHoldArmed == 0) {
    if (!pressed) {
      g_factoryHoldArmed = 1;
    }
    return;
  }
  if (!pressed) {
    if (g_factoryHoldUs != 0) {
      g_factoryHoldUs = 0;
      g_share.factoryHoldActive = 0;
      g_lcdNextRefreshUs = timeNowUs();
    }
    return;
  }
  if (g_factoryHoldUs == 0) {
    g_factoryHoldUs = nowUs;
    g_share.factoryHoldActive = 1;
    buzzerPlay(BUZ_CLICK);
    g_lcdNextRefreshUs = timeNowUs();
    return;
  }
  uint64_t heldMs = (nowUs - g_factoryHoldUs) / 1000ULL;
  if (heldMs >= (uint64_t)g_cfg.factoryHoldMs) {
    g_factoryConfirmed = 1;
    factoryWipeAndRestart();
  }
}

// ======================================================
// HEAP AND HEALTH GUARDIAN
// ======================================================

static void healthTick(uint64_t nowUs) {
  if (nowUs - g_lastHealthUs < HEALTH_INTERVAL_US) {
    return;
  }
  g_lastHealthUs = nowUs;
  if (g_cfg.lowHeapRestart != 1) {
    g_lowHeapStrikes = 0;
    return;
  }
  uint32_t freeHeap = ESP.getFreeHeap();
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < g_cfg.lowHeapBytes || largest < (g_cfg.lowHeapBytes / 2)) {
    if (g_lowHeapStrikes < 255) {
      g_lowHeapStrikes++;
    }
  } else {
    g_lowHeapStrikes = 0;
  }
  if (g_lowHeapStrikes >= 4) {
    g_lowHeapStrikes = 0;
    relayForceOffAll();
    statsSaveNow();
    ESP.restart();
  }
}


// ======================================================
// LCD FORWARD DECLARATIONS
// ======================================================

static void lcdFlushOverlayNow();
static void lcdRenderRows(uint64_t nowUs);
static void lcdInit();
static void lcdShutdown();

// ======================================================
// LCD I2C TRANSPORT
// ======================================================

static bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  uint8_t err = Wire.endTransmission();
  return (err == 0);
}

static void lcdShutdown() {
  if (g_lcd != nullptr) {
    g_lcd->noBacklight();
    g_lcd->clear();
    g_lcd = nullptr;
  }
  memset(g_lcdShadow, 0, sizeof(g_lcdShadow));
  memset(g_lcdRendered, 0, sizeof(g_lcdRendered));
  g_share.lcdPresent = 0;
}

static void lcdInit() {
  lcdShutdown();
  if (g_cfg.lcdSda > 39 || g_cfg.lcdScl > 39) {
    return;
  }
  Wire.end();
  Wire.begin(g_cfg.lcdSda, g_cfg.lcdScl, 100000UL);
  Wire.setTimeOut(40);
  g_lcdInitAttempts++;
  if (!i2cProbe(g_cfg.lcdAddr)) {
    if (g_lcdFailCount < 255) {
      g_lcdFailCount++;
    }
    g_lcdNextProbeUs = timeNowUs() + LCD_PROBE_INTERVAL_US;
    return;
  }
  g_lcd = new (g_lcdStorage) LiquidCrystal_I2C(g_cfg.lcdAddr, g_cfg.lcdCols, g_cfg.lcdRows);
  g_lcd->init();
  if (g_cfg.lcdBacklight == 1) {
    g_lcd->backlight();
  } else {
    g_lcd->noBacklight();
  }
  g_lcd->clear();
  memset(g_lcdShadow, 0, sizeof(g_lcdShadow));
  memset(g_lcdRendered, 0, sizeof(g_lcdRendered));
  g_lcdFailCount = 0;
  g_share.lcdPresent = 1;
  g_lcdNextProbeUs = timeNowUs() + LCD_PROBE_INTERVAL_US;
  g_lcdNextRefreshUs = timeNowUs();
}

// ======================================================
// LCD TEXT FORMATTERS
// ======================================================

static void lcdFormatTime(uint64_t remainMs, char* out, size_t outLen) {
  uint64_t sec = remainMs / 1000ULL;
  if (sec > LCD_MAX_SECONDS) {
    sec = LCD_MAX_SECONDS;
  }
  uint32_t total = (uint32_t)sec;
  if (total < 6000UL) {
    snprintf(out, outLen, "%02u:%02u", (unsigned)(total / 60UL), (unsigned)(total % 60UL));
  } else {
    snprintf(out, outLen, "%u:%02u", (unsigned)(total / 3600UL), (unsigned)((total / 60UL) % 60UL));
  }
}

static const char* lcdStateText(uint8_t ch) {
  if (g_ch[ch].state == CHS_RUNNING) {
    return "RUN ";
  }
  if (g_ch[ch].state == CHS_PAUSED) {
    return "PAUS";
  }
  if (channelRemainingMs(ch) > 0) {
    return "REDY";
  }
  return "OFF ";
}

static void lcdPadRow(char* row, size_t len) {
  if (len < 2) {
    return;
  }
  size_t target = (size_t)g_cfg.lcdCols;
  if (target > 20) {
    target = 20;
  }
  if (target + 1 > len) {
    target = len - 1;
  }
  size_t used = strlen(row);
  for (size_t i = used; i < target; ++i) {
    row[i] = ' ';
  }
  row[target] = '\0';
}

// ======================================================
// LCD ROW RENDERER
// ======================================================

static void lcdBuildChannelRow(uint8_t ch, uint64_t nowUs, char* out, size_t outLen) {
  char timeText[8];
  bool blinkOff = false;
  if (g_ch[ch].state == CHS_PAUSED) {
    uint64_t blinkIntervalUs = (uint64_t)g_cfg.lcdBlinkMs * 1000ULL;
    uint64_t sinceUs = (nowUs >= g_ch[ch].lastBlinkToggleUs) ? (nowUs - g_ch[ch].lastBlinkToggleUs) : 0ULL;
    if (blinkIntervalUs > 0) {
      blinkOff = ((sinceUs / blinkIntervalUs) % 2ULL) == 1ULL;
    }
  }
  uint64_t remain = channelRemainingMs(ch);
  if (g_ch[ch].state == CHS_IDLE && remain == 0) {
    safeCopyString(timeText, sizeof(timeText), "--:--");
  } else {
    lcdFormatTime(remain, timeText, sizeof(timeText));
  }
  if (blinkOff) {
    safeCopyString(timeText, sizeof(timeText), "     ");
  }
  snprintf(out, outLen, "%-5s %s %s", CHANNEL_NAME[ch], timeText, lcdStateText(ch));
  lcdPadRow(out, outLen);
}

static void lcdRenderRows(uint64_t nowUs) {
  uint8_t rows = g_cfg.lcdRows;
  if (rows > 4) {
    rows = 4;
  }
  for (uint8_t r = 0; r < 4; ++r) {
    if (r < rows) {
      lcdBuildChannelRow(r, nowUs, g_lcdRendered[r], sizeof(g_lcdRendered[r]));
    } else {
      g_lcdRendered[r][0] = '\0';
    }
  }
  if (g_factoryHoldUs != 0) {
    uint64_t heldMs = (nowUs - g_factoryHoldUs) / 1000ULL;
    uint32_t remainMs = (heldMs < (uint64_t)g_cfg.factoryHoldMs)
                          ? (uint32_t)((uint64_t)g_cfg.factoryHoldMs - heldMs)
                          : 0U;
    for (uint8_t r = 0; r < 4; ++r) {
      g_lcdRendered[r][0] = '\0';
    }
    snprintf(g_lcdRendered[0], sizeof(g_lcdRendered[0]), "   FACTORY RESET");
    snprintf(g_lcdRendered[1], sizeof(g_lcdRendered[1]), "  HOLD %u.%us MORE",
             (unsigned)(remainMs / 1000UL), (unsigned)((remainMs % 1000UL) / 100UL));
    snprintf(g_lcdRendered[2], sizeof(g_lcdRendered[2]), "   RELEASE TO ABORT");
    snprintf(g_lcdRendered[3], sizeof(g_lcdRendered[3]), "  ALL DATA ERASED");
    for (uint8_t r = 0; r < 4; ++r) {
      lcdPadRow(g_lcdRendered[r], sizeof(g_lcdRendered[r]));
    }
    return;
  }
  if (g_overlayActive == 1 && nowUs < g_overlayUntilUs) {
    for (uint8_t r = 0; r < 4; ++r) {
      safeCopyString(g_lcdRendered[r], sizeof(g_lcdRendered[r]), g_overlay[r]);
      lcdPadRow(g_lcdRendered[r], sizeof(g_lcdRendered[r]));
    }
    return;
  }
  g_overlayActive = 0;
}

// ======================================================
// LCD DIFFERENTIAL FLUSH
// ======================================================

static void lcdWriteRow(uint8_t row, const char* text) {
  if (g_lcd == nullptr) {
    return;
  }
  char padded[24];
  safeCopyString(padded, sizeof(padded), text);
  size_t used = strlen(padded);
  for (size_t i = used; i < (size_t)g_cfg.lcdCols && i + 1 < sizeof(padded); ++i) {
    padded[i] = ' ';
  }
  padded[g_cfg.lcdCols] = '\0';
  g_lcd->setCursor(0, row);
  g_lcd->print(padded);
}

static void lcdFlushRows() {
  if (g_lcd == nullptr) {
    return;
  }
  for (uint8_t r = 0; r < 4; ++r) {
    if (r >= g_cfg.lcdRows) {
      continue;
    }
    if (strcmp(g_lcdRendered[r], g_lcdShadow[r]) != 0) {
      lcdWriteRow(r, g_lcdRendered[r]);
      safeCopyString(g_lcdShadow[r], sizeof(g_lcdShadow[r]), g_lcdRendered[r]);
    }
  }
}

static void lcdFlushOverlayNow() {
  lcdRenderRows(timeNowUs());
  lcdFlushRows();
}

static void lcdTick(uint64_t nowUs) {
  if (g_share.lcdPresent == 0) {
    if (nowUs >= g_lcdNextProbeUs) {
      lcdInit();
    }
    return;
  }
  if (g_lcd == nullptr) {
    return;
  }
  if (nowUs >= g_lcdNextProbeUs) {
    g_lcdNextProbeUs = nowUs + LCD_PROBE_INTERVAL_US;
    if (i2cProbe(g_cfg.lcdAddr)) {
      g_lcdFailCount = 0;
    } else {
      if (g_lcdFailCount < 255) {
        g_lcdFailCount++;
      }
      if (g_lcdFailCount >= 3) {
        lcdShutdown();
        g_lcdNextProbeUs = nowUs + LCD_PROBE_INTERVAL_US;
        return;
      }
    }
  }
  if (nowUs < g_lcdNextRefreshUs) {
    return;
  }
  g_lcdNextRefreshUs = nowUs + ((uint64_t)g_cfg.lcdRefreshMs * 1000ULL);
  lcdRenderRows(nowUs);
  lcdFlushRows();
}


// ======================================================
// WI-FI ACCESS POINT MANAGER
// ======================================================

static const IPAddress AP_LOCAL_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

static void apStopServices() {
  g_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  g_share.apRunning = 0;
  g_apStationCount = 0;
}

static bool apStartServices() {
  apStopServices();
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAPConfig(AP_LOCAL_IP, AP_GATEWAY, AP_SUBNET)) {
    g_wifiFailCount++;
    return false;
  }
  const char* pass = (strlen(g_cfg.apPass) >= 8) ? g_cfg.apPass : nullptr;
  bool ok = WiFi.softAP(g_cfg.apSsid, pass, (int)g_cfg.apChannel, (int)g_cfg.apHidden, (int)g_cfg.apMaxClients);
  if (!ok) {
    g_wifiFailCount++;
    g_share.apRunning = 0;
    return false;
  }
  WiFi.softAPsetHostname("carwash");
  bool dnsOk = g_dns.start(53, "*", AP_LOCAL_IP);
  g_share.apRunning = (dnsOk ? 1 : 2);
  g_apStationCount = WiFi.softAPgetStationNum();
  g_wifiFailCount = 0;
  buzzerPlay(BUZ_WIFI);
  return true;
}

static void apRestartDeferred(uint32_t delayMs) {
  g_apRestartAtUs = timeNowUs() + ((uint64_t)delayMs * 1000ULL);
  g_apRestartPending = 1;
}

static void apGuardTick(uint64_t nowUs) {
  if (g_apRestartPending == 1 && nowUs >= g_apRestartAtUs) {
    g_apRestartPending = 0;
    apStartServices();
    g_apRestartCount++;
    g_lcdNextRefreshUs = nowUs;
    return;
  }
  if (nowUs - g_lastApGuardUs < AP_GUARD_INTERVAL_US) {
    return;
  }
  g_lastApGuardUs = nowUs;
  if (g_apRestartPending == 1) {
    return;
  }
  wifi_mode_t mode = WiFi.getMode();
  if (g_share.apRunning == 0 || mode != WIFI_AP) {
    apStartServices();
    g_apRestartCount++;
    return;
  }
  g_apStationCount = WiFi.softAPgetStationNum();
}

// ======================================================
// CAPTIVE PORTAL RESPONDER
// ======================================================

static void captiveRedirect() {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.sendHeader("Connection", "close");
  g_server.sendHeader("Location", "http://192.168.4.1/", true);
  g_server.send(302, "text/plain", "");
}

static void handleCaptiveAndroid204() {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.sendHeader("Location", "http://192.168.4.1/", true);
  g_server.send(302, "text/plain", "");
}

static void handleCaptiveApple() {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(200, "text/html; charset=utf-8",
                "<!DOCTYPE html><html><head><meta http-equiv=\"refresh\" content=\"0; url=http://192.168.4.1/\">"
                "<title>Carwash Controller</title></head><body>"
                "<p>Redirecting to the controller panel.</p>"
                "<p><a href=\"http://192.168.4.1/\">Open 192.168.4.1</a></p></body></html>");
}

static void handleCaptiveWindows() {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(200, "text/html; charset=utf-8",
                "<HTML><HEAD><TITLE>Carwash Controller</TITLE>"
                "<meta http-equiv=\"refresh\" content=\"0; url=http://192.168.4.1/\"></HEAD>"
                "<BODY>Redirecting to the controller panel.</BODY></HTML>");
}

// ======================================================
// JSON WRITER
// ======================================================

static size_t  g_jsonLen;
static uint8_t g_jsonNeedComma;

static void jsonReset() {
  g_jsonLen = 0;
  g_jsonNeedComma = 0;
  g_json[0] = '\0';
}

static void jsonAppendRaw(const char* text) {
  if (text == nullptr) {
    return;
  }
  size_t len = strlen(text);
  if (g_jsonLen + len + 1 >= JSON_BUFFER_LEN) {
    return;
  }
  memcpy(g_json + g_jsonLen, text, len);
  g_jsonLen += len;
  g_json[g_jsonLen] = '\0';
}

static void jsonAppendChar(char c) {
  if (g_jsonLen + 2 >= JSON_BUFFER_LEN) {
    return;
  }
  g_json[g_jsonLen++] = c;
  g_json[g_jsonLen] = '\0';
}

static void jsonAppendFormat(const char* fmt, ...) {
  if (g_jsonLen + 2 >= JSON_BUFFER_LEN) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(g_json + g_jsonLen, JSON_BUFFER_LEN - g_jsonLen, fmt, args);
  va_end(args);
  if (written < 0) {
    return;
  }
  size_t roomAvail = JSON_BUFFER_LEN - g_jsonLen - 1;
  if ((size_t)written > roomAvail) {
    g_jsonLen = JSON_BUFFER_LEN - 1;
  } else {
    g_jsonLen += (size_t)written;
  }
  g_json[g_jsonLen] = '\0';
}

static void jsonAppendEscaped(const char* text) {
  jsonAppendChar('"');
  if (text != nullptr) {
    for (size_t i = 0; text[i] != '\0'; ++i) {
      unsigned char c = (unsigned char)text[i];
      if (c == '"' || c == '\\') {
        jsonAppendChar('\\');
        jsonAppendChar((char)c);
      } else if (c < 0x20) {
        jsonAppendFormat("\\u%04x", (unsigned)c);
      } else {
        jsonAppendChar((char)c);
      }
    }
  }
  jsonAppendChar('"');
}

static void jsonSeparator() {
  if (g_jsonNeedComma == 1) {
    jsonAppendChar(',');
  }
  g_jsonNeedComma = 0;
}

static void jsonObjBegin() {
  jsonSeparator();
  jsonAppendChar('{');
}

static void jsonObjEnd() {
  jsonAppendChar('}');
  g_jsonNeedComma = 1;
}

static void jsonArrBegin() {
  jsonSeparator();
  jsonAppendChar('[');
}

static void jsonArrEnd() {
  jsonAppendChar(']');
  g_jsonNeedComma = 1;
}

static void jsonKey(const char* key) {
  jsonSeparator();
  jsonAppendChar('"');
  jsonAppendRaw(key);
  jsonAppendRaw("\":");
}

static void jsonU64Val(uint64_t value) {
  jsonSeparator();
  jsonAppendFormat("%llu", (unsigned long long)value);
  g_jsonNeedComma = 1;
}

static void jsonU32Val(uint32_t value) {
  jsonSeparator();
  jsonAppendFormat("%lu", (unsigned long)value);
  g_jsonNeedComma = 1;
}

static void jsonStrVal(const char* value) {
  jsonSeparator();
  jsonAppendEscaped(value);
  g_jsonNeedComma = 1;
}

static void jsonU64(const char* key, uint64_t value) {
  jsonKey(key);
  jsonU64Val(value);
}

static void jsonU32(const char* key, uint32_t value) {
  jsonKey(key);
  jsonU32Val(value);
}

static void jsonStr(const char* key, const char* value) {
  jsonKey(key);
  jsonStrVal(value);
}

static void jsonSend() {
  g_server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  g_server.send(200, "application/json", g_json);
}

static void jsonSendError(uint16_t code, const char* message) {
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 0u);
  jsonStr("error", message);
  jsonObjEnd();
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(code, "application/json", g_json);
}

static void jsonSendOk() {
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonObjEnd();
  jsonSend();
}


// ======================================================
// CONFIGURATION FIELD TABLE
// ======================================================

#define FD_STR(k, f, l) { k, FT_STR, (uint8_t)(l), (uint16_t)offsetof(Config, f), 0u, 0u }
#define FD_U8(k, f, lo, hi) { k, FT_U8, 0, (uint16_t)offsetof(Config, f), (uint32_t)(lo), (uint32_t)(hi) }
#define FD_U16(k, f, lo, hi) { k, FT_U16, 0, (uint16_t)offsetof(Config, f), (uint32_t)(lo), (uint32_t)(hi) }
#define FD_U32(k, f, lo, hi) { k, FT_U32, 0, (uint16_t)offsetof(Config, f), (uint32_t)(lo), (uint32_t)(hi) }

static const FieldDef g_fields[] = {
  FD_STR("apSsid", apSsid, 33),
  FD_STR("apPass", apPass, 64),
  FD_STR("adminPin", adminPin, 9),
  FD_U8("apChannel", apChannel, 1, 13),
  FD_U8("apHidden", apHidden, 0, 1),
  FD_U8("apMaxClients", apMaxClients, 1, 8),
  FD_U8("timeModel", timeModel, 0, 2),
  FD_U8("relayActiveLow", relayActiveLow, 0, 1),
  FD_U8("relayPin0", relayPin[0], 0, 39),
  FD_U8("relayPin1", relayPin[1], 0, 39),
  FD_U8("relayPin2", relayPin[2], 0, 39),
  FD_U8("relayPin3", relayPin[3], 0, 39),
  FD_U8("btnPin0", btnPin[0], 0, 39),
  FD_U8("btnPin1", btnPin[1], 0, 39),
  FD_U8("btnPin2", btnPin[2], 0, 39),
  FD_U8("btnPin3", btnPin[3], 0, 39),
  FD_U8("coinPin", coinPin, 0, 39),
  FD_U8("pulseOutPin", pulseOutPin, 0, 39),
  FD_U8("pulseOutEnabled", pulseOutEnabled, 0, 1),
  FD_U16("pulseOutWidthMs", pulseOutWidthMs, 10, 1000),
  FD_U8("buzzerPin", buzzerPin, 0, 39),
  FD_U8("buzzerEnabled", buzzerEnabled, 0, 1),
  FD_U16("buzzerFreq", buzzerFreq, 100, 10000),
  FD_U16("buzzerBeepMs", buzzerBeepMs, 10, 1000),
  FD_U16("buzzerErrorFreq", buzzerErrorFreq, 100, 10000),
  FD_U8("bootPin", bootPin, 0, 39),
  FD_U16("factoryHoldMs", factoryHoldMs, 1000, 30000),
  FD_U16("longPressMs", longPressMs, 200, 5000),
  FD_U16("doubleClickMs", doubleClickMs, 100, 2000),
  FD_U16("debounceMs", debounceMs, 5, 200),
  FD_U16("longPressAddSeconds", longPressAddSeconds, 0, 3600),
  FD_U8("bonusEnabled", bonusEnabled, 0, 1),
  FD_U32("maxBonusSeconds", maxBonusSeconds, 0, 86400),
  FD_U16("coinMinPulseGapMs", coinMinPulseGapMs, 5, 500),
  FD_U16("coinGroupGapMs", coinGroupGapMs, 50, 5000),
  FD_U16("coinMaxPulses", coinMaxPulses, 1, 200),
  FD_U8("coinMapCount", coinMapCount, 1, COIN_MAP_MAX),
  FD_U16("coinPulses0", coinMap[0].pulses, 1, 200),
  FD_U16("coinPulses1", coinMap[1].pulses, 1, 200),
  FD_U16("coinPulses2", coinMap[2].pulses, 1, 200),
  FD_U16("coinPulses3", coinMap[3].pulses, 1, 200),
  FD_U16("coinPulses4", coinMap[4].pulses, 0, 200),
  FD_U16("coinPulses5", coinMap[5].pulses, 0, 200),
  FD_U16("coinPulses6", coinMap[6].pulses, 0, 200),
  FD_U16("coinPulses7", coinMap[7].pulses, 0, 200),
  FD_U16("coinMinutes0", coinMap[0].minutes, 1, 1000),
  FD_U16("coinMinutes1", coinMap[1].minutes, 1, 1000),
  FD_U16("coinMinutes2", coinMap[2].minutes, 1, 1000),
  FD_U16("coinMinutes3", coinMap[3].minutes, 1, 1000),
  FD_U16("coinMinutes4", coinMap[4].minutes, 0, 1000),
  FD_U16("coinMinutes5", coinMap[5].minutes, 0, 1000),
  FD_U16("coinMinutes6", coinMap[6].minutes, 0, 1000),
  FD_U16("coinMinutes7", coinMap[7].minutes, 0, 1000),
  FD_U32("coinValue0", coinMap[0].valueCentavos, 0, 1000000),
  FD_U32("coinValue1", coinMap[1].valueCentavos, 0, 1000000),
  FD_U32("coinValue2", coinMap[2].valueCentavos, 0, 1000000),
  FD_U32("coinValue3", coinMap[3].valueCentavos, 0, 1000000),
  FD_U32("coinValue4", coinMap[4].valueCentavos, 0, 1000000),
  FD_U32("coinValue5", coinMap[5].valueCentavos, 0, 1000000),
  FD_U32("coinValue6", coinMap[6].valueCentavos, 0, 1000000),
  FD_U32("coinValue7", coinMap[7].valueCentavos, 0, 1000000),
  FD_U8("lcdAddr", lcdAddr, 8, 119),
  FD_U8("lcdCols", lcdCols, 16, 20),
  FD_U8("lcdRows", lcdRows, 2, 4),
  FD_U8("lcdBacklight", lcdBacklight, 0, 1),
  FD_U8("lcdSda", lcdSda, 0, 39),
  FD_U8("lcdScl", lcdScl, 0, 39),
  FD_U16("lcdRefreshMs", lcdRefreshMs, 50, 2000),
  FD_U16("lcdBlinkMs", lcdBlinkMs, 120, 2000),
  FD_U16("statsSaveIntervalSec", statsSaveIntervalSec, 30, 3600),
  FD_U32("maxRunSeconds", maxRunSeconds, 60, 86400),
  FD_U32("maxCreditSeconds", maxCreditSeconds, 60, 86400),
  FD_U8("lowHeapRestart", lowHeapRestart, 0, 1),
  FD_U32("lowHeapBytes", lowHeapBytes, 4096, 131072)
};

static const uint16_t FIELD_COUNT = (uint16_t)(sizeof(g_fields) / sizeof(FieldDef));

static const FieldDef* fieldFind(const char* key) {
  for (uint16_t i = 0; i < FIELD_COUNT; ++i) {
    if (strcmp(g_fields[i].key, key) == 0) {
      return &g_fields[i];
    }
  }
  return nullptr;
}

static void fieldToJson(const Config& cfg, const FieldDef& field) {
  const uint8_t* base = ((const uint8_t*)&cfg) + field.off;
  if (field.type == FT_STR) {
    jsonStr(field.key, (const char*)base);
    return;
  }
  if (field.type == FT_U8) {
    uint8_t value = 0;
    memcpy(&value, base, sizeof(value));
    jsonU32(field.key, (uint32_t)value);
    return;
  }
  if (field.type == FT_U16) {
    uint16_t value = 0;
    memcpy(&value, base, sizeof(value));
    jsonU32(field.key, (uint32_t)value);
    return;
  }
  uint32_t value = 0;
  memcpy(&value, base, sizeof(value));
  jsonU32(field.key, value);
}

static void configToJson(const Config& cfg) {
  jsonObjBegin();
  for (uint16_t i = 0; i < FIELD_COUNT; ++i) {
    fieldToJson(cfg, g_fields[i]);
  }
  jsonU32("cfgRevision", g_pubLocal.state.cfgRevision);
  jsonObjEnd();
}

// ======================================================
// FORM BODY PARSER
// ======================================================

static const char* formBodyPtr() {
  return g_formBuf;
}

static bool formBodyCopy() {
  if (g_server.clientContentLength() > (size_t)FORM_BODY_LIMIT) {
    return false;
  }
  String body = g_server.arg("plain");
  size_t len = body.length();
  if (len >= (size_t)FORM_BODY_LIMIT) {
    return false;
  }
  if (len > 0) {
    memcpy(g_formBuf, body.c_str(), len);
  }
  g_formBuf[len] = '\0';
  return true;
}

static bool formGet(const char* body, const char* key, char* out, size_t outLen) {
  if (body == nullptr || key == nullptr || out == nullptr || outLen == 0) {
    return false;
  }
  size_t keyLen = strlen(key);
  const char* cursor = body;
  while (*cursor != '\0') {
    const char* lineEnd = strchr(cursor, '\n');
    size_t lineLen = (lineEnd != nullptr) ? (size_t)(lineEnd - cursor) : strlen(cursor);
    const char* eq = (const char*)memchr(cursor, '=', lineLen);
    if (eq != nullptr) {
      size_t foundKeyLen = (size_t)(eq - cursor);
      if (foundKeyLen == keyLen && strncmp(cursor, key, keyLen) == 0) {
        size_t valueLen = lineLen - foundKeyLen - 1;
        if (valueLen >= outLen) {
          out[0] = '\0';
          return false;
        }
        memcpy(out, eq + 1, valueLen);
        out[valueLen] = '\0';
        while (valueLen > 0 && (out[valueLen - 1] == '\r' || out[valueLen - 1] == ' ')) {
          valueLen--;
          out[valueLen] = '\0';
        }
        return true;
      }
    }
    if (lineEnd == nullptr) {
      break;
    }
    cursor = lineEnd + 1;
  }
  return false;
}

static bool parseU32Strict(const char* text, uint32_t& value) {
  if (text == nullptr || *text == '\0') return false;
  uint32_t n = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    uint32_t digit = (uint32_t)(*p - '0');
    if (n > (UINT32_MAX - digit) / 10U) return false;
    n = n * 10U + digit;
  }
  value = n;
  return true;
}

static bool formGetU32(const char* body, const char* key, uint32_t& value) {
  char buf[24];
  return formGet(body, key, buf, sizeof(buf)) && parseU32Strict(buf, value);
}

static bool configApplyForm(const char* body, Config& cfg, char* err, size_t errLen) {
  const char* cursor = body;
  while (*cursor != '\0') {
    const char* lineEnd = strchr(cursor, '\n');
    size_t lineLen = (lineEnd != nullptr) ? (size_t)(lineEnd - cursor) : strlen(cursor);
    const char* eq = (const char*)memchr(cursor, '=', lineLen);
    if (eq != nullptr && (size_t)(eq - cursor) > 0) {
      char key[32];
      size_t keyLen = (size_t)(eq - cursor);
      if (keyLen >= sizeof(key)) {
        safeCopyString(err, errLen, "key");
        return false;
      }
      memcpy(key, cursor, keyLen);
      key[keyLen] = '\0';
      size_t valueLen = lineLen - keyLen - 1;
      char value[80];
      if (valueLen >= sizeof(value)) {
        safeCopyString(err, errLen, key);
        return false;
      }
      memcpy(value, eq + 1, valueLen);
      value[valueLen] = '\0';
      while (valueLen > 0 && (value[valueLen - 1] == '\r')) {
        valueLen--;
        value[valueLen] = '\0';
      }
      const FieldDef* field = fieldFind(key);
      if (field == nullptr) {
        if (lineEnd == nullptr) {
          break;
        }
        cursor = lineEnd + 1;
        continue;
      }
      uint8_t* base = ((uint8_t*)&cfg) + field->off;
      if (field->type == FT_STR) {
        if (strlen(value) >= (size_t)field->len) {
          safeCopyString(err, errLen, key);
          return false;
        }
        safeCopyString((char*)base, (size_t)field->len, value);
      } else {
        uint32_t parsed = 0;
        if (!parseU32Strict(value, parsed)) {
          safeCopyString(err, errLen, key);
          return false;
        }
        if (parsed < field->minv || parsed > field->maxv) {
          safeCopyString(err, errLen, key);
          return false;
        }
        if (field->type == FT_U8) {
          uint8_t v = (uint8_t)parsed;
          memcpy(base, &v, sizeof(v));
        } else if (field->type == FT_U16) {
          uint16_t v = (uint16_t)parsed;
          memcpy(base, &v, sizeof(v));
        } else {
          uint32_t v = (uint32_t)parsed;
          memcpy(base, &v, sizeof(v));
        }
      }
    }
    if (lineEnd == nullptr) {
      break;
    }
    cursor = lineEnd + 1;
  }
  cfg.magic = CONFIG_MAGIC;
  cfg.version = CONFIG_VERSION;
  return true;
}

// ======================================================
// REQUEST GUARDS
// ======================================================

static void refreshLocalSnapshot() {
  readPublished(g_pubLocal);
}

static bool guardPin() {
  if (strlen(g_pubLocal.cfg.adminPin) == 0) {
    return true;
  }
  String provided = g_server.header("X-Pin");
  if (provided.length() == 0) {
    provided = g_server.arg("pin");
  }
  if (provided.length() == 0) {
    jsonSendError(401, "pin");
    return false;
  }
  if (provided != String(g_pubLocal.cfg.adminPin)) {
    jsonSendError(401, "pin");
    return false;
  }
  return true;
}

static bool guardConfirm(const char* expected) {
  char buf[24];
  if (!formGet(formBodyPtr(), "confirm", buf, sizeof(buf))) {
    jsonSendError(400, "confirm");
    return false;
  }
  if (strcmp(buf, expected) != 0) {
    jsonSendError(400, "confirm");
    return false;
  }
  return true;
}

// ======================================================
// STATE AND TELEMETRY ENDPOINT
// ======================================================

static void handleApiState() {
  refreshLocalSnapshot();
  const SharedState& s = g_pubLocal.state;
  const Stats& st = g_pubLocal.stats;
  const Config& cfg = g_pubLocal.cfg;

  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonStr("fw", FW_VERSION);
  jsonU64("uptimeTotal", st.uptimeSeconds);
  jsonU64("bootUs", s.bootUs);
  jsonU32("freeHeap", ESP.getFreeHeap());
  jsonU32("largestBlock", (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  jsonU64("poolMs", s.poolMs);
  jsonU64("creditAdded", s.creditAddedSeconds);
  jsonU32("timeModel", (uint32_t)cfg.timeModel);
  jsonU32("active", (uint32_t)s.activeChannels);
  jsonU32("lastButton", (uint32_t)s.lastButtonChannel);
  jsonU32("cfgRevision", s.cfgRevision);
  jsonU32("coinPulses", s.coinPulsesTotal);
  uint64_t lastCoinAgo = 0;
  if (s.lastCoinUs > 0) {
    uint64_t nowUs = timeNowUs();
    if (nowUs > s.lastCoinUs) {
      lastCoinAgo = (nowUs - s.lastCoinUs) / 1000000ULL;
    }
  }
  jsonU32("lastCoinAgo", (s.lastCoinUs > 0) ? (uint32_t)((lastCoinAgo > 0xFFFFFFFFULL) ? 0xFFFFFFFFULL : lastCoinAgo) : 0xFFFFFFFFUL);
  jsonU64("coinGroups", st.coins);
  jsonU64("revenueCentavos", st.revenueCentavos);
  jsonU32("bonusUsed", (uint32_t)s.bonusUsedSeconds);
  jsonU32("factoryHold", (uint32_t)s.factoryHoldActive);
  jsonU32("lcdPresent", (uint32_t)s.lcdPresent);
  jsonU32("apRunning", (uint32_t)s.apRunning);
  jsonU32("restartRequired", (uint32_t)s.restartRequired);
  jsonStr("apSsid", cfg.apSsid);
  jsonU32("apStations", (uint32_t)g_apStationCount);
  jsonU32("apRestarts", g_apRestartCount);
  jsonU32("wifiFails", g_wifiFailCount);
  jsonU32("nvsWrites", g_meta.nvsWrites);
  jsonU32("maxBonusSeconds", cfg.maxBonusSeconds);
  jsonU32("longPressAddSeconds", (uint32_t)cfg.longPressAddSeconds);

  jsonKey("ch");
  jsonArrBegin();
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    jsonObjBegin();
    jsonStr("n", CHANNEL_NAME[i]);
    jsonU32("state", (uint32_t)s.channelState[i]);
    jsonU64("remMs", s.channelRemainMs[i]);
    jsonU64("usedMs", s.channelUsedMs[i]);
    jsonU32("relay", (uint32_t)cfg.relayPin[i]);
    jsonU32("btn", (uint32_t)cfg.btnPin[i]);
    jsonU32("relayOn", (uint32_t)s.channelRelay[i]);
    jsonU32("blink", (uint32_t)s.blinkPhase[i]);
    jsonObjEnd();
  }
  jsonArrEnd();

  jsonKey("lcd");
  jsonArrBegin();
  for (uint8_t i = 0; i < 4; ++i) {
    jsonStrVal(s.lcdLine[i]);
  }
  jsonArrEnd();

  jsonKey("cfg");
  jsonObjBegin();
  jsonU32("lcdAddr", (uint32_t)cfg.lcdAddr);
  jsonU32("lcdCols", (uint32_t)cfg.lcdCols);
  jsonU32("lcdRows", (uint32_t)cfg.lcdRows);
  jsonU32("lcdBacklight", (uint32_t)cfg.lcdBacklight);
  jsonU32("buzzerEnabled", (uint32_t)cfg.buzzerEnabled);
  jsonU32("relayActiveLow", (uint32_t)cfg.relayActiveLow);
  jsonU32("longPressMs", (uint32_t)cfg.longPressMs);
  jsonU32("doubleClickMs", (uint32_t)cfg.doubleClickMs);
  jsonU32("bonusEnabled", (uint32_t)cfg.bonusEnabled);
  jsonObjEnd();

  jsonObjEnd();
  jsonSend();
}

static void handleApiStats() {
  refreshLocalSnapshot();
  const Stats& st = g_pubLocal.stats;

  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU64("revenueCentavos", st.revenueCentavos);
  jsonU64("coins", st.coins);
  jsonU64("pulses", st.pulses);
  jsonU64("bonusSeconds", st.bonusSeconds);
  jsonU64("uptimeSeconds", st.uptimeSeconds);
  jsonU32("boots", st.boots);
  jsonU32("dayIndex", st.dayIndex);
  jsonU32("sessionCount", (uint32_t)st.sessionCount);

  jsonKey("channelSeconds");
  jsonArrBegin();
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    jsonU64Val(st.channelSeconds[i]);
  }
  jsonArrEnd();

  jsonKey("channelSessions");
  jsonArrBegin();
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    jsonU64Val(st.channelSessions[i]);
  }
  jsonArrEnd();

  jsonKey("days");
  jsonArrBegin();
  uint32_t available = st.dayIndex + 1;
  if (available > DAY_RING_LEN) {
    available = DAY_RING_LEN;
  }
  for (uint32_t i = 0; i < available; ++i) {
    uint32_t dayNumber = st.dayIndex - (available - 1) + i;
    uint32_t slot = dayNumber % DAY_RING_LEN;
    jsonObjBegin();
    jsonU32("d", dayNumber);
    jsonU64("rev", st.days[slot].revenueCentavos);
    jsonU64("sec", st.days[slot].seconds);
    jsonU32("coins", st.days[slot].coins);
    jsonObjEnd();
  }
  jsonArrEnd();

  jsonKey("sessions");
  jsonArrBegin();
  for (uint8_t i = 0; i < st.sessionCount && i < SESSION_RING_LEN; ++i) {
    uint8_t idx = (uint8_t)((st.sessionHead + SESSION_RING_LEN - 1 - i) % SESSION_RING_LEN);
    const SessionRecord& rec = st.sessions[idx];
    jsonObjBegin();
    jsonU32("ch", (uint32_t)rec.channel);
    jsonU32("start", rec.startSec);
    jsonU32("dur", rec.durationSec);
    jsonU32("reason", (uint32_t)rec.reason);
    jsonObjEnd();
  }
  jsonArrEnd();

  jsonObjEnd();
  jsonSend();
}


// ======================================================
// CONFIGURATION ENDPOINTS
// ======================================================

static void handleApiConfigGet() {
  refreshLocalSnapshot();
  if (!guardPin()) return;
  jsonReset();
  configToJson(g_pubLocal.cfg);
  jsonSend();
}

static void handleApiConfigPost() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!formBodyCopy()) {
    jsonSendError(413, "body");
    return;
  }
  Config next = g_pubLocal.cfg;
  char err[32];
  if (!configApplyForm(g_formBuf, next, err, sizeof(err))) {
    jsonSendError(400, err);
    return;
  }
  if (!validateConfig(next, err, sizeof(err))) {
    jsonSendError(400, err);
    return;
  }
  if (!configPushIfCurrent(next, g_pubLocal.state.cfgRevision)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonU32("cfgRevision", g_pubLocal.state.cfgRevision);
  jsonObjEnd();
  jsonSend();
}

static void handleApiConfigDefaults() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  Config defaults;
  configSetDefaults(defaults);
  if (!configPushIfCurrent(defaults, g_pubLocal.state.cfgRevision)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonObjEnd();
  jsonSend();
}

static void handleApiBackup() {
  refreshLocalSnapshot();
  if (!guardPin()) return;
  jsonReset();
  jsonObjBegin();
  jsonStr("firmware", FW_NAME);
  jsonStr("version", FW_VERSION);
  jsonStr("build", FW_BUILD_TAG);
  jsonKey("config");
  configToJson(g_pubLocal.cfg);
  jsonObjEnd();
  g_server.sendHeader("Content-Disposition", "attachment; filename=carwash-backup.json");
  jsonSend();
}

// ======================================================
// CONTROL ACTION ENDPOINTS
// ======================================================

static void handleApiChannel() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!formBodyCopy()) {
    jsonSendError(413, "body");
    return;
  }
  uint32_t ch = 0;
  if (!formGetU32(formBodyPtr(), "ch", ch) || ch >= CHANNEL_COUNT) {
    jsonSendError(400, "ch");
    return;
  }
  char action[12];
  if (!formGet(formBodyPtr(), "action", action, sizeof(action))) {
    jsonSendError(400, "action");
    return;
  }
  uint8_t type = CMD_NONE;
  if (strcmp(action, "start") == 0) {
    type = CMD_CH_START;
  } else if (strcmp(action, "pause") == 0) {
    type = CMD_CH_PAUSE;
  } else if (strcmp(action, "resume") == 0) {
    type = CMD_CH_RESUME;
  } else if (strcmp(action, "stop") == 0) {
    type = CMD_CH_STOP;
  } else if (strcmp(action, "toggle") == 0) {
    type = CMD_CH_TOGGLE;
  } else if (strcmp(action, "add") == 0) {
    type = CMD_CH_ADD;
  } else {
    jsonSendError(400, "action");
    return;
  }
  uint32_t seconds = 0;
  formGetU32(formBodyPtr(), "seconds", seconds);
  if (seconds > 3600UL) {
    seconds = 3600UL;
  }
  if (type == CMD_CH_ADD && seconds == 0) {
    seconds = g_pubLocal.cfg.longPressAddSeconds;
  }
  if (!cmdPush(type, (uint8_t)ch, (uint16_t)seconds, 0, 0)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonObjEnd();
  jsonSend();
}

static void handleApiCredit() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!formBodyCopy()) {
    jsonSendError(413, "body");
    return;
  }
  uint32_t seconds = 0;
  if (!formGetU32(formBodyPtr(), "seconds", seconds) || seconds == 0 || seconds > 86400UL) {
    jsonSendError(400, "seconds");
    return;
  }
  if (!cmdPush(CMD_ADD_CREDIT, 0, 0, seconds, 0)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonObjEnd();
  jsonSend();
}

static void handleApiCoinSimulate() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!formBodyCopy()) {
    jsonSendError(413, "body");
    return;
  }
  uint32_t pulses = 0;
  if (!formGetU32(formBodyPtr(), "pulses", pulses) || pulses == 0 || pulses > 200UL) {
    jsonSendError(400, "pulses");
    return;
  }
  if (!cmdPush(CMD_SIM_COIN, 0, 0, pulses, 0)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonObjEnd();
  jsonSend();
}

static void handleApiBuzzer() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!formBodyCopy()) {
    jsonSendError(413, "body");
    return;
  }
  uint32_t pattern = 0;
  if (!formGetU32(formBodyPtr(), "pattern", pattern) || pattern == 0 || pattern > 10UL) {
    jsonSendError(400, "pattern");
    return;
  }
  if (!cmdPush(CMD_BUZZER_TEST, 0, 0, pattern, 0)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonObjEnd();
  jsonSend();
}

// ======================================================
// MAINTENANCE ENDPOINTS
// ======================================================

static void handleApiResetStats() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!formBodyCopy()) {
    jsonSendError(413, "body");
    return;
  }
  if (!guardConfirm("RESET")) {
    return;
  }
  if (!cmdPush(CMD_RESET_STATS, 0, 0, 0, 120)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonObjEnd();
  jsonSend();
}

static void handleApiClearSessions() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!formBodyCopy()) {
    jsonSendError(413, "body");
    return;
  }
  if (!guardConfirm("CLEAR")) {
    return;
  }
  if (!cmdPush(CMD_CLEAR_SESSIONS, 0, 0, 0, 120)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonObjEnd();
  jsonSend();
}

static void handleApiFactoryReset() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!formBodyCopy()) {
    jsonSendError(413, "body");
    return;
  }
  if (!guardConfirm("FACTORY")) {
    return;
  }
  if (!cmdPush(CMD_FACTORY_RESET, 0, 0, 0, 700)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonObjEnd();
  jsonSend();
}

static void handleApiReboot() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!formBodyCopy()) {
    jsonSendError(413, "body");
    return;
  }
  if (!guardConfirm("REBOOT")) {
    return;
  }
  if (!cmdPush(CMD_REBOOT, 0, 0, 0, 700)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU32("queued", 1u);
  jsonObjEnd();
  jsonSend();
}

static void handleApiStatsSave() {
  refreshLocalSnapshot();
  if (!guardPin()) {
    return;
  }
  if (!cmdPush(CMD_SAVE_STATS, 0, 0, 0, 0)) {
    jsonSendError(503, "busy");
    return;
  }
  jsonSendOk();
}

// ======================================================
// DIAGNOSTICS ENDPOINT
// ======================================================

static const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power_on";
    case ESP_RST_EXT:      return "external";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_DEEPSLEEP:return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
  }
}

static void handleApiDiag() {
  refreshLocalSnapshot();
  const Config& cfg = g_pubLocal.cfg;
  const Stats& st = g_pubLocal.stats;

  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonStr("firmware", FW_NAME);
  jsonStr("version", FW_VERSION);
  jsonStr("build", FW_BUILD_TAG);
  jsonStr("resetReason", resetReasonText());
  jsonU64("uptimeSeconds", st.uptimeSeconds);
  jsonU32("boots", st.boots);
  jsonU32("freeHeap", ESP.getFreeHeap());
  jsonU32("minFreeHeap", ESP.getMinFreeHeap());
  jsonU32("largestBlock", (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  jsonU32("psramSize", (uint32_t)ESP.getPsramSize());
  jsonU32("flashSize", (uint32_t)ESP.getFlashChipSize());
  jsonU32("cpuFreq", (uint32_t)ESP.getCpuFreqMHz());
  jsonU32("nvsWrites", g_meta.nvsWrites);
  jsonU32("configSeq", g_meta.configSeq);
  jsonU32("activeSlot", (uint32_t)g_meta.activeSlot);
  jsonU32("lcdPresent", (uint32_t)g_pubLocal.state.lcdPresent);
  jsonU32("i2cAddress", (uint32_t)cfg.lcdAddr);
  jsonU32("apRestarts", g_apRestartCount);
  jsonU32("wifiFails", g_wifiFailCount);
  jsonU32("coinDropped", g_coinRingDropped);
  jsonU32("cmdQueueDepth", (uint32_t)g_cmdCount);
  jsonU32("statsDirty", (uint32_t)g_statsDirty);
  jsonU32("lowHeapStrikes", (uint32_t)g_lowHeapStrikes);
  jsonKey("pins");
  jsonObjBegin();
  jsonU32("waterRelay", (uint32_t)cfg.relayPin[CH_WATER]);
  jsonU32("blowerRelay", (uint32_t)cfg.relayPin[CH_BLOWER]);
  jsonU32("soapRelay", (uint32_t)cfg.relayPin[CH_SOAP]);
  jsonU32("foamRelay", (uint32_t)cfg.relayPin[CH_FOAM]);
  jsonU32("waterButton", (uint32_t)cfg.btnPin[CH_WATER]);
  jsonU32("blowerButton", (uint32_t)cfg.btnPin[CH_BLOWER]);
  jsonU32("soapButton", (uint32_t)cfg.btnPin[CH_SOAP]);
  jsonU32("foamButton", (uint32_t)cfg.btnPin[CH_FOAM]);
  jsonU32("coin", (uint32_t)cfg.coinPin);
  jsonU32("pulseOut", (uint32_t)cfg.pulseOutPin);
  jsonU32("buzzer", (uint32_t)cfg.buzzerPin);
  jsonU32("lcdSda", (uint32_t)cfg.lcdSda);
  jsonU32("lcdScl", (uint32_t)cfg.lcdScl);
  jsonU32("boot", (uint32_t)cfg.bootPin);
  jsonObjEnd();
  jsonObjEnd();
  jsonSend();
}


// ======================================================
// EMBEDDED WEB APPLICATION
// ======================================================

static const char APP_HTML[] PROGMEM = R"RAWHTML(<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark light">
<meta name="theme-color" content="#0b0f14">
<meta name="description" content="Carwash 4-in-1 coin operated controller panel">
<title>Carwash 4-in-1 Controller</title>
<style>
/* ======================================================
   DESIGN TOKENS
   ====================================================== */
:root{
  --font-sans:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,"Noto Sans",sans-serif;
  --font-mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,"Liberation Mono",monospace;
  --r-xs:6px; --r-sm:9px; --r-md:13px; --r-lg:18px; --r-xl:26px;
  --sp-1:4px; --sp-2:8px; --sp-3:12px; --sp-4:16px; --sp-5:22px; --sp-6:30px;
  --sidebar-w:266px;
  --shadow-1:0 1px 2px rgba(0,0,0,.18);
  --shadow-2:0 10px 30px rgba(0,0,0,.28);
  --shadow-3:0 22px 60px rgba(0,0,0,.42);
  --ease:cubic-bezier(.4,.0,.2,1);
  --water:#38bdf8; --blower:#a78bfa; --soap:#34d399; --foam:#fbbf24;
  --accent:#22d3ee;
}
html[data-theme="dark"]{
  --bg:#080c11; --bg-2:#0b1118;
  --surface:#111823; --surface-2:#16202d; --surface-3:#1c2837;
  --border:#22303f; --border-strong:#2d3f52;
  --text:#e8eff8; --text-2:#a9bccf; --text-3:#6f869d;
  --accent:#22d3ee; --accent-soft:rgba(34,211,238,.14);
  --ok:#34d399; --ok-soft:rgba(52,211,153,.14);
  --warn:#fbbf24; --warn-soft:rgba(251,191,36,.14);
  --danger:#f87171; --danger-soft:rgba(248,113,113,.14);
  --grid:rgba(255,255,255,.055);
  --header-bg:rgba(11,17,24,.86);
  --lcd-bg:#0f2a1c; --lcd-on:#5ef08a; --lcd-off:rgba(94,240,138,.09);
}
html[data-theme="light"]{
  --bg:#eef3f9; --bg-2:#f6f9fc;
  --surface:#ffffff; --surface-2:#f5f8fc; --surface-3:#e9f0f8;
  --border:#d9e3ee; --border-strong:#c2d1e1;
  --text:#0d1a29; --text-2:#496178; --text-3:#7d92a8;
  --accent:#0891b2; --accent-soft:rgba(8,145,178,.12);
  --ok:#059669; --ok-soft:rgba(5,150,105,.12);
  --warn:#b45309; --warn-soft:rgba(180,83,9,.12);
  --danger:#dc2626; --danger-soft:rgba(220,38,38,.12);
  --grid:rgba(13,26,41,.07);
  --header-bg:rgba(255,255,255,.88);
  --lcd-bg:#0d2b1d; --lcd-on:#6cf79b; --lcd-off:rgba(108,247,155,.1);
}
*,*::before,*::after{box-sizing:border-box}
html,body{height:100%}
body{
  margin:0; background:var(--bg); color:var(--text);
  font-family:var(--font-sans); font-size:14.5px; line-height:1.5;
  -webkit-font-smoothing:antialiased; text-rendering:optimizeLegibility;
  overscroll-behavior-y:none;
}
h1,h2,h3,h4,p,figure{margin:0}
button,input,select,textarea{font:inherit;color:inherit}
a{color:inherit;text-decoration:none}
::selection{background:var(--accent-soft)}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:var(--r-xs)}
::-webkit-scrollbar{width:10px;height:10px}
::-webkit-scrollbar-thumb{background:var(--border-strong);border-radius:99px;border:2px solid var(--bg)}
::-webkit-scrollbar-track{background:transparent}

/* ======================================================
   APPLICATION SHELL
   ====================================================== */
.app-shell{display:grid;grid-template-columns:var(--sidebar-w) minmax(0,1fr);min-height:100vh;min-height:100dvh}

/* ======================================================
   SIDEBAR NAVIGATION
   ====================================================== */
.sidebar{
  position:sticky;top:0;height:100vh;height:100dvh;display:flex;flex-direction:column;
  background:linear-gradient(180deg,var(--surface) 0%,var(--bg-2) 100%);
  border-right:1px solid var(--border);z-index:40;
}
.brand{display:flex;align-items:center;gap:12px;padding:18px 18px 16px;border-bottom:1px solid var(--border)}
.brand-mark{
  width:40px;height:40px;border-radius:12px;flex:0 0 auto;display:grid;place-items:center;
  background:linear-gradient(150deg,rgba(34,211,238,.22),rgba(56,189,248,.06));
  border:1px solid rgba(34,211,238,.32);color:var(--accent);
}
.brand-text{min-width:0}
.brand-text strong{display:block;font-size:14px;letter-spacing:.2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.brand-text span{display:block;font-size:11px;color:var(--text-3);font-family:var(--font-mono);letter-spacing:.4px}
.nav{flex:1 1 auto;overflow-y:auto;padding:14px 10px 18px}
.nav-group{margin-bottom:16px}
.nav-label{
  display:block;padding:0 10px 7px;font-size:10.5px;font-weight:700;letter-spacing:.14em;
  text-transform:uppercase;color:var(--text-3);
}
.nav-link{
  display:flex;align-items:center;gap:11px;padding:9.5px 11px;border-radius:10px;
  color:var(--text-2);font-size:13.6px;font-weight:500;cursor:pointer;
  transition:background .12s var(--ease),color .12s var(--ease);
  position:relative;
}
.nav-link:hover{background:var(--surface-2);color:var(--text)}
.nav-link.is-active{background:var(--accent-soft);color:var(--accent);font-weight:600}
.nav-link.is-active::before{
  content:"";position:absolute;left:-10px;top:50%;transform:translateY(-50%);
  width:3px;height:20px;border-radius:0 3px 3px 0;background:var(--accent);
}
.nav-link svg{flex:0 0 auto;opacity:.9}
.nav-badge{
  margin-left:auto;font-family:var(--font-mono);font-size:10.5px;padding:1px 6px;border-radius:99px;
  background:var(--surface-3);color:var(--text-2);
}
.sidebar-foot{border-top:1px solid var(--border);padding:12px 16px;font-size:11px;color:var(--text-3);font-family:var(--font-mono)}
.sidebar-foot .row{display:flex;justify-content:space-between;gap:8px}
.sidebar-foot .row span:last-child{color:var(--text-2)}

/* ======================================================
   MAIN COLUMN AND TOP BAR
   ====================================================== */
.main{min-width:0;display:flex;flex-direction:column}
.topbar{
  position:sticky;top:0;z-index:30;display:flex;align-items:center;gap:14px;
  padding:12px 22px;background:var(--header-bg);backdrop-filter:blur(14px);
  border-bottom:1px solid var(--border);
}
.topbar-title{min-width:0;flex:1 1 auto}
.topbar-title h1{font-size:16.5px;font-weight:650;letter-spacing:-.01em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.topbar-title p{font-size:11.8px;color:var(--text-3);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.topbar-actions{display:flex;align-items:center;gap:9px}
.icon-btn{
  display:grid;place-items:center;width:36px;height:36px;border-radius:10px;
  background:var(--surface-2);border:1px solid var(--border);color:var(--text-2);cursor:pointer;
  transition:background .12s var(--ease),color .12s var(--ease),border-color .12s var(--ease);
}
.icon-btn:hover{background:var(--surface-3);color:var(--text);border-color:var(--border-strong)}
#menuBtn{display:none}
.content{padding:22px;display:block;max-width:1560px;width:100%;margin:0 auto}

/* ======================================================
   CONNECTION PILL
   ====================================================== */
.pill{
  display:inline-flex;align-items:center;gap:7px;height:30px;padding:0 11px;border-radius:99px;
  background:var(--surface-2);border:1px solid var(--border);font-size:11.6px;font-weight:600;
  color:var(--text-2);font-family:var(--font-mono);letter-spacing:.02em;white-space:nowrap;
}
.dot{width:7px;height:7px;border-radius:99px;background:var(--text-3);flex:0 0 auto}
.pill.is-live{color:var(--ok);border-color:rgba(52,211,153,.35);background:var(--ok-soft)}
.pill.is-live .dot{background:var(--ok);box-shadow:0 0 0 3px rgba(52,211,153,.18);animation:pulse 2.4s infinite}
.pill.is-off{color:var(--danger);border-color:rgba(248,113,113,.35);background:var(--danger-soft)}
.pill.is-off .dot{background:var(--danger)}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}

/* ======================================================
   VIEWS AND PAGE HEADERS
   ====================================================== */
.view{display:none;animation:fade .18s var(--ease)}
.view.is-active{display:block}
@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.section-head{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;margin:0 0 14px}
.section-head h2{font-size:14.5px;font-weight:650;letter-spacing:-.005em}
.section-head p{font-size:12.2px;color:var(--text-3);margin-top:2px}
.stack{display:grid;gap:18px}

/* ======================================================
   CARDS
   ====================================================== */
.card{
  background:var(--surface);border:1px solid var(--border);border-radius:var(--r-lg);
  box-shadow:var(--shadow-1);overflow:hidden;
}
.card-head{
  display:flex;align-items:center;justify-content:space-between;gap:12px;
  padding:15px 18px;border-bottom:1px solid var(--border);background:var(--surface-2);
}
.card-head h3{font-size:13.4px;font-weight:650}
.card-head span{font-size:11.6px;color:var(--text-3)}
.card-body{padding:18px}
.card-body.tight{padding:0}

/* ======================================================
   KPI GRID
   ====================================================== */
.kpi-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(196px,1fr));gap:14px}
.kpi{
  position:relative;background:var(--surface);border:1px solid var(--border);border-radius:var(--r-md);
  padding:15px 16px;overflow:hidden;box-shadow:var(--shadow-1);
}
.kpi::after{
  content:"";position:absolute;inset:0 auto 0 0;width:3px;
  background:var(--accent);opacity:.85;
}
.kpi.k-water::after{background:var(--water)} .kpi.k-blower::after{background:var(--blower)}
.kpi.k-soap::after{background:var(--soap)}   .kpi.k-foam::after{background:var(--foam)}
.kpi.k-ok::after{background:var(--ok)}       .kpi.k-warn::after{background:var(--warn)}
.kpi-label{display:flex;align-items:center;gap:7px;font-size:11.2px;font-weight:650;letter-spacing:.06em;text-transform:uppercase;color:var(--text-3)}
.kpi-value{font-family:var(--font-mono);font-size:24px;font-weight:600;letter-spacing:-.02em;margin-top:7px;line-height:1.15;font-variant-numeric:tabular-nums}
.kpi-value small{font-size:12px;color:var(--text-3);font-weight:500;margin-left:4px}
.kpi-sub{font-size:11.6px;color:var(--text-3);margin-top:4px;display:flex;align-items:center;gap:6px}

/* ======================================================
   CHANNEL CONTROL CARDS
   ====================================================== */
.channel-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:14px}
.channel{
  background:var(--surface);border:1px solid var(--border);border-radius:var(--r-md);
  padding:16px;display:flex;flex-direction:column;gap:13px;position:relative;overflow:hidden;
  transition:border-color .18s var(--ease),box-shadow .18s var(--ease);
}
.channel[data-state="1"]{border-color:color-mix(in srgb,var(--ch-color) 55%,var(--border));box-shadow:0 0 0 1px color-mix(in srgb,var(--ch-color) 22%,transparent),0 8px 26px rgba(0,0,0,.16)}
.channel[data-state="2"]{border-color:color-mix(in srgb,var(--warn) 45%,var(--border))}
.channel-top{display:flex;align-items:center;gap:10px}
.channel-icon{
  width:34px;height:34px;border-radius:10px;display:grid;place-items:center;flex:0 0 auto;
  background:color-mix(in srgb,var(--ch-color) 16%,transparent);
  border:1px solid color-mix(in srgb,var(--ch-color) 34%,transparent);color:var(--ch-color);
}
.channel-name{font-size:13px;font-weight:700;letter-spacing:.05em}
.channel-pin{font-family:var(--font-mono);font-size:10.8px;color:var(--text-3)}
.status-chip{
  margin-left:auto;font-size:10.4px;font-weight:700;letter-spacing:.08em;padding:3px 8px;border-radius:99px;
  background:var(--surface-3);color:var(--text-3);font-family:var(--font-mono);
}
.channel[data-state="1"] .status-chip{background:var(--ok-soft);color:var(--ok)}
.channel[data-state="2"] .status-chip{background:var(--warn-soft);color:var(--warn)}
.channel-time{
  font-family:var(--font-mono);font-size:33px;font-weight:600;letter-spacing:-.03em;line-height:1;
  font-variant-numeric:tabular-nums;color:var(--text);
}
.channel-time.is-blinking{opacity:.25}
.channel-time small{font-size:11px;color:var(--text-3);font-family:var(--font-sans);font-weight:600;letter-spacing:.06em;margin-left:6px;text-transform:uppercase}
.channel-meta{display:flex;justify-content:space-between;font-size:11.4px;color:var(--text-3);font-family:var(--font-mono)}
.bar{height:6px;border-radius:99px;background:var(--surface-3);overflow:hidden}
.bar>i{display:block;height:100%;border-radius:99px;background:var(--ch-color);transition:width .45s var(--ease)}
.channel-actions{display:flex;gap:7px;flex-wrap:wrap}

/* ======================================================
   BUTTONS
   ====================================================== */
.btn{
  display:inline-flex;align-items:center;justify-content:center;gap:7px;height:34px;padding:0 13px;
  border-radius:9px;border:1px solid var(--border);background:var(--surface-2);color:var(--text);
  font-size:12.6px;font-weight:600;cursor:pointer;white-space:nowrap;
  transition:background .12s var(--ease),border-color .12s var(--ease),transform .06s var(--ease),opacity .12s;
}
.btn:hover{background:var(--surface-3);border-color:var(--border-strong)}
.btn:active{transform:translateY(1px)}
.btn[disabled]{opacity:.45;cursor:not-allowed;transform:none}
.btn-primary{background:var(--accent);border-color:transparent;color:#04181d}
.btn-primary:hover{background:color-mix(in srgb,var(--accent) 86%,#fff)}
.btn-ok{background:var(--ok);border-color:transparent;color:#03251b}
.btn-ok:hover{background:color-mix(in srgb,var(--ok) 86%,#fff)}
.btn-warn{background:var(--warn);border-color:transparent;color:#2b1a00}
.btn-warn:hover{background:color-mix(in srgb,var(--warn) 86%,#fff)}
.btn-danger{background:var(--danger);border-color:transparent;color:#2c0505}
.btn-danger:hover{background:color-mix(in srgb,var(--danger) 86%,#fff)}
.btn-ghost{background:transparent}
.btn-sm{height:29px;padding:0 10px;font-size:11.8px;border-radius:8px}
.btn-block{width:100%}
.btn-group{display:flex;gap:9px;flex-wrap:wrap}

/* ======================================================
   FORMS
   ====================================================== */
.form-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px 20px}
.field{display:flex;flex-direction:column;gap:6px;min-width:0}
.field>label{font-size:12.2px;font-weight:600;color:var(--text-2);display:flex;align-items:center;gap:6px}
.field .hint{font-size:11.2px;color:var(--text-3);line-height:1.45}
.input,.select{
  height:36px;padding:0 11px;border-radius:9px;border:1px solid var(--border);
  background:var(--surface-2);color:var(--text);font-size:13.2px;width:100%;
  transition:border-color .12s var(--ease),background .12s var(--ease);
}
.input:focus,.select:focus{outline:none;border-color:var(--accent);background:var(--surface)}
.input[type="number"]{font-family:var(--font-mono)}
.input.is-invalid{border-color:var(--danger);background:var(--danger-soft)}
.select{appearance:none;background-image:linear-gradient(45deg,transparent 50%,var(--text-3) 50%),linear-gradient(135deg,var(--text-3) 50%,transparent 50%);background-position:calc(100% - 16px) 15px,calc(100% - 11px) 15px;background-size:5px 5px,5px 5px;background-repeat:no-repeat;padding-right:32px}
.switch{display:inline-flex;align-items:center;gap:10px;cursor:pointer;user-select:none}
.switch input{position:absolute;opacity:0;width:0;height:0}
.switch .track{
  width:42px;height:24px;border-radius:99px;background:var(--surface-3);border:1px solid var(--border);
  position:relative;transition:background .16s var(--ease),border-color .16s var(--ease);flex:0 0 auto;
}
.switch .track::after{
  content:"";position:absolute;top:2px;left:2px;width:18px;height:18px;border-radius:99px;
  background:var(--text-3);transition:transform .16s var(--ease),background .16s var(--ease);
}
.switch input:checked + .track{background:var(--accent-soft);border-color:var(--accent)}
.switch input:checked + .track::after{transform:translateX(18px);background:var(--accent)}
.switch input:focus-visible + .track{outline:2px solid var(--accent);outline-offset:2px}
.switch .label{font-size:12.8px;font-weight:600;color:var(--text-2)}
.form-actions{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:20px;padding-top:16px;border-top:1px solid var(--border)}
.form-actions .spacer{flex:1 1 auto}
.fieldset{border:1px solid var(--border);border-radius:var(--r-md);padding:16px;background:var(--surface-2)}
.fieldset legend{padding:0 8px;font-size:11.4px;font-weight:700;letter-spacing:.1em;text-transform:uppercase;color:var(--text-3)}

/* ======================================================
   DIAGNOSTIC GRID
   ====================================================== */
.diag-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:0 26px;padding:4px 2px}
.diag-row{display:flex;align-items:baseline;justify-content:space-between;gap:14px;padding:10px 16px;border-bottom:1px solid var(--border);font-size:12.7px;min-width:0}
.diag-row span{color:var(--text-3);flex:0 0 auto}
.diag-row b{font-family:var(--font-mono);font-weight:600;text-align:right;word-break:break-word;min-width:0}

/* ======================================================
   TABLES
   ====================================================== */
.table-wrap{overflow-x:auto}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{padding:11px 16px;text-align:left;border-bottom:1px solid var(--border);white-space:nowrap}
th{font-size:10.8px;font-weight:700;letter-spacing:.09em;text-transform:uppercase;color:var(--text-3);background:var(--surface-2);position:sticky;top:0}
tbody tr:last-child td{border-bottom:none}
tbody tr:hover{background:var(--surface-2)}
td.num{font-family:var(--font-mono);font-variant-numeric:tabular-nums}
.tag{display:inline-flex;align-items:center;gap:5px;font-size:11px;font-weight:650;padding:2px 8px;border-radius:99px;background:var(--surface-3);color:var(--text-2)}
.tag.ok{background:var(--ok-soft);color:var(--ok)}
.tag.warn{background:var(--warn-soft);color:var(--warn)}
.tag.danger{background:var(--danger-soft);color:var(--danger)}
.tag.info{background:var(--accent-soft);color:var(--accent)}
.empty{padding:34px 18px;text-align:center;color:var(--text-3);font-size:12.8px}

/* ======================================================
   CHARTS
   ====================================================== */
.chart{width:100%;display:block;overflow:visible}
.chart text{fill:var(--text-3);font-size:10px;font-family:var(--font-mono)}
.chart .grid{stroke:var(--grid);stroke-width:1}
.chart .chart-bar{fill:var(--accent);opacity:.85;transition:opacity .12s}
.chart .chart-bar:hover{opacity:1}
.chart .chart-bar-peak{fill:var(--soap)}
.legend{display:flex;gap:16px;flex-wrap:wrap;font-size:11.6px;color:var(--text-2)}
.legend i{display:inline-block;width:9px;height:9px;border-radius:3px;margin-right:6px}
.chart-tip{
  position:fixed;pointer-events:none;z-index:90;background:var(--surface-3);border:1px solid var(--border-strong);
  border-radius:9px;padding:7px 10px;font-size:11.8px;box-shadow:var(--shadow-2);opacity:0;transition:opacity .1s;
  font-family:var(--font-mono);white-space:nowrap;
}
.chart-tip.is-on{opacity:1}
.donut-wrap{display:flex;align-items:center;gap:22px;flex-wrap:wrap}
.donut{flex:0 0 auto}
.donut circle{transition:stroke-dasharray .5s var(--ease)}
.donut-legend{display:grid;gap:9px;flex:1 1 180px;min-width:0}
.donut-legend .row{display:flex;align-items:center;gap:9px;font-size:12.4px}
.donut-legend .row i{width:10px;height:10px;border-radius:3px;flex:0 0 auto}
.donut-legend .row span:last-child{margin-left:auto;font-family:var(--font-mono);color:var(--text-2)}

/* ======================================================
   LCD PREVIEW
   ====================================================== */
.lcd-frame{
  background:linear-gradient(180deg,#152b20,#0b1c14);
  border:1px solid #1f4433;border-radius:var(--r-md);padding:16px 18px;
  box-shadow:inset 0 2px 20px rgba(0,0,0,.55),0 10px 30px rgba(0,0,0,.3);
}
.lcd-screen{
  font-family:var(--font-mono);font-size:clamp(9px,2.6vw,15px);line-height:1.55;letter-spacing:1.5px;
  color:var(--lcd-on);text-shadow:0 0 8px rgba(94,240,138,.45);
  background:var(--lcd-bg);border-radius:6px;padding:12px 14px;white-space:pre;overflow-x:auto;
}
.lcd-screen .off{color:var(--lcd-off);text-shadow:none}
.lcd-meta{display:flex;justify-content:space-between;gap:10px;margin-top:10px;font-size:11px;color:#4f8f6c;font-family:var(--font-mono)}
.lcd-row{display:block}

/* ======================================================
   PIN MATRIX
   ====================================================== */
.pin-map{display:grid;grid-template-columns:repeat(auto-fill,minmax(132px,1fr));gap:10px}
.pin-item{
  border:1px solid var(--border);border-radius:10px;padding:10px 12px;background:var(--surface-2);
  display:flex;flex-direction:column;gap:3px;
}
.pin-item b{font-family:var(--font-mono);font-size:15px;font-weight:600;color:var(--accent)}
.pin-item span{font-size:11.2px;color:var(--text-3);letter-spacing:.04em;text-transform:uppercase;font-weight:600}

/* ======================================================
   TOASTS AND MODALS
   ====================================================== */
#toasts{position:fixed;right:18px;bottom:18px;z-index:120;display:flex;flex-direction:column;gap:9px;max-width:min(380px,calc(100vw - 36px))}
.toast{
  display:flex;align-items:flex-start;gap:10px;padding:12px 14px;border-radius:11px;
  background:var(--surface-3);border:1px solid var(--border-strong);box-shadow:var(--shadow-2);
  font-size:12.8px;animation:toastIn .22s var(--ease);
}
.toast.ok{border-color:rgba(52,211,153,.45)}
.toast.err{border-color:rgba(248,113,113,.45)}
.toast .ico{flex:0 0 auto;margin-top:1px}
.toast strong{display:block;font-size:12.6px;margin-bottom:1px}
.toast p{color:var(--text-2);font-size:12.2px}
@keyframes toastIn{from{opacity:0;transform:translateY(10px) scale(.98)}to{opacity:1;transform:none}}
.modal-scrim{
  position:fixed;inset:0;z-index:110;background:rgba(3,7,12,.66);backdrop-filter:blur(3px);
  display:grid;place-items:center;padding:20px;animation:fade .15s var(--ease);
}
.modal{
  width:min(460px,100%);background:var(--surface);border:1px solid var(--border-strong);
  border-radius:var(--r-lg);box-shadow:var(--shadow-3);overflow:hidden;animation:modalIn .2s var(--ease);
}
@keyframes modalIn{from{opacity:0;transform:translateY(12px) scale(.98)}to{opacity:1;transform:none}}
.modal-head{padding:18px 20px 0}
.modal-head h3{font-size:15.5px;font-weight:680}
.modal-body{padding:10px 20px 18px;color:var(--text-2);font-size:13px;line-height:1.6}
.modal-foot{display:flex;gap:10px;justify-content:flex-end;padding:14px 20px;background:var(--surface-2);border-top:1px solid var(--border)}

/* ======================================================
   MISC COMPONENTS
   ====================================================== */
.mono{font-family:var(--font-mono);font-variant-numeric:tabular-nums}
.muted{color:var(--text-3)}
.split{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:18px;align-items:start}
.split-2-1{display:grid;grid-template-columns:minmax(0,1.65fr) minmax(0,1fr);gap:18px;align-items:start}
.split-2-1.stretch{align-items:stretch}
.chart-card{display:flex;flex-direction:column}
.chart-body{display:flex;flex-direction:column;flex:1}
.chart-body .chart{flex:1 1 auto;height:auto;min-height:250px}
.status-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.status-tile{display:flex;flex-direction:column;gap:3px;padding:10px 12px;border-radius:10px;background:var(--surface-2);border:1px solid var(--border)}
.status-tile span{font-size:10.4px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--text-3)}
.status-tile b{font-family:var(--font-mono);font-size:16px;font-weight:600;letter-spacing:-.01em}
.inline-stat{display:flex;align-items:baseline;gap:8px}
.inline-stat b{font-family:var(--font-mono);font-size:19px;font-weight:600}
.inline-stat span{font-size:11.6px;color:var(--text-3);text-transform:uppercase;letter-spacing:.07em;font-weight:650}
.skel{background:linear-gradient(90deg,var(--surface-2),var(--surface-3),var(--surface-2));background-size:200% 100%;animation:skel 1.3s linear infinite;border-radius:8px;color:transparent!important}
@keyframes skel{0%{background-position:200% 0}100%{background-position:-200% 0}}
.scrim{display:none}
.sr-only{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}
.notice{
  display:flex;gap:11px;padding:13px 15px;border-radius:11px;background:var(--accent-soft);
  border:1px solid color-mix(in srgb,var(--accent) 32%,transparent);font-size:12.6px;color:var(--text-2);line-height:1.55;
}
.notice strong{color:var(--text)}
.notice.warn{background:var(--warn-soft);border-color:color-mix(in srgb,var(--warn) 32%,transparent)}
.notice.danger{background:var(--danger-soft);border-color:color-mix(in srgb,var(--danger) 32%,transparent)}

/* ======================================================
   RESPONSIVE LAYOUT
   ====================================================== */
@media (max-width:1180px){
  .split-2-1{grid-template-columns:1fr}
  th,td{padding:9px 9px;font-size:12px}
  th{font-size:9.8px;letter-spacing:.06em}
  .tag{font-size:10.4px;padding:2px 6px}
}
@media (max-width:1024px){
  .app-shell{grid-template-columns:1fr}
  .sidebar{
    position:fixed;left:0;top:0;width:min(292px,86vw);height:100dvh;
    transform:translateX(-102%);transition:transform .24s var(--ease);
    box-shadow:var(--shadow-3);
  }
  .sidebar.is-open{transform:none}
  #menuBtn{display:grid}
  .scrim.is-on{display:block;position:fixed;inset:0;z-index:35;background:rgba(3,7,12,.55);backdrop-filter:blur(2px)}
  .content{padding:16px 14px 40px}
  .topbar{padding:10px 14px}
  .channel-grid{grid-template-columns:repeat(auto-fit,minmax(220px,1fr))}
}
@media (max-width:640px){
  body{font-size:14px}
  .section-head{flex-direction:column;align-items:stretch;gap:10px}
  .section-head .btn-group{justify-content:stretch}
  .section-head .btn-group .btn{flex:1 1 0}
  .channel-time{font-size:29px}
  .diag-grid{grid-template-columns:1fr}
  .diag-row{padding:10px 12px}
  .kpi-grid{grid-template-columns:repeat(auto-fit,minmax(148px,1fr));gap:10px}
  .kpi{padding:12px 13px}
  .kpi-value{font-size:20px}
  .channel-grid{grid-template-columns:1fr}
  .form-grid{grid-template-columns:1fr}
  .card-body{padding:14px}
  .card-head{padding:13px 14px}
  .topbar-title h1{font-size:15px}
  .topbar-title p{display:none}
  #toasts{right:10px;left:10px;bottom:10px;max-width:none}
  th,td{padding:9px 12px;font-size:12.4px}
  .btn{height:36px}
}
@media (prefers-reduced-motion:reduce){
  *{animation-duration:.001ms!important;transition-duration:.001ms!important}
}
</style>
</head>
<body>
<div class="app-shell">

<aside class="sidebar" id="sidebar" aria-label="Main navigation">
  <div class="brand">
    <div class="brand-mark" aria-hidden="true">
      <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
        <path d="M12 2.5c3.6 4.2 6 7.4 6 10.5a6 6 0 1 1-12 0c0-3.1 2.4-6.3 6-10.5Z"></path>
        <path d="M9.5 13.6a2.5 2.5 0 0 0 5 0"></path>
      </svg>
    </div>
    <div class="brand-text">
      <strong>Carwash 4-in-1</strong>
      <span id="brandVersion">v1.2.0</span>
    </div>
  </div>

  <nav class="nav">
    <div class="nav-group">
      <span class="nav-label">Overview</span>
      <a class="nav-link" href="#/dashboard" data-route="dashboard">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><rect x="3" y="3" width="7.5" height="7.5" rx="1.6"></rect><rect x="13.5" y="3" width="7.5" height="7.5" rx="1.6"></rect><rect x="3" y="13.5" width="7.5" height="7.5" rx="1.6"></rect><rect x="13.5" y="13.5" width="7.5" height="7.5" rx="1.6"></rect></svg>
        Dashboard
      </a>
      <a class="nav-link" href="#/inventory" data-route="inventory">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M4 20V9.5L12 4l8 5.5V20"></path><path d="M9.5 20v-5.5h5V20"></path></svg>
        Sales Inventory
      </a>
    </div>

    <div class="nav-group">
      <span class="nav-label">Settings</span>
      <a class="nav-link" href="#/settings/general" data-route="settings-general">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><circle cx="12" cy="12" r="3.2"></circle><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.88l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.7 1.7 0 0 0-2.9 1.2V21a2 2 0 1 1-4 0v-.1A1.7 1.7 0 0 0 7.7 19.4l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.7 1.7 0 0 0 3 15H3a2 2 0 1 1 0-4h.1A1.7 1.7 0 0 0 4.6 7.7l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.7 1.7 0 0 0 9 3h.1A2 2 0 1 1 13 3v.1a1.7 1.7 0 0 0 2.9 1.2l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06A1.7 1.7 0 0 0 21 9h.1a2 2 0 1 1 0 4H21"></path></svg>
        General
      </a>
      <a class="nav-link" href="#/settings/coin" data-route="settings-coin">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><circle cx="12" cy="12" r="8.4"></circle><path d="M9.4 9.2h5.2M9.4 12h5.2M12 7.4v9.2"></path></svg>
        Coin &amp; Bill Acceptor
      </a>
      <a class="nav-link" href="#/settings/lcd" data-route="settings-lcd">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><rect x="2.6" y="5" width="18.8" height="12" rx="2"></rect><path d="M6.4 9.4h4M6.4 12.6h7M6.4 15.8h3"></path></svg>
        LCD 20x4
      </a>
      <a class="nav-link" href="#/settings/io" data-route="settings-io">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3"></path><rect x="7" y="7" width="10" height="10" rx="2"></rect></svg>
        Buttons &amp; Relays
      </a>
      <a class="nav-link" href="#/settings/buzzer" data-route="settings-buzzer">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M11 5.5 6.6 9H3.4v6h3.2L11 18.5z"></path><path d="M15.4 8.6a4.6 4.6 0 0 1 0 6.8M18.2 5.8a8.6 8.6 0 0 1 0 12.4"></path></svg>
        Buzzer
      </a>
      <a class="nav-link" href="#/settings/wifi" data-route="settings-wifi">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M2.6 8.8a15 15 0 0 1 18.8 0M5.8 12.3a10.4 10.4 0 0 1 12.4 0M9 15.8a5.8 5.8 0 0 1 6 0"></path><circle cx="12" cy="19.3" r="1.1" fill="currentColor" stroke="none"></circle></svg>
        Wi-Fi Access Point
      </a>
      <a class="nav-link" href="#/settings/system" data-route="settings-system">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M12 3.2 20 7v5.6c0 4.4-3.2 7.4-8 8.6-4.8-1.2-8-4.2-8-8.6V7z"></path><path d="M9.2 12.2l2 2 3.6-3.8"></path></svg>
        System &amp; Maintenance
      </a>
    </div>
  </nav>

  <div class="sidebar-foot">
    <div class="row"><span>Device</span><span id="footIp">192.168.4.1</span></div>
    <div class="row"><span>Uptime</span><span id="footUptime">--</span></div>
  </div>
</aside>

<div class="scrim" id="scrim" aria-hidden="true"></div>

<main class="main">
  <header class="topbar">
    <button class="icon-btn" id="menuBtn" aria-label="Open navigation menu" aria-expanded="false">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3.5 6.5h17M3.5 12h17M3.5 17.5h17"></path></svg>
    </button>
    <div class="topbar-title">
      <h1 id="pageTitle">Dashboard</h1>
      <p id="pageSub">Live machine status, credit and all time sales</p>
    </div>
    <div class="topbar-actions">
      <span class="pill" id="connPill"><i class="dot"></i><span id="connText">Connecting</span></span>
      <button class="icon-btn" id="themeBtn" aria-label="Toggle colour theme">
        <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M20.4 14.3A8.6 8.6 0 1 1 9.7 3.6a6.9 6.9 0 0 0 10.7 10.7Z"></path></svg>
      </button>
    </div>
  </header>

  <div class="content">

    <!-- ======================================================
         VIEW DASHBOARD
         ====================================================== -->
    <section class="view" id="view-dashboard" aria-labelledby="dashboardTitle">
      <div class="stack">

        <div class="kpi-grid">
          <div class="kpi k-ok">
            <div class="kpi-label">Credit pool</div>
            <div class="kpi-value" id="kpiCredit">--:--</div>
            <div class="kpi-sub"><span id="kpiCreditSub">waiting for data</span></div>
          </div>
          <div class="kpi k-water">
            <div class="kpi-label">Active functions</div>
            <div class="kpi-value" id="kpiActive">0<small>/ 4</small></div>
            <div class="kpi-sub" id="kpiActiveSub">all idle</div>
          </div>
          <div class="kpi k-soap">
            <div class="kpi-label">All time sales</div>
            <div class="kpi-value" id="kpiRevenue">--</div>
            <div class="kpi-sub" id="kpiRevenueSub">no sales yet</div>
          </div>
          <div class="kpi k-foam">
            <div class="kpi-label">Coins accepted</div>
            <div class="kpi-value" id="kpiCoins">0</div>
            <div class="kpi-sub" id="kpiCoinsSub">0 pulses</div>
          </div>
          <div class="kpi k-blower">
            <div class="kpi-label">Time dispensed</div>
            <div class="kpi-value" id="kpiDispensed">0<small>min</small></div>
            <div class="kpi-sub" id="kpiDispensedSub">all functions</div>
          </div>
        </div>

        <div>
          <div class="section-head">
            <div>
              <h2 id="dashboardTitle">Function control</h2>
              <p>Push a function button once to start, twice to pause or resume, hold to add time.</p>
            </div>
            <div class="btn-group">
              <button class="btn btn-sm" id="btnStartAll" type="button">Start all</button>
              <button class="btn btn-sm" id="btnStopAll" type="button">Stop all</button>
            </div>
          </div>
          <div class="channel-grid" id="channelGrid"></div>
        </div>

        <div class="split-2-1 stretch">
          <div class="card chart-card">
            <div class="card-head">
              <div>
                <h3>Sales performance</h3>
                <span id="chartRangeLabel">last 14 operating days</span>
              </div>
              <div class="legend">
                <span><i style="background:var(--accent)"></i>Revenue</span>
                <span><i style="background:var(--soap)"></i>Machine minutes</span>
              </div>
            </div>
            <div class="card-body chart-body">
              <svg class="chart" id="salesChart" viewBox="0 0 720 240" role="img" aria-label="Revenue and machine minutes per day"></svg>
            </div>
          </div>

          <div class="stack">
            <div class="card">
              <div class="card-head"><h3>LCD 20x4</h3><span id="lcdStateLabel">live</span></div>
              <div class="card-body">
                <div class="lcd-frame">
                  <div class="lcd-screen" id="lcdScreen">WATER --:-- OFF
BLOWER --:-- OFF
SOAP  --:-- OFF
FOAM  --:-- OFF</div>
                  <div class="lcd-meta"><span id="lcdAddrLabel">0x27</span><span id="lcdBusLabel">SDA 21 / SCL 22</span></div>
                </div>
              </div>
            </div>

            <div class="card">
              <div class="card-head"><h3>System status</h3><span id="statusUpdated">--</span></div>
              <div class="card-body">
                <div class="status-grid">
                  <div class="status-tile"><span>Wi-Fi AP</span><b id="statWifi">--</b></div>
                  <div class="status-tile"><span>Clients</span><b id="statClients">0</b></div>
                  <div class="status-tile"><span>Free heap</span><b id="statHeap">--</b></div>
                  <div class="status-tile"><span>LCD bus</span><b id="statLcd">--</b></div>
                </div>
              </div>
            </div>
          </div>
        </div>

        <div class="split">
          <div class="card">
            <div class="card-head"><h3>Usage share</h3><span>runtime distribution</span></div>
            <div class="card-body">
              <div class="donut-wrap">
                <svg class="donut" id="donutChart" width="150" height="150" viewBox="0 0 150 150" role="img" aria-label="Usage share by function"></svg>
                <div class="donut-legend" id="donutLegend"></div>
              </div>
            </div>
          </div>

          <div class="card">
            <div class="card-head"><h3>Recent activity</h3><span>last sessions</span></div>
            <div class="card-body tight">
              <div class="table-wrap">
                <table>
                  <thead><tr><th>Function</th><th>Duration</th><th>Ended</th><th>End reason</th></tr></thead>
                  <tbody id="recentBody"><tr><td colspan="4" class="empty">No sessions recorded yet</td></tr></tbody>
                </table>
              </div>
            </div>
          </div>
        </div>

      </div>
    </section>

    <!-- ======================================================
         VIEW SALES INVENTORY
         ====================================================== -->
    <section class="view" id="view-inventory" aria-labelledby="inventoryTitle">
      <div class="stack">

        <div class="kpi-grid">
          <div class="kpi k-ok">
            <div class="kpi-label">Gross sales</div>
            <div class="kpi-value" id="invRevenue">--</div>
            <div class="kpi-sub">all time, all functions</div>
          </div>
          <div class="kpi k-water">
            <div class="kpi-label">Coins</div>
            <div class="kpi-value" id="invCoins">0</div>
            <div class="kpi-sub" id="invPulsesSub">0 pulses</div>
          </div>
          <div class="kpi k-blower">
            <div class="kpi-label">Sessions</div>
            <div class="kpi-value" id="invSessions">0</div>
            <div class="kpi-sub">completed runs</div>
          </div>
          <div class="kpi k-warn">
            <div class="kpi-label">Bonus time</div>
            <div class="kpi-value" id="invBonus">0<small>min</small></div>
            <div class="kpi-sub">granted by long press</div>
          </div>
          <div class="kpi k-soap">
            <div class="kpi-label">Avg ticket</div>
            <div class="kpi-value" id="invAvg">--</div>
            <div class="kpi-sub">sales per coin</div>
          </div>
        </div>

        <div class="card">
          <div class="card-head">
            <div><h3 id="inventoryTitle">Inventory usage by function</h3><span>runtime, sessions and share of total</span></div>
            <button class="btn btn-sm" id="btnExportInventory" type="button">Export CSV</button>
          </div>
          <div class="card-body tight">
            <div class="table-wrap">
              <table>
                <thead>
                  <tr><th>Function</th><th>Runtime</th><th>Sessions</th><th>Avg session</th><th>Share</th><th>Relay</th><th>Button</th></tr>
                </thead>
                <tbody id="inventoryBody"><tr><td colspan="7" class="empty">Loading inventory</td></tr></tbody>
              </table>
            </div>
          </div>
        </div>

        <div class="split">
          <div class="card">
            <div class="card-head"><h3>Daily ledger</h3><span>most recent first</span></div>
            <div class="card-body tight">
              <div class="table-wrap" style="max-height:420px;overflow-y:auto">
                <table>
                  <thead><tr><th>Operating day</th><th>Revenue</th><th>Coins</th><th>Minutes</th></tr></thead>
                  <tbody id="ledgerBody"><tr><td colspan="4" class="empty">No ledger data</td></tr></tbody>
                </table>
              </div>
            </div>
          </div>

          <div class="card">
            <div class="card-head"><h3>Coin configuration</h3><span>pulse to time mapping</span></div>
            <div class="card-body tight">
              <div class="table-wrap">
                <table>
                  <thead><tr><th>Pulses</th><th>Time value</th><th>Coin value</th><th>Per minute</th></tr></thead>
                  <tbody id="coinMapBody"><tr><td colspan="4" class="empty">No presets</td></tr></tbody>
                </table>
              </div>
            </div>
          </div>
        </div>

      </div>
    </section>

    <!-- ======================================================
         VIEW SETTINGS GENERAL
         ====================================================== -->
    <section class="view" id="view-settings-general" aria-labelledby="generalTitle">
      <div class="stack">
        <div class="notice">
          <div><strong>Time model.</strong> Choose how coin credit is consumed. Shared pool lets any combination of functions run at the same time; allocated hands the whole credit to the function you start.</div>
        </div>

        <form class="card" data-settings-form="general" novalidate>
          <div class="card-head"><div><h3 id="generalTitle">General operation</h3><span>credit model, timing and access</span></div></div>
          <div class="card-body">
            <div class="form-grid">
              <div class="field">
                <label for="setTimeModel">Time model</label>
                <select class="select" id="setTimeModel" data-field="timeModel">
                  <option value="0">Shared pool, single drain rate</option>
                  <option value="1">Shared pool, drain per active function</option>
                  <option value="2">Allocated per function</option>
                </select>
                <div class="hint">Shared pool: one credit for all four functions. Allocated: credit is moved into the function you start.</div>
              </div>
              <div class="field">
                <label for="setLongAdd">Long press add time</label>
                <div class="inline-stat" style="gap:8px">
                  <input class="input" id="setLongAdd" type="number" min="0" max="3600" step="10" data-field="longPressAddSeconds" style="max-width:130px">
                  <span class="muted" style="font-size:12px">seconds</span>
                </div>
                <div class="hint">Holding a function button for the long press threshold adds this many seconds.</div>
              </div>
              <div class="field">
                <label>Bonus time from long press</label>
                <label class="switch">
                  <input type="checkbox" data-field="bonusEnabled" data-type="bool" id="setBonusEnabled">
                  <span class="track"></span>
                  <span class="label">Allow bonus seconds</span>
                </label>
                <div class="hint">When disabled, long press only moves unallocated credit and never grants free time.</div>
              </div>
              <div class="field">
                <label for="setMaxBonus">Maximum bonus per credit cycle</label>
                <input class="input" id="setMaxBonus" type="number" min="0" max="86400" step="30" data-field="maxBonusSeconds">
                <div class="hint">Bonus budget resets every time a coin is accepted.</div>
              </div>
              <div class="field">
                <label for="setLongMs">Long press threshold</label>
                <input class="input" id="setLongMs" type="number" min="200" max="5000" step="50" data-field="longPressMs">
                <div class="hint">Milliseconds the button must be held to count as a long press.</div>
              </div>
              <div class="field">
                <label for="setDoubleMs">Double press window</label>
                <input class="input" id="setDoubleMs" type="number" min="100" max="2000" step="50" data-field="doubleClickMs">
                <div class="hint">Maximum gap between two presses to register a pause or resume gesture.</div>
              </div>
              <div class="field">
                <label for="setDebounce">Button debounce</label>
                <input class="input" id="setDebounce" type="number" min="5" max="200" step="5" data-field="debounceMs">
                <div class="hint">Contact settling time in milliseconds.</div>
              </div>
              <div class="field">
                <label for="setMaxRun">Safety maximum run time</label>
                <input class="input" id="setMaxRun" type="number" min="60" max="86400" step="60" data-field="maxRunSeconds">
                <div class="hint">A function is force stopped after this many seconds, protecting pumps and valves.</div>
              </div>
              <div class="field">
                <label for="setMaxCredit">Maximum credit balance</label>
                <input class="input" id="setMaxCredit" type="number" min="60" max="86400" step="60" data-field="maxCreditSeconds">
                <div class="hint">Upper limit on the credit pool in seconds.</div>
              </div>
              <div class="field">
                <label for="setStatsSave">Statistics save interval</label>
                <input class="input" id="setStatsSave" type="number" min="30" max="3600" step="30" data-field="statsSaveIntervalSec">
                <div class="hint">Seconds between flash writes. Longer intervals extend flash life.</div>
              </div>
              <div class="field">
                <label for="setFactoryHold">Factory reset hold time</label>
                <input class="input" id="setFactoryHold" type="number" min="1000" max="30000" step="500" data-field="factoryHoldMs">
                <div class="hint">Milliseconds the onboard BOOT button must be held to erase all data.</div>
              </div>
              <div class="field">
                <label for="setAdminPin">Panel PIN</label>
                <input class="input" id="setAdminPin" type="text" inputmode="numeric" maxlength="8" data-field="adminPin" placeholder="empty = no PIN">
                <div class="hint">Digits only. When set, every write action asks for this PIN.</div>
              </div>
              <div class="field">
                <label>Low heap guard</label>
                <label class="switch">
                  <input type="checkbox" data-field="lowHeapRestart" data-type="bool" id="setLowHeap">
                  <span class="track"></span>
                  <span class="label">Restart on low heap</span>
                </label>
                <div class="hint">Reboots automatically when free heap stays below the threshold.</div>
              </div>
              <div class="field">
                <label for="setLowHeapBytes">Low heap threshold</label>
                <input class="input" id="setLowHeapBytes" type="number" min="4096" max="131072" step="1024" data-field="lowHeapBytes">
                <div class="hint">Bytes of free heap considered critical.</div>
              </div>
            </div>
            <div class="form-actions">
              <button class="btn btn-primary" type="submit">Save general settings</button>
              <button class="btn" type="button" data-reload>Reload</button>
              <div class="spacer"></div>
              <span class="muted" style="font-size:11.6px" data-revision>--</span>
            </div>
          </div>
        </form>
      </div>
    </section>

    <!-- ======================================================
         VIEW SETTINGS COIN AND BILL ACCEPTOR
         ====================================================== -->
    <section class="view" id="view-settings-coin" aria-labelledby="coinTitle">
      <div class="stack">
        <div class="notice">
          <div><strong>Wiring.</strong> Coin or bill acceptor pulse line goes to the coin GPIO with a common ground. The optional pulse output mirrors every accepted pulse for an external counter or bill validator handshake.</div>
        </div>

        <form class="card" data-settings-form="coin" novalidate>
          <div class="card-head"><div><h3 id="coinTitle">Acceptor interface</h3><span>GPIO 25 coin pulse input, GPIO 26 pulse output</span></div></div>
          <div class="card-body">
            <div class="form-grid">
              <div class="field">
                <label for="setCoinPin">Coin pulse input pin</label>
                <input class="input" id="setCoinPin" type="number" min="0" max="39" data-field="coinPin" data-pin="1">
                <div class="hint">Default GPIO 25, falling edge with internal pull-up.</div>
              </div>
              <div class="field">
                <label for="setPulseOutEnabled">Pulse output</label>
                <label class="switch">
                  <input type="checkbox" data-field="pulseOutEnabled" data-type="bool" id="setPulseOutEnabled">
                  <span class="track"></span>
                  <span class="label">Mirror accepted pulses</span>
                </label>
                <div class="hint">Drives the pulse output pin for every pulse that is accepted.</div>
              </div>
              <div class="field">
                <label for="setPulseOutPin">Pulse output pin</label>
                <input class="input" id="setPulseOutPin" type="number" min="0" max="39" data-field="pulseOutPin" data-pin="1">
                <div class="hint">Default GPIO 26.</div>
              </div>
              <div class="field">
                <label for="setPulseWidth">Pulse output width</label>
                <input class="input" id="setPulseWidth" type="number" min="10" max="1000" step="5" data-field="pulseOutWidthMs">
                <div class="hint">Milliseconds the output stays active per pulse.</div>
              </div>
              <div class="field">
                <label for="setCoinGap">Minimum pulse gap</label>
                <input class="input" id="setCoinGap" type="number" min="5" max="500" step="5" data-field="coinMinPulseGapMs">
                <div class="hint">Pulses closer than this are treated as contact bounce.</div>
              </div>
              <div class="field">
                <label for="setGroupGap">Coin group gap</label>
                <input class="input" id="setGroupGap" type="number" min="50" max="5000" step="10" data-field="coinGroupGapMs">
                <div class="hint">Silence after the last pulse before a coin is committed.</div>
              </div>
              <div class="field">
                <label for="setMaxPulses">Maximum pulses per coin</label>
                <input class="input" id="setMaxPulses" type="number" min="1" max="200" data-field="coinMaxPulses">
                <div class="hint">Safety cap for a single coin or bill burst.</div>
              </div>
              <div class="field">
                <label for="setCoinCount">Active presets</label>
                <select class="select" id="setCoinCount" data-field="coinMapCount">
                  <option value="1">1 preset</option>
                  <option value="2">2 presets</option>
                  <option value="3">3 presets</option>
                  <option value="4">4 presets</option>
                  <option value="5">5 presets</option>
                  <option value="6">6 presets</option>
                  <option value="7">7 presets</option>
                  <option value="8">8 presets</option>
                </select>
                <div class="hint">How many pulse to time mappings are enabled.</div>
              </div>
            </div>
          </div>
        </form>

        <form class="card" data-settings-form="coin" novalidate>
          <div class="card-head"><div><h3>Pulse to time mapping</h3><span>1 pulse = 5 minutes, 5 pulses = 25 minutes, 10 pulses = 50 minutes, 20 pulses = 1 hour</span></div></div>
          <div class="card-body tight">
            <div class="table-wrap">
              <table>
                <thead><tr><th>Preset</th><th>Pulses</th><th>Minutes</th><th>Coin value (centavos)</th><th>Result</th></tr></thead>
                <tbody id="coinPresetBody"></tbody>
              </table>
            </div>
            <div class="form-actions" style="margin:0;padding:14px 18px">
              <button class="btn btn-primary" type="submit">Save acceptor settings</button>
              <button class="btn" type="button" data-reload>Reload</button>
              <div class="spacer"></div>
              <span class="muted" style="font-size:11.6px" data-revision>--</span>
            </div>
          </div>
        </form>

        <div class="split">
          <div class="card">
            <div class="card-head"><h3>Live pulse monitor</h3><span id="coinMonitorState">idle</span></div>
            <div class="card-body">
              <div class="stack" style="gap:12px">
                <div class="inline-stat"><b id="monPulses">0</b><span>total pulses</span></div>
                <div class="inline-stat"><b id="monCoins">0</b><span>coins accepted</span></div>
                <div class="inline-stat"><b id="monLast">--</b><span>last coin</span></div>
                <div class="btn-group">
                  <button class="btn btn-sm" type="button" data-sim-coin="1">Simulate 1 pulse</button>
                  <button class="btn btn-sm" type="button" data-sim-coin="5">5 pulses</button>
                  <button class="btn btn-sm" type="button" data-sim-coin="10">10 pulses</button>
                  <button class="btn btn-sm" type="button" data-sim-coin="20">20 pulses</button>
                </div>
              </div>
            </div>
          </div>

          <div class="card">
            <div class="card-head"><h3>Credit tools</h3><span>owner actions</span></div>
            <div class="card-body">
              <div class="stack" style="gap:12px">
                <div class="field">
                  <label for="creditSeconds">Add credit manually</label>
                  <div class="btn-group">
                    <input class="input" id="creditSeconds" type="number" min="1" max="86400" value="60" style="max-width:130px">
                    <button class="btn btn-sm" type="button" id="btnAddCredit">Add seconds</button>
                  </div>
                  <div class="hint">Useful for testing and for owner granted time. Credit is logged as credit added, not as sales.</div>
                </div>
                <div class="notice warn" style="margin-top:4px"><div>The pulse simulator and credit tools change machine state immediately.</div></div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- ======================================================
         VIEW SETTINGS LCD
         ====================================================== -->
    <section class="view" id="view-settings-lcd" aria-labelledby="lcdTitle">
      <div class="stack">
        <div class="notice">
          <div><strong>Wiring.</strong> I2C LCD backpack SDA to GPIO 21 and SCL to GPIO 22, 5 V and ground. The controller probes the bus continuously and keeps running if the display is unplugged.</div>
        </div>

        <div class="split-2-1">
          <form class="card" data-settings-form="lcd" novalidate>
            <div class="card-head"><div><h3 id="lcdTitle">Display configuration</h3><span>20 columns by 4 rows, parallel channel indicators</span></div></div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="setLcdAddr">I2C address</label>
                  <select class="select" id="setLcdAddr" data-field="lcdAddr">
                    <option value="39">0x27 (39)</option>
                    <option value="63">0x3F (63)</option>
                    <option value="32">0x20 (32)</option>
                    <option value="56">0x38 (56)</option>
                  </select>
                  <div class="hint">Most PCF8574 backpacks use 0x27 or 0x3F.</div>
                </div>
                <div class="field">
                  <label for="setLcdSda">SDA pin</label>
                  <input class="input" id="setLcdSda" type="number" min="0" max="39" data-field="lcdSda" data-pin="1">
                  <div class="hint">Default GPIO 21.</div>
                </div>
                <div class="field">
                  <label for="setLcdScl">SCL pin</label>
                  <input class="input" id="setLcdScl" type="number" min="0" max="39" data-field="lcdScl" data-pin="1">
                  <div class="hint">Default GPIO 22.</div>
                </div>
                <div class="field">
                  <label for="setLcdCols">Columns</label>
                  <select class="select" id="setLcdCols" data-field="lcdCols">
                    <option value="20">20</option>
                    <option value="16">16</option>
                  </select>
                </div>
                <div class="field">
                  <label for="setLcdRows">Rows</label>
                  <select class="select" id="setLcdRows" data-field="lcdRows">
                    <option value="4">4</option>
                    <option value="2">2</option>
                  </select>
                </div>
                <div class="field">
                  <label>Backlight</label>
                  <label class="switch">
                    <input type="checkbox" data-field="lcdBacklight" data-type="bool" id="setLcdBacklight">
                    <span class="track"></span>
                    <span class="label">Backlight on</span>
                  </label>
                </div>
                <div class="field">
                  <label for="setLcdRefresh">Refresh interval</label>
                  <input class="input" id="setLcdRefresh" type="number" min="50" max="2000" step="25" data-field="lcdRefreshMs">
                  <div class="hint">Only changed rows are rewritten, so a longer interval is safe.</div>
                </div>
                <div class="field">
                  <label for="setLcdBlink">Blink interval</label>
                  <input class="input" id="setLcdBlink" type="number" min="120" max="2000" step="20" data-field="lcdBlinkMs">
                  <div class="hint">Blink rate for the remaining time of each paused function.</div>
                </div>
              </div>
              <div class="form-actions">
                <button class="btn btn-primary" type="submit">Save display settings</button>
                <button class="btn" type="button" data-reload>Reload</button>
                <div class="spacer"></div>
                <span class="muted" style="font-size:11.6px" data-revision>--</span>
              </div>
            </div>
          </form>

          <div class="card">
            <div class="card-head"><h3>Live mirror</h3><span id="lcdMirrorState">live</span></div>
            <div class="card-body">
              <div class="lcd-frame">
                <div class="lcd-screen" id="lcdScreenBig">WATER --:-- OFF
BLOWER --:-- OFF
SOAP  --:-- OFF
FOAM  --:-- OFF</div>
                <div class="lcd-meta"><span id="lcdAddrLabel2">0x27</span><span id="lcdBusLabel2">SDA 21 / SCL 22</span></div>
              </div>
              <div class="notice" style="margin-top:14px">
                <div>Row layout: function name, remaining time, state. Each paused function blinks its counter; the display stops blinking when it resumes.</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- ======================================================
         VIEW SETTINGS BUTTONS AND RELAYS
         ====================================================== -->
    <section class="view" id="view-settings-io" aria-labelledby="ioTitle">
      <div class="stack">
        <div class="notice">
          <div><strong>Wiring.</strong> Buttons connect the GPIO to ground, no external resistor needed. Relays drive from the listed GPIO through an opto isolated module; set the trigger polarity to match your module.</div>
        </div>

        <form class="card" data-settings-form="io" novalidate>
          <div class="card-head"><div><h3 id="ioTitle">Relay outputs</h3><span>WATER 19, BLOWER 18, SOAP 17, FOAM 16 by default</span></div></div>
          <div class="card-body">
            <div class="form-grid">
              <div class="field">
                <label for="setRelayWater">WATER relay pin</label>
                <input class="input" id="setRelayWater" type="number" min="0" max="39" data-field="relayPin0" data-pin="1">
              </div>
              <div class="field">
                <label for="setRelayBlower">BLOWER relay pin</label>
                <input class="input" id="setRelayBlower" type="number" min="0" max="39" data-field="relayPin1" data-pin="1">
              </div>
              <div class="field">
                <label for="setRelaySoap">SOAP relay pin</label>
                <input class="input" id="setRelaySoap" type="number" min="0" max="39" data-field="relayPin2" data-pin="1">
              </div>
              <div class="field">
                <label for="setRelayFoam">FOAM relay pin</label>
                <input class="input" id="setRelayFoam" type="number" min="0" max="39" data-field="relayPin3" data-pin="1">
              </div>
              <div class="field">
                <label>Relay trigger polarity</label>
                <label class="switch">
                  <input type="checkbox" data-field="relayActiveLow" data-type="bool" id="setRelayActiveLow">
                  <span class="track"></span>
                  <span class="label">Active low module</span>
                </label>
                <div class="hint">Enabled for the common low level triggered relay board.</div>
              </div>
            </div>
          </div>
        </form>

        <form class="card" data-settings-form="io" novalidate>
          <div class="card-head"><div><h3>Push buttons</h3><span>WATER 32, BLOWER 33, SOAP 27, FOAM 14 by default</span></div></div>
          <div class="card-body">
            <div class="form-grid">
              <div class="field">
                <label for="setBtnWater">WATER button pin</label>
                <input class="input" id="setBtnWater" type="number" min="0" max="39" data-field="btnPin0" data-pin="1">
              </div>
              <div class="field">
                <label for="setBtnBlower">BLOWER button pin</label>
                <input class="input" id="setBtnBlower" type="number" min="0" max="39" data-field="btnPin1" data-pin="1">
              </div>
              <div class="field">
                <label for="setBtnSoap">SOAP button pin</label>
                <input class="input" id="setBtnSoap" type="number" min="0" max="39" data-field="btnPin2" data-pin="1">
              </div>
              <div class="field">
                <label for="setBtnFoam">FOAM button pin</label>
                <input class="input" id="setBtnFoam" type="number" min="0" max="39" data-field="btnPin3" data-pin="1">
              </div>
            </div>
            <div class="form-actions">
              <button class="btn btn-primary" type="submit">Save pin configuration</button>
              <button class="btn" type="button" data-reload>Reload</button>
              <div class="spacer"></div>
              <span class="muted" style="font-size:11.6px" data-revision>--</span>
            </div>
          </div>
        </form>

        <div class="card">
          <div class="card-head"><h3>Pin map</h3><span>current assignment</span></div>
          <div class="card-body">
            <div class="pin-map" id="pinMap"></div>
          </div>
        </div>
      </div>
    </section>

    <!-- ======================================================
         VIEW SETTINGS BUZZER
         ====================================================== -->
    <section class="view" id="view-settings-buzzer" aria-labelledby="buzzerTitle">
      <div class="stack">
        <div class="notice">
          <div><strong>Wiring.</strong> Passive or active buzzer positive leg to GPIO 23, negative to ground. Use a transistor driver for buzzers that draw more than 20 mA.</div>
        </div>

        <div class="split-2-1">
          <form class="card" data-settings-form="buzzer" novalidate>
            <div class="card-head"><div><h3 id="buzzerTitle">Buzzer output</h3><span>GPIO 23 default</span></div></div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="setBuzzerPin">Buzzer pin</label>
                  <input class="input" id="setBuzzerPin" type="number" min="0" max="39" data-field="buzzerPin" data-pin="1">
                </div>
                <div class="field">
                  <label>Buzzer enabled</label>
                  <label class="switch">
                    <input type="checkbox" data-field="buzzerEnabled" data-type="bool" id="setBuzzerEnabled">
                    <span class="track"></span>
                    <span class="label">Audible feedback</span>
                  </label>
                </div>
                <div class="field">
                  <label for="setBuzzerFreq">Confirmation tone</label>
                  <input class="input" id="setBuzzerFreq" type="number" min="100" max="10000" step="50" data-field="buzzerFreq">
                  <div class="hint">Hertz used for confirmations.</div>
                </div>
                <div class="field">
                  <label for="setBuzzerBeep">Beep duration</label>
                  <input class="input" id="setBuzzerBeep" type="number" min="10" max="1000" step="5" data-field="buzzerBeepMs">
                  <div class="hint">Milliseconds per beep.</div>
                </div>
                <div class="field">
                  <label for="setBuzzerErr">Error tone</label>
                  <input class="input" id="setBuzzerErr" type="number" min="100" max="10000" step="50" data-field="buzzerErrorFreq">
                  <div class="hint">Hertz used when a function cannot start.</div>
                </div>
              </div>
              <div class="form-actions">
                <button class="btn btn-primary" type="submit">Save buzzer settings</button>
                <button class="btn" type="button" data-reload>Reload</button>
                <div class="spacer"></div>
                <span class="muted" style="font-size:11.6px" data-revision>--</span>
              </div>
            </div>
          </form>

          <div class="card">
            <div class="card-head"><h3>Pattern test</h3><span>plays on the machine</span></div>
            <div class="card-body">
              <div class="btn-group">
                <button class="btn btn-sm" type="button" data-buzz="1">Boot</button>
                <button class="btn btn-sm" type="button" data-buzz="2">Coin</button>
                <button class="btn btn-sm" type="button" data-buzz="3">Start</button>
                <button class="btn btn-sm" type="button" data-buzz="4">Pause</button>
                <button class="btn btn-sm" type="button" data-buzz="5">Stop</button>
                <button class="btn btn-sm" type="button" data-buzz="6">Error</button>
                <button class="btn btn-sm" type="button" data-buzz="7">Click</button>
                <button class="btn btn-sm" type="button" data-buzz="10">Wi-Fi</button>
              </div>
              <div class="notice" style="margin-top:14px"><div>Patterns play silently when the buzzer is disabled.</div></div>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- ======================================================
         VIEW SETTINGS WIFI AP
         ====================================================== -->
    <section class="view" id="view-settings-wifi" aria-labelledby="wifiTitle">
      <div class="stack">
        <div class="notice">
          <div><strong>Captive portal.</strong> Every DNS query and unknown HTTP host is redirected to http://192.168.4.1 so phones open the panel automatically after joining the network.</div>
        </div>

        <div class="split-2-1">
          <form class="card" data-settings-form="wifi" novalidate>
            <div class="card-head"><div><h3 id="wifiTitle">Access point</h3><span>default ESP32_4in1_Carwash / 12345678</span></div></div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="setSsid">Network name (SSID)</label>
                  <input class="input" id="setSsid" type="text" maxlength="32" data-field="apSsid" autocomplete="off">
                  <div class="hint">1 to 32 printable characters.</div>
                </div>
                <div class="field">
                  <label for="setPass">Password</label>
                  <input class="input" id="setPass" type="password" maxlength="63" data-field="apPass" autocomplete="new-password">
                  <div class="hint">8 to 63 characters, or empty for an open network.</div>
                </div>
                <div class="field">
                  <label for="setChannel">Channel</label>
                  <input class="input" id="setChannel" type="number" min="1" max="13" data-field="apChannel">
                  <div class="hint">1 to 13.</div>
                </div>
                <div class="field">
                  <label for="setMaxClients">Maximum clients</label>
                  <input class="input" id="setMaxClients" type="number" min="1" max="8" data-field="apMaxClients">
                </div>
                <div class="field">
                  <label>Hidden network</label>
                  <label class="switch">
                    <input type="checkbox" data-field="apHidden" data-type="bool" id="setHidden">
                    <span class="track"></span>
                    <span class="label">Do not broadcast SSID</span>
                  </label>
                </div>
              </div>
              <div class="form-actions">
                <button class="btn btn-primary" type="submit">Save Wi-Fi settings</button>
                <button class="btn" type="button" data-reload>Reload</button>
                <div class="spacer"></div>
                <span class="muted" style="font-size:11.6px" data-revision>--</span>
              </div>
              <div class="notice warn" style="margin-top:14px">
                <div>Saving new credentials restarts the access point within one second. Reconnect with the new network name afterwards.</div>
              </div>
            </div>
          </form>

          <div class="card">
            <div class="card-head"><h3>Live network</h3><span id="wifiStateLabel">--</span></div>
            <div class="card-body">
              <div class="stack" style="gap:12px">
                <div class="inline-stat"><b id="wifiSsidLive">--</b><span>SSID</span></div>
                <div class="inline-stat"><b id="wifiClients">0</b><span>clients connected</span></div>
                <div class="inline-stat"><b id="wifiIp">192.168.4.1</b><span>panel address</span></div>
                <div class="inline-stat"><b id="wifiRestarts">0</b><span>AP restarts</span></div>
                <div class="notice"><div>The panel is reachable at <strong>http://192.168.4.1</strong> while connected to this network.</div></div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- ======================================================
         VIEW SETTINGS SYSTEM AND MAINTENANCE
         ====================================================== -->
    <section class="view" id="view-settings-system" aria-labelledby="systemTitle">
      <div class="stack">

        <div class="card">
          <div class="card-head"><div><h3 id="systemTitle">Device diagnostics</h3><span>live telemetry</span></div><button class="btn btn-sm" type="button" id="btnRefreshDiag">Refresh</button></div>
          <div class="card-body tight">
            <div class="diag-grid" id="diagBody">
              <div class="diag-row"><span>Loading diagnostics</span><b>--</b></div>
            </div>
          </div>
        </div>

        <div class="split">
          <div class="card">
            <div class="card-head"><h3>Configuration backup</h3><span>portable text format</span></div>
            <div class="card-body">
              <div class="stack" style="gap:12px">
                <button class="btn btn-block" type="button" id="btnExportConfig">Download configuration</button>
                <label class="btn btn-block" for="importFile" style="cursor:pointer">Restore configuration</label>
                <input class="sr-only" type="file" id="importFile" accept=".txt,.cfg,text/plain">
                <div class="hint muted" style="font-size:11.4px">Restoring applies every stored value and re-saves it to flash in one operation.</div>
              </div>
            </div>
          </div>

          <div class="card">
            <div class="card-head"><h3>Statistics</h3><span>flash persistence</span></div>
            <div class="card-body">
              <div class="stack" style="gap:12px">
                <button class="btn btn-block" type="button" id="btnSaveStats">Commit statistics now</button>
                <button class="btn btn-block" type="button" id="btnClearSessions">Clear session journal</button>
                <div class="inline-stat"><b id="nvsWrites">0</b><span>total flash writes</span></div>
                <div class="hint muted" style="font-size:11.4px">Statistics are written at most every configured interval, which keeps flash wear far below its endurance limit.</div>
              </div>
            </div>
          </div>
        </div>

  <div class="card" style="border-color:color-mix(in srgb,var(--danger) 34%,var(--border))">
          <div class="card-head" style="background:var(--danger-soft)"><div><h3>Danger zone</h3><span>irreversible actions</span></div></div>
          <div class="card-body">
            <div class="stack" style="gap:14px">
              <div class="notice danger"><div><strong>Factory reset</strong> erases configuration, sales statistics, session journal and the stored network credentials. The device restarts with the ESP32_4in1_Carwash network and default pins.</div></div>
              <div class="btn-group">
                <button class="btn btn-warn" type="button" id="btnResetStats">Reset sales statistics</button>
                <button class="btn" type="button" id="btnReboot">Restart controller</button>
                <button class="btn btn-danger" type="button" id="btnFactoryReset">Factory reset</button>
              </div>
              <div class="hint muted" style="font-size:11.4px">Holding the BOOT button on the board for five seconds also performs a factory reset.</div>
            </div>
          </div>
        </div>

      </div>
    </section>

  </div>
</main>
</div>

<div id="toasts" aria-live="polite" aria-atomic="false"></div>
<div id="modalRoot"></div>
<div class="chart-tip" id="chartTip"></div>
<script>
(function () {
  "use strict";

  const safeStorage = (function () {
    const pick = function (kind) {
      try {
        var target = (kind === "session") ? window.sessionStorage : window.localStorage;
        var probe = "__cw_probe__";
        target.setItem(probe, "1");
        target.removeItem(probe);
        return target;
      } catch (err) {
        return null;
      }
    };
    const local = pick("local");
    const session = pick("session");
    return {
      get: function (key) {
        try { return local ? local.getItem(key) : null; } catch (err) { return null; }
      },
      set: function (key, value) {
        try { if (local) { local.setItem(key, value); } } catch (err) { }
      },
      sessionGet: function (key) {
        try { return session ? session.getItem(key) : null; } catch (err) { return null; }
      },
      sessionSet: function (key, value) {
        try { if (session) { session.setItem(key, value); } } catch (err) { }
      }
    };
  })();




  /* ======================================================
     CONSTANTS AND APPLICATION STATE
     ====================================================== */
  var CH_NAMES = ["WATER", "BLOWER", "SOAP", "FOAM"];
  var CH_COLORS = ["#38bdf8", "#a78bfa", "#34d399", "#fbbf24"];
  var CH_ICONS = [
    "<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\"><path d=\"M12 3c3.4 4 5.7 7 5.7 9.9a5.7 5.7 0 1 1-11.4 0C6.3 10 8.6 7 12 3Z\"></path></svg>",
    "<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\"><path d=\"M3 8h9a3 3 0 0 1 3 3v2H6a3 3 0 0 1-3-3z\"></path><path d=\"M15 10h3l3 3v3h-6z\"></path><circle cx=\"7\" cy=\"17.5\" r=\"1.6\"></circle><circle cx=\"18\" cy=\"17.5\" r=\"1.6\"></circle></svg>",
    "<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\"><circle cx=\"7.5\" cy=\"7.5\" r=\"2.6\"></circle><circle cx=\"16.5\" cy=\"16.5\" r=\"2.6\"></circle><path d=\"M14.4 5.4h5.2v5.2\"></path><path d=\"M4.4 13.4v5.2h5.2\"></path></svg>",
    "<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\"><circle cx=\"9\" cy=\"9\" r=\"4.2\"></circle><circle cx=\"16\" cy=\"14.6\" r=\"3\"></circle><circle cx=\"8\" cy=\"17\" r=\"2.2\"></circle></svg>"
  ];
  var ROUTES = {
    "dashboard": { title: "Dashboard", sub: "Live machine status, credit and all time sales", view: "view-dashboard" },
    "inventory": { title: "Sales Inventory", sub: "Revenue, runtime and session ledger", view: "view-inventory" },
    "settings-general": { title: "General Settings", sub: "Credit model, timing and panel access", view: "view-settings-general" },
    "settings-coin": { title: "Coin & Bill Acceptor", sub: "Pulse input, mapping and credit tools", view: "view-settings-coin" },
    "settings-lcd": { title: "LCD 20x4", sub: "I2C display layout and indicators", view: "view-settings-lcd" },
    "settings-io": { title: "Buttons & Relays", sub: "GPIO assignment and trigger polarity", view: "view-settings-io" },
    "settings-buzzer": { title: "Buzzer", sub: "Audible feedback configuration", view: "view-settings-buzzer" },
    "settings-wifi": { title: "Wi-Fi Access Point", sub: "Network identity and captive portal", view: "view-settings-wifi" },
    "settings-system": { title: "System & Maintenance", sub: "Diagnostics, backup and reset", view: "view-settings-system" }
  };

  var app = {
    route: "dashboard",
    state: null,
    stats: null,
    cfg: null,
    diag: null,
    loadedCfg: false,
    lastStateAt: 0,
    failures: 0,
    pollTimer: null,
    pin: safeStorage.sessionGet("cw_pin") || "",
    busy: false
  };

  /* ======================================================
     UTILITIES
     ====================================================== */
  const $ = function (sel, root) { return (root || document).querySelector(sel); }
  const $$ = function (sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); }
  const pad = function (n) { return (n < 10 ? "0" : "") + n; }
  const pad2 = function (s, n) { s = String(s); while (s.length < n) { s += " "; } return s; }
  const num = function (v, d) { var n = Number(v); return isNaN(n) ? d : n; }
  const downloadText = function (text, mime, filename) {
    try {
      var blob = new Blob([text], { type: mime });
      var url = URL.createObjectURL(blob);
      var a = document.createElement("a");
      a.href = url;
      a.download = filename;
      a.rel = "noopener";
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      setTimeout(function () { URL.revokeObjectURL(url); }, 1500);
      toast("ok", "Export ready", filename);
    } catch (err) {
      toast("err", "Download blocked", "Open the panel in a browser tab to export files.");
    }
  };

  const esc = function (s) { return String(s == null ? "" : s).replace(/[&<>"']/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;" }[c]; }); }

  const fmtClock = function (ms) {
    var s = Math.max(0, Math.round(ms / 1000));
    var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
    if (h > 0) { return h + ":" + pad(m) + ":" + pad(sec); }
    return pad(m) + ":" + pad(sec);
  }
  const fmtMinutes = function (sec) {
    var m = Math.floor(sec / 60);
    if (m >= 60000) { return (m / 60000).toFixed(1) + "k"; }
    return String(m);
  }
  const fmtDuration = function (sec) {
    var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
    if (h > 0) { return h + "h " + pad(m) + "m"; }
    if (m > 0) { return m + "m " + pad(s) + "s"; }
    return s + "s";
  }
  const fmtMoney = function (centavos) {
    var v = (num(centavos, 0) / 100);
    return "₱" + v.toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
  }
  const fmtCompact = function (n) {
    n = num(n, 0);
    if (n >= 1000000) { return (n / 1000000).toFixed(2) + "M"; }
    if (n >= 10000) { return (n / 1000).toFixed(1) + "k"; }
    return String(n);
  }
  const fmtUptime = function (sec) {
    sec = Math.floor(num(sec, 0));
    var d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600), m = Math.floor((sec % 3600) / 60);
    if (d > 0) { return d + "d " + h + "h"; }
    if (h > 0) { return h + "h " + m + "m"; }
    return m + "m " + (sec % 60) + "s";
  }
  const agoText = function (seconds) {
    seconds = Math.max(0, Math.floor(seconds));
    if (seconds < 60) { return seconds + "s ago"; }
    if (seconds < 3600) { return Math.floor(seconds / 60) + "m ago"; }
    if (seconds < 86400) { return Math.floor(seconds / 3600) + "h ago"; }
    return Math.floor(seconds / 86400) + "d ago";
  }
  const stateLabel = function (s) { return s === 1 ? "RUNNING" : s === 2 ? "PAUSED" : "IDLE"; }

  /* ======================================================
     TRANSPORT LAYER
     ====================================================== */
  const encodeBody = function (obj) {
    var lines = [];
    Object.keys(obj || {}).forEach(function (k) {
      if (obj[k] === null || obj[k] === undefined) { return; }
      lines.push(k + "=" + String(obj[k]).replace(/[\r\n]/g, " "));
    });
    return lines.join("\n");
  }

  const transport = function (path, options) {
    
    var opts = Object.assign({ method: "GET", cache: "no-store", headers: {} }, options || {});
    if (opts.body !== undefined) {
      opts.headers["Content-Type"] = "text/plain;charset=UTF-8";
    }
    if (app.pin) { opts.headers["X-Pin"] = app.pin; }
    return fetch(path, opts).then(function (res) {
      return res.text().then(function (text) {
        var data;
        try { data = text ? JSON.parse(text) : {}; }
        catch (err) { data = { ok: 0, error: "bad_response" }; }
        if (!res.ok && data && data.ok !== 0) { data.ok = 0; }
        if (res.status === 401) { data.ok = 0; data.error = data.error || "pin"; }
        return data;
      });
    });
  }

  const apiGet = function (path) { return transport(path, { method: "GET" }); }
  const apiPost = function (path, obj) { return transport(path, { method: "POST", body: encodeBody(obj) }); }

  const withPinRetry = function (promiseFactory) {
    return promiseFactory().then(function (res) {
      if (res && res.ok === 0 && res.error === "pin") {
        return askPin().then(function (pin) {
          if (!pin) { throw new Error("pin_required"); }
          app.pin = pin;
          safeStorage.sessionSet("cw_pin", pin);
          return promiseFactory();
        });
      }
      return res;
    });
  }

  /* ======================================================
     NOTIFICATIONS AND MODALS
     ====================================================== */
  const toast = function (kind, title, message, ms) {
    var host = $("#toasts");
    if (!host) { return; }
    var el = document.createElement("div");
    el.className = "toast " + (kind || "");
    var icon = kind === "err"
      ? "<svg class=\"ico\" width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#f87171\" stroke-width=\"2\" stroke-linecap=\"round\"><circle cx=\"12\" cy=\"12\" r=\"9\"></circle><path d=\"M12 7.5v5.5M12 16.4v.1\"></path></svg>"
      : "<svg class=\"ico\" width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#34d399\" stroke-width=\"2\" stroke-linecap=\"round\"><circle cx=\"12\" cy=\"12\" r=\"9\"></circle><path d=\"M8.5 12.4l2.4 2.4 4.6-5\"></path></svg>";
    el.innerHTML = icon + "<div><strong>" + esc(title) + "</strong>" + (message ? "<p>" + esc(message) + "</p>" : "") + "</div>";
    host.appendChild(el);
    setTimeout(function () {
      el.style.opacity = "0";
      el.style.transform = "translateY(6px)";
      setTimeout(function () { if (el.parentNode) { el.parentNode.removeChild(el); } }, 220);
    }, ms || 3600);
  }

  const modal = function (options) {
    return new Promise(function (resolve) {
      var root = $("#modalRoot");
      var scrim = document.createElement("div");
      scrim.className = "modal-scrim";
      scrim.innerHTML =
        "<div class=\"modal\" role=\"dialog\" aria-modal=\"true\">" +
        "<div class=\"modal-head\"><h3>" + esc(options.title) + "</h3></div>" +
        "<div class=\"modal-body\">" + (options.html || esc(options.body || "")) + "</div>" +
        "<div class=\"modal-foot\">" +
        "<button class=\"btn\" data-modal-cancel type=\"button\">" + esc(options.cancelLabel || "Cancel") + "</button>" +
        "<button class=\"btn " + (options.danger ? "btn-danger" : "btn-primary") + "\" data-modal-ok type=\"button\">" + esc(options.okLabel || "Confirm") + "</button>" +
        "</div></div>";
      root.appendChild(scrim);
      const close = function (result) {
        if (scrim.parentNode) { scrim.parentNode.removeChild(scrim); }
        resolve(result);
      }
      $("[data-modal-cancel]", scrim).addEventListener("click", function () { close(null); });
      $("[data-modal-ok]", scrim).addEventListener("click", function () {
        var input = $("[data-modal-input]", scrim);
        close(input ? input.value : true);
      });
      scrim.addEventListener("click", function (e) { if (e.target === scrim) { close(null); } });
      var focusTarget = $("[data-modal-input]", scrim) || $("[data-modal-ok]", scrim);
      if (focusTarget) { focusTarget.focus(); }
      scrim.addEventListener("keydown", function (e) {
        if (e.key === "Escape") { close(null); }
        if (e.key === "Enter") {
          var input = $("[data-modal-input]", scrim);
          close(input ? input.value : true);
        }
      });
    });
  }

  const confirmAction = function (title, body, okLabel, danger) {
    return modal({ title: title, body: body, okLabel: okLabel, danger: danger }).then(function (r) { return !!r; });
  }

  const askPin = function () {
    return modal({
      title: "Panel PIN required",
      body: "This device is protected. Enter the numeric PIN configured in General settings.",
      html: "<label class=\"field\"><span style=\"font-size:12.2px;font-weight:600;color:var(--text-2)\">PIN</span><input class=\"input\" type=\"password\" inputmode=\"numeric\" data-modal-input maxlength=\"8\" autocomplete=\"off\"></label>",
      okLabel: "Unlock"
    });
  }

  /* ======================================================
     ROUTER
     ====================================================== */
  const currentRouteFromHash = function () {
    var raw = (location.hash || "").replace(/^#\/?/, "");
    if (!raw) { return "dashboard"; }
    return ROUTES[raw] ? raw : "dashboard";
  }

  const navigate = function (route, fromHash) {
    if (!ROUTES[route]) { route = "dashboard"; }
    app.route = route;
    var meta = ROUTES[route];
    $$(".view").forEach(function (v) { v.classList.toggle("is-active", v.id === meta.view); });
    $$(".nav-link").forEach(function (a) { a.classList.toggle("is-active", a.getAttribute("data-route") === route); });
    $("#pageTitle").textContent = meta.title;
    $("#pageSub").textContent = meta.sub;
    document.title = meta.title + " · Carwash 4-in-1";
    if (!fromHash) { location.hash = "#/" + route; }
    closeDrawer();
    try { window.scrollTo(0, 0); } catch (err) { }
    if (route === "inventory") { loadStats(true); }
    if (route.indexOf("settings-") === 0) { loadConfig(true); }
    if (route === "settings-system") { loadDiag(); }
  }

  const closeDrawer = function () {
    var sb = $("#sidebar");
    sb.classList.remove("is-open");
    $("#scrim").classList.remove("is-on");
    $("#menuBtn").setAttribute("aria-expanded", "false");
  }

  const openDrawer = function () {
    $("#sidebar").classList.add("is-open");
    $("#scrim").classList.add("is-on");
    $("#menuBtn").setAttribute("aria-expanded", "true");
  }

  /* ======================================================
     DATA LOADING
     ====================================================== */
  const loadState = function () {
    return apiGet("api/state").then(function (data) {
      if (!data || data.ok !== 1) { throw new Error("state"); }
      app.state = data;
      app.lastStateAt = Date.now();
      app.failures = 0;
      setConnected(true);
      renderState();
      return data;
    }).catch(function (err) {
      app.failures++;
      setConnected(false);
      throw err;
    });
  }

  const loadStats = function (force) {
    if (!force && app.stats && Date.now() - (app.statsAt || 0) < 4000) { return Promise.resolve(); }
    return apiGet("api/stats").then(function (data) {
      if (data && data.ok === 1) {
        app.stats = data;
        app.statsAt = Date.now();
        renderStats();
      }
    }).catch(function () {});
  }

  const loadConfig = function (force) {
    if (!force && app.loadedCfg) { return Promise.resolve(true); }
    var request = function () { return apiGet("api/config"); };
    return (force ? withPinRetry(request) : request()).then(function (data) {
      if (data && typeof data === "object" && data.ok !== 0) {
        app.cfg = data;
        app.loadedCfg = true;
        renderConfig();
        return true;
      }
      return false;
    }).catch(function () { return false; });
  }

  const loadDiag = function () {
    return apiGet("api/diag").then(function (data) {
      if (data && data.ok === 1) {
        app.diag = data;
        renderDiag();
      }
    }).catch(function () {});
  }

  const setConnected = function (ok) {
    var pill = $("#connPill");
    pill.classList.toggle("is-live", ok);
    pill.classList.toggle("is-off", !ok);
    $("#connText").textContent = ok ? "Live" : "Offline";
  }

  /* ======================================================
     POLLING SCHEDULER
     ====================================================== */
  const pollLoop = function () {
    if (document.hidden) {
      app.pollTimer = setTimeout(pollLoop, 1500);
      return;
    }
    loadState().then(function () {
      var delay = 1000;
      if (app.route === "inventory") { loadStats(false); }
      app.pollTimer = setTimeout(pollLoop, delay);
    }).catch(function () {
      var backoff = Math.min(8000, 1000 * Math.pow(2, Math.min(app.failures, 4)));
      app.pollTimer = setTimeout(pollLoop, backoff);
    });
  }

  /* ======================================================
     DASHBOARD RENDERERS
     ====================================================== */
  const renderState = function () {
    var s = app.state;
    if (!s) { return; }
    renderKpis(s);
    renderChannels(s);
    renderLcd(s.lcd || []);
    renderStatusPanel(s);
    $("#footUptime").textContent = fmtUptime(s.uptimeTotal);
    $("#footIp").textContent = "192.168.4.1";
    if (app.route === "settings-coin") { renderCoinMonitor(s); }
  }

  const renderKpis = function (s) {
    var stats = app.stats || {};
    $("#kpiCredit").textContent = fmtClock(s.poolMs);
    var creditMin = Math.floor(s.poolMs / 60000);
    $("#kpiCreditSub").textContent = creditMin > 0 ? creditMin + " minutes available" : "insert a coin to start";

    $("#kpiActive").innerHTML = s.active + "<small>/ 4</small>";
    var runningNames = (s.ch || []).filter(function (c) { return c.state === 1; }).map(function (c) { return c.n; });
    $("#kpiActiveSub").textContent = runningNames.length ? runningNames.join(", ").toLowerCase() : "all functions idle";

    $("#kpiRevenue").textContent = fmtMoney(s.revenueCentavos);
    var coins = num(s.coinGroups, 0);
    $("#kpiRevenueSub").textContent = coins > 0 ? "from " + fmtCompact(coins) + " coins" : "no sales yet";

    $("#kpiCoins").textContent = fmtCompact(coins);
    $("#kpiCoinsSub").textContent = fmtCompact(num(s.coinPulses, 0)) + " pulses";

    var seconds = 0;
    (stats.channelSeconds || []).forEach(function (v) { seconds += num(v, 0); });
    $("#kpiDispensed").innerHTML = fmtCompact(fmtMinutes(seconds)) + "<small>min</small>";
    $("#kpiDispensedSub").textContent = "across all functions";
  }

  const channelCard = function (ch, index) {
    var wrap = document.createElement("article");
    wrap.className = "channel";
    wrap.setAttribute("data-state", String(ch.state));
    wrap.style.setProperty("--ch-color", CH_COLORS[index]);
    var maxMs = 3600000;
    var blinkClass = ch.blink ? " is-blinking" : "";
    wrap.innerHTML =
      "<div class=\"channel-top\">" +
        "<span class=\"channel-icon\">" + CH_ICONS[index] + "</span>" +
        "<div><div class=\"channel-name\">" + esc(ch.n) + "</div><div class=\"channel-pin\">relay " + ch.relay + " · button " + ch.btn + "</div></div>" +
        "<span class=\"status-chip\">" + stateLabel(ch.state) + "</span>" +
      "</div>" +
      "<div class=\"channel-time" + blinkClass + "\">" + fmtClock(ch.remMs) + "<small>" + (ch.state === 1 ? "running" : ch.state === 2 ? "paused" : "ready") + "</small></div>" +
      "<div class=\"bar\"><i style=\"width:" + Math.min(100, Math.max(0, (num(ch.remMs, 0) / maxMs) * 100)) + "%\"></i></div>" +
      "<div class=\"channel-meta\"><span>used " + fmtDuration(Math.floor(num(ch.usedMs, 0) / 1000)) + "</span><span>" + (ch.relayOn ? "relay ON" : "relay off") + "</span></div>" +
      "<div class=\"channel-actions\">" +
        "<button class=\"btn btn-sm " + (ch.state === 1 ? "btn-warn" : "btn-ok") + "\" data-ch-action=\"" + (ch.state === 1 ? "pause" : ch.state === 2 ? "resume" : "start") + "\" data-ch=\"" + index + "\" type=\"button\">" +
          (ch.state === 1 ? "Pause" : ch.state === 2 ? "Resume" : "Start") + "</button>" +
        "<button class=\"btn btn-sm\" data-ch-action=\"add\" data-ch=\"" + index + "\" type=\"button\">+1 min</button>" +
        "<button class=\"btn btn-sm btn-ghost\" data-ch-action=\"stop\" data-ch=\"" + index + "\" type=\"button\">Stop</button>" +
      "</div>";
    return wrap;
  }

  const renderChannels = function (s) {
    var grid = $("#channelGrid");
    if (!grid || !s.ch) { return; }
    var signature = s.ch.map(function (c) { return c.state + ":" + c.relayOn + ":" + c.blink + ":" + Math.floor(num(c.usedMs, 0) / 60000) + ":" + Math.round(num(c.remMs, 0) / 1000); }).join("|");
    if (grid.getAttribute("data-sig") === signature) { return; }
    grid.setAttribute("data-sig", signature);
    grid.innerHTML = "";
    s.ch.forEach(function (ch, i) { grid.appendChild(channelCard(ch, i)); });
  }

  const renderLcd = function (lines) {
    var text = lines.map(function (l) { return pad2(String(l || "").replace(/\s+$/, ""), 20); }).join("\n");
    ["#lcdScreen", "#lcdScreenBig"].forEach(function (sel) {
      var el = $(sel);
      if (el) { el.textContent = text; }
    });
    var cfgState = (app.state && app.state.cfg) || {};
    var addr = "$" + ["0x27", "0x3F", "0x20", "0x38"][0];
    var addrNum = num(cfgState.lcdAddr, 39);
    addr = "0x" + addrNum.toString(16).toUpperCase();
    var label = addr + " · " + (cfgState.lcdCols || 20) + "x" + (cfgState.lcdRows || 4);
    ["#lcdAddrLabel", "#lcdAddrLabel2"].forEach(function (sel) { var el = $(sel); if (el) { el.textContent = addr; } });
    ["#lcdBusLabel", "#lcdBusLabel2"].forEach(function (sel) {
      var el = $(sel);
      if (el) { el.textContent = "SDA " + num(app.cfg && app.cfg.lcdSda, 21) + " / SCL " + num(app.cfg && app.cfg.lcdScl, 22); }
    });
    var stateLabelEl = $("#lcdStateLabel");
    if (stateLabelEl) { stateLabelEl.textContent = app.state && app.state.lcdPresent ? "connected" : "not detected"; }
    var mirror = $("#lcdMirrorState");
    if (mirror) { mirror.textContent = app.state && app.state.lcdPresent ? "live" : "not detected"; }
  }

  const renderStatusPanel = function (s) {
    $("#statWifi").textContent = s.apRunning === 1 ? "on" : s.apRunning === 2 ? "dns off" : "off";
    $("#statClients").textContent = num(s.apStations, 0);
    $("#statHeap").textContent = Math.round(num(s.freeHeap, 0) / 1024) + " kB";
    $("#statLcd").textContent = s.lcdPresent ? "ready" : "missing";
    $("#statusUpdated").textContent = "updated " + new Date().toLocaleTimeString();
    $("#wifiSsidLive").textContent = s.apSsid || "--";
    $("#wifiClients").textContent = num(s.apStations, 0);
    $("#wifiRestarts").textContent = num(s.apRestarts, 0);
    $("#wifiStateLabel").textContent = s.apRunning === 1 ? "broadcasting" : "restarting";
    $("#coinMonitorState").textContent = s.active ? "dispensing" : "idle";
    $("#monPulses").textContent = fmtCompact(num(s.coinPulses, 0));
    $("#monCoins").textContent = fmtCompact(num(s.coinGroups, 0));
    $("#monLast").textContent = num(s.lastCoinAgo, -1) >= 0 ? agoText(num(s.lastCoinAgo, 0)) : "--";
  }

  const renderCoinMonitor = function (s) {
    $("#monPulses").textContent = fmtCompact(num(s.coinPulses, 0));
    $("#monCoins").textContent = fmtCompact(num(s.coinGroups, 0));
  }

  /* ======================================================
     STATISTICS RENDERERS
     ====================================================== */
  const renderStats = function () {
    var st = app.stats;
    if (!st) { return; }
    var totalSeconds = 0;
    (st.channelSeconds || []).forEach(function (v) { totalSeconds += num(v, 0); });
    var totalSessions = 0;
    (st.channelSessions || []).forEach(function (v) { totalSessions += num(v, 0); });

    $("#invRevenue").textContent = fmtMoney(st.revenueCentavos);
    $("#invCoins").textContent = fmtCompact(num(st.coins, 0));
    $("#invPulsesSub").textContent = fmtCompact(num(st.pulses, 0)) + " pulses";
    $("#invSessions").textContent = fmtCompact(totalSessions);
    $("#invBonus").innerHTML = fmtCompact(Math.floor(num(st.bonusSeconds, 0) / 60)) + "<small>min</small>";
    $("#invAvg").textContent = num(st.coins, 0) > 0 ? fmtMoney(num(st.revenueCentavos, 0) / num(st.coins, 1)) : fmtMoney(0);

    if (app.state) {
      $("#kpiRevenue").textContent = fmtMoney(st.revenueCentavos);
      $("#kpiCoins").textContent = fmtCompact(num(st.coins, 0));
      $("#kpiDispensed").innerHTML = fmtCompact(fmtMinutes(totalSeconds)) + "<small>min</small>";
    }

    renderInventoryTable(st, totalSeconds, totalSessions);
    renderLedger(st);
    renderRecent(st);
    renderSalesChart(st);
    renderDonut(st, totalSeconds);
    renderCoinMapTable();
  }

  const renderInventoryTable = function (st, totalSeconds, totalSessions) {
    var body = $("#inventoryBody");
    if (!body) { return; }
    var rows = "";
    for (var i = 0; i < 4; i++) {
      var sec = num((st.channelSeconds || [])[i], 0);
      var runs = num((st.channelSessions || [])[i], 0);
      var share = totalSeconds > 0 ? (sec / totalSeconds) * 100 : 0;
      var avg = runs > 0 ? Math.round(sec / runs) : 0;
      rows += "<tr>" +
        "<td><span class=\"tag\" style=\"background:" + CH_COLORS[i] + "22;color:" + CH_COLORS[i] + "\">" + CH_NAMES[i] + "</span></td>" +
        "<td class=\"num\">" + fmtDuration(sec) + "</td>" +
        "<td class=\"num\">" + fmtCompact(runs) + "</td>" +
        "<td class=\"num\">" + fmtDuration(avg) + "</td>" +
        "<td class=\"num\">" + share.toFixed(1) + "%</td>" +
        "<td class=\"num\">" + ((app.state && app.state.ch && app.state.ch[i]) ? app.state.ch[i].relay : "-") + "</td>" +
        "<td class=\"num\">" + ((app.state && app.state.ch && app.state.ch[i]) ? app.state.ch[i].btn : "-") + "</td>" +
        "</tr>";
    }
    body.innerHTML = rows || "<tr><td colspan=\"7\" class=\"empty\">No runtime recorded</td></tr>";
  }

  const renderLedger = function (st) {
    var body = $("#ledgerBody");
    if (!body) { return; }
    var days = (st.days || []).slice().reverse();
    if (!days.length) {
      body.innerHTML = "<tr><td colspan=\"4\" class=\"empty\">No ledger data yet</td></tr>";
      return;
    }
    body.innerHTML = days.map(function (d) {
      return "<tr><td>Day " + d.d + "</td><td class=\"num\">" + fmtMoney(d.rev) + "</td><td class=\"num\">" + d.coins + "</td><td class=\"num\">" + fmtMinutes(d.sec) + "</td></tr>";
    }).join("");
  }

  const renderRecent = function (st) {
    var body = $("#recentBody");
    if (!body) { return; }
    var sessions = st.sessions || [];
    if (!sessions.length) {
      body.innerHTML = "<tr><td colspan=\"4\" class=\"empty\">No sessions recorded yet</td></tr>";
      return;
    }
    var reasons = { 1: "completed", 2: "safety limit", 3: "credit exhausted", 4: "stopped", 5: "mode change" };
    body.innerHTML = sessions.slice(0, 8).map(function (s) {
      var name = CH_NAMES[s.ch] || "?";
      var color = CH_COLORS[s.ch] || "#888";
      var endedAgo = Math.max(0, num(st.uptimeSeconds, 0) - num(s.start, 0));
      return "<tr>" +
        "<td><span class=\"tag\" style=\"background:" + color + "22;color:" + color + "\">" + name + "</span></td>" +
        "<td class=\"num\">" + fmtDuration(s.dur) + "</td>" +
        "<td class=\"num\">" + agoText(endedAgo) + "</td>" +
        "<td>" + (reasons[s.reason] || "unknown") + "</td>" +
        "</tr>";
    }).join("");
  }

  const renderSalesChart = function (st) {
    var svg = $("#salesChart");
    if (!svg) { return; }
    var days = (st.days || []).slice(-14);
    var w = 720, h = 240, padL = 46, padR = 14, padT = 16, padB = 30;
    var innerW = w - padL - padR, innerH = h - padT - padB;
    if (!days.length) {
      svg.innerHTML = "<text x=\"" + (w / 2) + "\" y=\"" + (h / 2) + "\" text-anchor=\"middle\" style=\"font-size:12px\">No sales data recorded yet</text>";
      $("#chartRangeLabel").textContent = "no operating days recorded";
      return;
    }
    var maxRev = Math.max.apply(null, days.map(function (d) { return num(d.rev, 0); }).concat([100]));
    var maxMin = Math.max.apply(null, days.map(function (d) { return num(d.sec, 0) / 60; }).concat([10]));
    var slot = innerW / days.length;
    var barW = Math.max(6, Math.min(34, slot * 0.52));
    var parts = [];
    for (var g = 0; g <= 4; g++) {
      var y = padT + (innerH / 4) * g;
      parts.push("<line class=\"grid\" x1=\"" + padL + "\" y1=\"" + y.toFixed(1) + "\" x2=\"" + (w - padR) + "\" y2=\"" + y.toFixed(1) + "\"></line>");
      var val = maxRev * (1 - g / 4);
      parts.push("<text x=\"" + (padL - 8) + "\" y=\"" + (y + 3.5).toFixed(1) + "\" text-anchor=\"end\">" + Math.round(val / 100) + "</text>");
    }
    var linePts = [];
    days.forEach(function (d, i) {
      var cx = padL + slot * i + slot / 2;
      var revH = maxRev > 0 ? (num(d.rev, 0) / maxRev) * innerH : 0;
      var by = padT + innerH - revH;
      var isPeak = num(d.rev, 0) === maxRev;
      parts.push("<rect class=\"chart-bar" + (isPeak ? " chart-bar-peak" : "") + "\" x=\"" + (cx - barW / 2).toFixed(1) + "\" y=\"" + by.toFixed(1) + "\" width=\"" + barW.toFixed(1) + "\" height=\"" + Math.max(1, revH).toFixed(1) + "\" rx=\"3\" data-day=\"" + d.d + "\" data-rev=\"" + num(d.rev, 0) + "\" data-sec=\"" + num(d.sec, 0) + "\" data-coins=\"" + num(d.coins, 0) + "\"></rect>");
      var minutes = num(d.sec, 0) / 60;
      var my = padT + innerH - (maxMin > 0 ? (minutes / maxMin) * innerH : 0);
      linePts.push(cx.toFixed(1) + "," + my.toFixed(1));
      if (days.length <= 10 || i % Math.ceil(days.length / 8) === 0) {
        parts.push("<text x=\"" + cx.toFixed(1) + "\" y=\"" + (h - 10) + "\" text-anchor=\"middle\">" + d.d + "</text>");
      }
    });
    if (linePts.length > 1) {
      parts.push("<polyline points=\"" + linePts.join(" ") + "\" fill=\"none\" stroke=\"#34d399\" stroke-width=\"2\" stroke-linejoin=\"round\" stroke-linecap=\"round\" opacity=\"0.95\"></polyline>");
      linePts.forEach(function (p) {
        var xy = p.split(",");
        parts.push("<circle cx=\"" + xy[0] + "\" cy=\"" + xy[1] + "\" r=\"2.6\" fill=\"#34d399\"></circle>");
      });
    }
    svg.innerHTML = parts.join("");
    $("#chartRangeLabel").textContent = "day " + days[0].d + " to day " + days[days.length - 1].d + " · " + days.length + " days";
    attachChartTooltip(svg);
  }

  const attachChartTooltip = function (svg) {
    var tip = $("#chartTip");
    $$("rect.chart-bar", svg).forEach(function (rect) {
      rect.addEventListener("mousemove", function (e) {
        var rev = num(rect.getAttribute("data-rev"), 0);
        var sec = num(rect.getAttribute("data-sec"), 0);
        var coins = num(rect.getAttribute("data-coins"), 0);
        tip.textContent = "day " + rect.getAttribute("data-day") + " · " + fmtMoney(rev) + " · " + Math.floor(sec / 60) + " min · " + coins + " coins";
        tip.style.left = (e.clientX + 14) + "px";
        tip.style.top = (e.clientY - 12) + "px";
        tip.classList.add("is-on");
      });
      rect.addEventListener("mouseleave", function () { tip.classList.remove("is-on"); });
    });
  }

  const renderDonut = function (st, totalSeconds) {
    var svg = $("#donutChart");
    var legend = $("#donutLegend");
    if (!svg || !legend) { return; }
    var values = (st.channelSeconds || []).map(function (v) { return num(v, 0); });
    var total = values.reduce(function (a, b) { return a + b; }, 0);
    var radius = 60, circumference = 2 * Math.PI * radius, offset = 0;
    var parts = ["<circle cx=\"75\" cy=\"75\" r=\"" + radius + "\" fill=\"none\" stroke=\"var(--surface-3)\" stroke-width=\"18\"></circle>"];
    for (var i = 0; i < 4; i++) {
      var frac = total > 0 ? values[i] / total : 0;
      var dash = frac * circumference;
      parts.push("<circle cx=\"75\" cy=\"75\" r=\"" + radius + "\" fill=\"none\" stroke=\"" + CH_COLORS[i] + "\" stroke-width=\"18\" stroke-dasharray=\"" + dash.toFixed(2) + " " + (circumference - dash).toFixed(2) + "\" stroke-dashoffset=\"" + (-offset).toFixed(2) + "\" transform=\"rotate(-90 75 75)\" stroke-linecap=\"butt\"></circle>");
      offset += dash;
    }
    parts.push("<text x=\"75\" y=\"71\" text-anchor=\"middle\" style=\"font-size:11px\">total</text>");
    parts.push("<text x=\"75\" y=\"87\" text-anchor=\"middle\" style=\"font-size:13px;fill:var(--text)\">" + fmtMinutes(totalSeconds) + "m</text>");
    svg.innerHTML = parts.join("");
    legend.innerHTML = [0, 1, 2, 3].map(function (i) {
      var pct = total > 0 ? ((values[i] / total) * 100).toFixed(1) : "0.0";
      return "<div class=\"row\"><i style=\"background:" + CH_COLORS[i] + "\"></i><span>" + CH_NAMES[i] + "</span><span>" + pct + "%</span></div>";
    }).join("");
  }

  const renderCoinMapTable = function () {
    var body = $("#coinMapBody");
    if (!body || !app.cfg) { return; }
    var count = num(app.cfg.coinMapCount, 4);
    var rows = "";
    for (var i = 0; i < count; i++) {
      var pulses = num(app.cfg["coinPulses" + i], 0);
      var minutes = num(app.cfg["coinMinutes" + i], 0);
      var value = num(app.cfg["coinValue" + i], 0);
      var perMin = minutes > 0 ? (value / minutes) : 0;
      rows += "<tr><td class=\"num\">" + pulses + "</td><td class=\"num\">" + (minutes >= 60 ? Math.floor(minutes / 60) + "h " + (minutes % 60) + "m" : minutes + "m") + "</td><td class=\"num\">" + fmtMoney(value) + "</td><td class=\"num\">" + fmtMoney(perMin) + "</td></tr>";
    }
    body.innerHTML = rows || "<tr><td colspan=\"4\" class=\"empty\">No presets configured</td></tr>";
  }

  /* ======================================================
     CONFIGURATION FORM BINDING
     ====================================================== */
  const fieldValue = function (cfg, el) {
    var key = el.getAttribute("data-field");
    if (el.getAttribute("data-type") === "bool") { return num(cfg[key], 0) === 1; }
    return cfg[key];
  }

  const renderConfig = function () {
    var cfg = app.cfg;
    if (!cfg) { return; }
    $$("[data-field]").forEach(function (el) {
      var key = el.getAttribute("data-field");
      if (!(key in cfg)) { return; }
      if (el.getAttribute("data-type") === "bool") {
        el.checked = num(cfg[key], 0) === 1;
      } else if (el.tagName === "SELECT") {
        var val = String(cfg[key]);
        if (!Array.prototype.some.call(el.options, function (o) { return o.value === val; })) {
          var opt = document.createElement("option");
          opt.value = val;
          opt.textContent = val;
          el.appendChild(opt);
        }
        el.value = val;
      } else {
        el.value = cfg[key];
      }
    });
    renderCoinPresets();
    renderPinMap();
    renderCoinMapTable();
    $$("[data-revision]").forEach(function (el) { el.textContent = "config revision " + num(cfg.cfgRevision, 0); });
    renderLcd(app.state ? app.state.lcd : []);
  }

  const renderCoinPresets = function () {
    var body = $("#coinPresetBody");
    if (!body || !app.cfg) { return; }
    var cfg = app.cfg;
    var count = num(cfg.coinMapCount, 4);
    var html = "";
    for (var i = 0; i < 8; i++) {
      var active = i < count;
      var minP = active ? 1 : 0;
      var minM = active ? 1 : 0;
      html += "<tr" + (active ? "" : " style=\"opacity:.42\"") + ">" +
        "<td><span class=\"tag\">preset " + (i + 1) + "</span></td>" +
        "<td><input class=\"input\" type=\"number\" min=\"" + minP + "\" max=\"200\" style=\"max-width:96px\" data-field=\"coinPulses" + i + "\" aria-label=\"Pulses preset " + (i + 1) + "\"></td>" +
        "<td><input class=\"input\" type=\"number\" min=\"" + minM + "\" max=\"1000\" style=\"max-width:96px\" data-field=\"coinMinutes" + i + "\" aria-label=\"Minutes preset " + (i + 1) + "\"></td>" +
        "<td><input class=\"input\" type=\"number\" min=\"0\" max=\"1000000\" step=\"50\" style=\"max-width:120px\" data-field=\"coinValue" + i + "\" aria-label=\"Value preset " + (i + 1) + "\"></td>" +
        "<td class=\"num muted\" id=\"coinResult" + i + "\">--</td>" +
        "</tr>";
    }
    body.innerHTML = html;
    for (var j = 0; j < 8; ++j) {
      var pulsesNode = $("[data-field=\"coinPulses" + j + "\"]", body);
      var minutesNode = $("[data-field=\"coinMinutes" + j + "\"]", body);
      var valueNode = $("[data-field=\"coinValue" + j + "\"]", body);
      var resultNode = $("#coinResult" + j, body);
      var pulses = num(cfg["coinPulses" + j], 0);
      var minutes = num(cfg["coinMinutes" + j], 0);
      if (pulsesNode) { pulsesNode.value = pulses; }
      if (minutesNode) { minutesNode.value = minutes; }
      if (valueNode) { valueNode.value = num(cfg["coinValue" + j], 0); }
      if (resultNode) {
        resultNode.textContent = pulses > 0 ? (pulses + " pulses = " + minutes + " min · " + fmtMoney(num(cfg["coinValue" + j], 0))) : "inactive";
      }
    }
  };

  const renderPinMap = function () {
    var host = $("#pinMap");
    if (!host || !app.cfg) { return; }
    var cfg = app.cfg;
    var items = [
      ["Water relay", cfg.relayPin0], ["Blower relay", cfg.relayPin1], ["Soap relay", cfg.relayPin2], ["Foam relay", cfg.relayPin3],
      ["Water button", cfg.btnPin0], ["Blower button", cfg.btnPin1], ["Soap button", cfg.btnPin2], ["Foam button", cfg.btnPin3],
      ["Coin pulse", cfg.coinPin], ["Pulse output", cfg.pulseOutPin], ["Buzzer", cfg.buzzerPin],
      ["LCD SDA", cfg.lcdSda], ["LCD SCL", cfg.lcdScl], ["BOOT button", cfg.bootPin]
    ];
    host.innerHTML = items.map(function (it) {
      return "<div class=\"pin-item\"><b>" + esc(String(it[1])) + "</b><span>" + esc(it[0]) + "</span></div>";
    }).join("");
  }

  const collectFields = function (scope) {
    var out = {};
    $$("[data-field]", scope).forEach(function (el) {
      var key = el.getAttribute("data-field");
      if (el.getAttribute("data-type") === "bool") {
        out[key] = el.checked ? 1 : 0;
      } else if (el.type === "number") {
        out[key] = el.value === "" ? 0 : Number(el.value);
      } else {
        out[key] = el.value;
      }
    });
    return out;
  }

  const validateScope = function (scope) {
    var ok = true;
    var pins = {};
    $$("[data-field]", scope).forEach(function (el) { el.classList.remove("is-invalid"); });
    $$("[data-pin]", scope).forEach(function (el) {
      var v = Number(el.value);
      if (isNaN(v) || v < 0 || v > 39) {
        el.classList.add("is-invalid");
        ok = false;
        return;
      }
      if (pins[v]) {
        pins[v].classList.add("is-invalid");
        el.classList.add("is-invalid");
        ok = false;
      } else {
        pins[v] = el;
      }
    });
    $$("input[type=number]", scope).forEach(function (el) {
      var min = el.getAttribute("min"), max = el.getAttribute("max");
      if (el.value === "") { return; }
      var v = Number(el.value);
      if ((min !== null && v < Number(min)) || (max !== null && v > Number(max))) {
        el.classList.add("is-invalid");
        ok = false;
      }
    });
    return ok;
  }

  const waitForRevision = function (previous, attempts) {
    attempts = attempts || 0;
    return apiGet("api/state").then(function (s) {
      if (s && s.ok === 1 && num(s.cfgRevision, -1) !== previous) {
        app.state = s;
        renderState();
        return true;
      }
      if (attempts > 22) { return false; }
      return new Promise(function (resolve) { setTimeout(resolve, 180); }).then(function () {
        return waitForRevision(previous, attempts + 1);
      });
    }).catch(function () { return false; });
  }

  const submitSettings = function (form) {
    if (app.busy) { return; }
    if (!validateScope(form)) {
      toast("err", "Check the highlighted fields", "Values must be within the allowed range and GPIO pins must be unique.");
      return;
    }
    var payload = collectFields(form);
    if (!("bonusEnabled" in payload)) { payload.bonusEnabled = num(app.cfg && app.cfg.bonusEnabled, 1); }
    if (!payload.bonusEnabled && num(payload.longPressAddSeconds, 0) === 0 && "longPressAddSeconds" in payload) {
      toast("err", "Long press needs a value", "Bonus is disabled so the long press add time must be at least one second.");
      return;
    }
    var previous = num(app.state && app.state.cfgRevision, 0);
    app.busy = true;
    withPinRetry(function () { return apiPost("api/config", payload); }).then(function (res) {
      if (!res || res.ok !== 1) {
        toast("err", "Configuration rejected", res && res.error ? String(res.error) : "The controller refused these values.");
        return null;
      }
      
      return waitForRevision(num(res.cfgRevision, previous)).then(function (changed) {
        if (changed) {
          toast("ok", "Settings saved", "Applied and stored in flash.");
          loadConfig(true);
        } else {
          toast("err", "Save timed out", "The controller did not confirm the new configuration.");
        }
      });
    }).catch(function (err) {
      toast("err", "Save failed", err && err.message === "pin_required" ? "PIN required." : "Network error, please retry.");
    }).then(function () {
      app.busy = false;
    });
  }

  /* ======================================================
     EVENT WIRING
     ====================================================== */
  const channelAction = function (index, action) {
    var payload = { ch: index, action: action };
    if (action === "add") { payload.seconds = 60; }
    withPinRetry(function () { return apiPost("api/channel", payload); }).then(function (res) {
      if (!res || res.ok !== 1) {
        toast("err", "Command rejected", res && res.error ? String(res.error) : "Try again.");
        return;
      }
      loadState().catch(function () {});
    }).catch(function () { toast("err", "Network error", "Command not sent."); });
  }

  const bindEvents = function () {
    $("#menuBtn").addEventListener("click", function () {
      if ($("#sidebar").classList.contains("is-open")) { closeDrawer(); } else { openDrawer(); }
    });
    $("#scrim").addEventListener("click", closeDrawer);

    $$(".nav-link").forEach(function (link) {
      link.addEventListener("click", function (e) {
        e.preventDefault();
        navigate(link.getAttribute("data-route"));
      });
    });

    window.addEventListener("hashchange", function () { navigate(currentRouteFromHash(), true); });

    $("#themeBtn").addEventListener("click", function () {
      var current = document.documentElement.getAttribute("data-theme");
      var next = current === "dark" ? "light" : "dark";
      document.documentElement.setAttribute("data-theme", next);
      safeStorage.set("cw_theme", next);
      $("#themeBtn").setAttribute("aria-label", next === "dark" ? "Switch to light theme" : "Switch to dark theme");
    });

    document.addEventListener("click", function (e) {
      var target = e.target.closest ? e.target.closest("[data-ch-action]") : null;
      if (target) {
        channelAction(num(target.getAttribute("data-ch"), 0), target.getAttribute("data-ch-action"));
        return;
      }
      var sim = e.target.closest ? e.target.closest("[data-sim-coin]") : null;
      if (sim) {
        var pulses = num(sim.getAttribute("data-sim-coin"), 1);
        withPinRetry(function () { return apiPost("api/coin", { pulses: pulses }); }).then(function (res) {
          if (res && res.ok === 1) { toast("ok", "Pulse burst injected", pulses + " pulses sent to the acceptor logic."); loadState().catch(function () {}); }
          else { toast("err", "Rejected", res && res.error ? String(res.error) : "Try again."); }
        }).catch(function () {});
        return;
      }
      var buzz = e.target.closest ? e.target.closest("[data-buzz]") : null;
      if (buzz) {
        withPinRetry(function () { return apiPost("api/buzzer", { pattern: num(buzz.getAttribute("data-buzz"), 1) }); }).then(function (res) {
          if (res && res.ok === 1) { toast("ok", "Pattern queued", "Listen to the machine."); }
          else { toast("err", "Rejected", res && res.error ? String(res.error) : "Try again."); }
        }).catch(function () { toast("err", "Command failed", "PIN or network error."); });
        return;
      }
      var reload = e.target.closest ? e.target.closest("[data-reload]") : null;
      if (reload) {
        loadConfig(true).then(function (loaded) {
          if (loaded) { toast("ok", "Reloaded", "Values restored from the controller."); }
          else { toast("err", "Reload failed", "Configuration was not loaded."); }
        });
      }
    });

    $$("form[data-settings-form]").forEach(function (form) {
      form.addEventListener("submit", function (e) {
        e.preventDefault();
        submitSettings(form);
      });
    });

    $("#btnStartAll").addEventListener("click", function () {
      [0, 1, 2, 3].forEach(function (i) { channelAction(i, "start"); });
    });
    $("#btnStopAll").addEventListener("click", function () {
      [0, 1, 2, 3].forEach(function (i) { channelAction(i, "stop"); });
    });

    $("#btnAddCredit").addEventListener("click", function () {
      var seconds = num($("#creditSeconds").value, 0);
      if (seconds < 1) { toast("err", "Invalid amount", "Enter at least one second."); return; }
      withPinRetry(function () { return apiPost("api/credit", { seconds: seconds }); }).then(function (res) {
        if (res && res.ok === 1) { toast("ok", "Credit command queued", "The controller will process " + seconds + " seconds."); loadState().catch(function () {}); }
        else { toast("err", "Rejected", res && res.error ? String(res.error) : "Try again."); }
      }).catch(function () {});
    });

    $("#btnRefreshDiag").addEventListener("click", loadDiag);

    $("#btnSaveStats").addEventListener("click", function () {
      withPinRetry(function () { return apiPost("api/stats/save", {}); }).then(function (res) {
        if (res && res.ok === 1) { toast("ok", "Statistics save queued", "The controller will attempt to write to flash."); }
      }).catch(function () {});
    });

    $("#btnClearSessions").addEventListener("click", function () {
      confirmAction("Clear session journal", "The recent session list will be erased. Sales totals are kept.", "Clear journal").then(function (yes) {
        if (!yes) { return; }
        withPinRetry(function () { return apiPost("api/sessions/clear", { confirm: "CLEAR" }); }).then(function (res) {
          if (res && res.ok === 1) { toast("ok", "Journal clear queued", ""); loadStats(true); }
          else { toast("err", "Rejected", "Confirmation failed."); }
        }).catch(function () {});
      });
    });

    $("#btnResetStats").addEventListener("click", function () {
      confirmAction("Reset sales statistics", "All time revenue, coins, runtime and the session journal will be reset to zero. Configuration is kept.", "Reset statistics", true).then(function (yes) {
        if (!yes) { return; }
        withPinRetry(function () { return apiPost("api/stats/reset", { confirm: "RESET" }); }).then(function (res) {
          if (res && res.ok === 1) { toast("ok", "Statistics reset queued", "The controller will process the request."); loadStats(true); setTimeout(function () { loadState().catch(function () {}); }, 400); }
          else { toast("err", "Rejected", "Confirmation failed."); }
        }).catch(function () {});
      });
    });

    $("#btnReboot").addEventListener("click", function () {
      confirmAction("Restart controller", "The machine stops all functions and restarts. Configuration is preserved, but remaining credit is lost on restart.", "Restart").then(function (yes) {
        if (!yes) { return; }
        withPinRetry(function () { return apiPost("api/reboot", { confirm: "REBOOT" }); }).then(function (res) {
          if (!res || res.ok !== 1) { toast("err", "Rejected", res && res.error ? String(res.error) : "Restart not allowed."); return; }
          toast("ok", "Restarting", "The panel reconnects in a few seconds.");
          app.cfg = null;
          app.loadedCfg = false;
        }).catch(function () { toast("err", "Rejected", "Restart not allowed."); });
      });
    });

    $("#btnFactoryReset").addEventListener("click", function () {
      confirmAction("Factory reset", "This erases configuration, sales statistics, the session journal and the Wi-Fi credentials. The device restarts with default settings.", "Erase and restart", true).then(function (yes) {
        if (!yes) { return; }
        withPinRetry(function () { return apiPost("api/factory-reset", { confirm: "FACTORY" }); }).then(function (res) {
          if (!res || res.ok !== 1) { toast("err", "Rejected", res && res.error ? String(res.error) : "Factory reset not allowed."); return; }
          toast("ok", "Factory reset", "Reconnect to ESP32_4in1_Carwash after the restart.");
        }).catch(function () { toast("err", "Rejected", "Factory reset not allowed."); });
      });
    });

    $("#btnExportConfig").addEventListener("click", function () {
      withPinRetry(function () { return apiGet("api/config"); }).then(function (cfg) {
        if (!cfg || cfg.ok === 0) { toast("err", "Export failed", "Access denied."); return; }
        var lines = ["# carwash-4in1 configuration", "# firmware " + (app.state ? app.state.fw : "")];
        Object.keys(cfg).forEach(function (k) {
          if (k === "cfgRevision") { return; }
          lines.push(k + "=" + cfg[k]);
        });
        downloadText(lines.join("\n"), "text/plain", "carwash-config.txt");
      }).catch(function () { toast("err", "Export failed", "Could not read configuration."); });
    });

    $("#importFile").addEventListener("change", function (e) {
      var file = e.target.files && e.target.files[0];
      if (!file) { return; }
      var reader = new FileReader();
      reader.onload = function () {
        var text = String(reader.result || "");
        var payload = {};
        text.split(/\r?\n/).forEach(function (line) {
          if (!line || line.charAt(0) === "#") { return; }
          var i = line.indexOf("=");
          if (i > 0) { payload[line.slice(0, i).trim()] = line.slice(i + 1).trim(); }
        });
        if (!Object.keys(payload).length) { toast("err", "Nothing to restore", "The file contains no settings."); return; }
        confirmAction("Restore configuration", "Apply " + Object.keys(payload).length + " settings from the file and store them in flash.", "Restore").then(function (yes) {
          if (!yes) { return; }
          withPinRetry(function () { return apiPost("api/restore", payload); }).then(function (res) {
            if (res && res.ok === 1) {
              var previous = num(res.cfgRevision, num(app.state && app.state.cfgRevision, 0));
              return waitForRevision(previous).then(function (changed) {
                if (changed) {
                  toast("ok", "Configuration restored", "Reconnect if the network name changed.");
                  loadConfig(true);
                } else {
                  toast("err", "Restore timed out", "The controller did not confirm the configuration.");
                }
              });
            }
            toast("err", "Restore rejected", res && res.error ? String(res.error) : "Invalid values in the file.");
          }).catch(function () {});
        });
      };
      reader.readAsText(file);
      e.target.value = "";
    });

    $("#btnExportInventory").addEventListener("click", function () {
      var st = app.stats;
      if (!st) { toast("err", "No data", "Statistics are not loaded yet."); return; }
      var rows = ["day,revenue,coins,minutes"];
      (st.days || []).forEach(function (d) { rows.push([d.d, (num(d.rev, 0) / 100).toFixed(2), d.coins, Math.round(num(d.sec, 0) / 60)].join(",")); });
      rows.push("");
      rows.push("function,runtime_seconds,sessions");
      for (var i = 0; i < 4; i++) {
        rows.push([CH_NAMES[i], num((st.channelSeconds || [])[i], 0), num((st.channelSessions || [])[i], 0)].join(","));
      }
      downloadText(rows.join("\n"), "text/csv", "carwash-inventory.csv");
    });

    document.addEventListener("visibilitychange", function () {
      if (!document.hidden) { loadState().catch(function () {}); }
    });

    window.addEventListener("online", function () { loadState().catch(function () {}); });
  }

  /* ======================================================
     DIAGNOSTICS RENDERER
     ====================================================== */
  const renderDiag = function () {
    var d = app.diag;
    var body = $("#diagBody");
    if (!d || !body) { return; }
    const row = function (label, value) {
      return "<div class=\"diag-row\"><span>" + esc(label) + "</span><b>" + esc(String(value)) + "</b></div>";
    };
    var pins = d.pins || {};
    body.innerHTML =
      row("Firmware", d.firmware + " " + d.version) +
      row("Build", d.build) +
      row("Reset reason", d.resetReason) +
      row("Uptime", fmtUptime(d.uptimeSeconds)) +
      row("Boot count", d.boots) +
      row("Free heap", Math.round(num(d.freeHeap, 0) / 1024) + " kB") +
      row("Largest block", Math.round(num(d.largestBlock, 0) / 1024) + " kB") +
      row("Minimum free heap", Math.round(num(d.minFreeHeap, 0) / 1024) + " kB") +
      row("Flash size", Math.round(num(d.flashSize, 0) / 1048576) + " MB") +
      row("CPU", num(d.cpuFreq, 0) + " MHz") +
      row("NVS writes", d.nvsWrites) +
      row("Config sequence", d.configSeq) +
      row("Active config slot", d.activeSlot === 0 ? "A" : "B") +
      row("LCD", d.lcdPresent ? "detected at 0x" + num(d.i2cAddress, 0).toString(16).toUpperCase() : "not detected") +
      row("AP restarts", d.apRestarts) +
      row("Wi-Fi failures", d.wifiFails) +
      row("Dropped coin pulses", d.coinDropped) +
      row("Command queue", d.cmdQueueDepth) +
      row("Relays", "water " + pins.waterRelay + " · blower " + pins.blowerRelay + " · soap " + pins.soapRelay + " · foam " + pins.foamRelay) +
      row("Buttons", "water " + pins.waterButton + " · blower " + pins.blowerButton + " · soap " + pins.soapButton + " · foam " + pins.foamButton) +
      row("Acceptor", "coin " + pins.coin + " · pulse out " + pins.pulseOut + " · buzzer " + pins.buzzer) +
      row("I2C", "SDA " + pins.lcdSda + " · SCL " + pins.lcdScl);
    $("#nvsWrites").textContent = d.nvsWrites;
  };

  /* ======================================================
     BOOTSTRAP
     ====================================================== */
  const init = function () {
    var theme = safeStorage.get("cw_theme");
    if (theme === "light" || theme === "dark") {
      document.documentElement.setAttribute("data-theme", theme);
    }
    bindEvents();
    navigate(currentRouteFromHash(), true);
    loadState().catch(function () {});
    loadStats(true);
    loadConfig(false);
    pollLoop();
    setInterval(function () {
      if (app.route === "settings-system" && !document.hidden) { loadDiag(); }
      if (!document.hidden) { loadStats(false); }
    }, 5000);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
</script>
</body>
</html>
)RAWHTML";

// ======================================================
// HTTP ROUTE TABLE
// ======================================================

static void handleRootDocument() {
  g_server.sendHeader("Cache-Control", "no-cache, must-revalidate");
  g_server.sendHeader("X-Content-Type-Options", "nosniff");
  g_server.sendHeader("X-Frame-Options", "SAMEORIGIN");
  g_server.sendHeader("Referrer-Policy", "no-referrer");
  g_server.send_P(200, "text/html; charset=utf-8", APP_HTML);
}

static void handleFavicon() {
  g_server.send(204, "text/plain", "");
}

static void handlePing() {
  jsonReset();
  jsonObjBegin();
  jsonU32("ok", 1u);
  jsonU64("bootUs", g_bootUs);
  jsonObjEnd();
  jsonSend();
}

static void handleNotFoundRoute() {
  String uri = g_server.uri();
  if (uri.startsWith("/api/")) {
    jsonSendError(404, "not_found");
    return;
  }
  String host = g_server.hostHeader();
  bool localHost = (host == "192.168.4.1") || (host.length() == 0);
  if (!localHost) {
    captiveRedirect();
    return;
  }
  handleRootDocument();
}

static const char* HEADER_COLLECT_KEYS[] = { "X-Pin" };

static void registerRoutes() {
  g_server.collectHeaders(HEADER_COLLECT_KEYS, 1);
  g_server.on("/", HTTP_GET, handleRootDocument);
  g_server.on("/index.html", HTTP_GET, handleRootDocument);
  g_server.on("/favicon.ico", HTTP_GET, handleFavicon);

  g_server.on("/generate_204", HTTP_GET, handleCaptiveAndroid204);
  g_server.on("/gen_204", HTTP_GET, handleCaptiveAndroid204);
  g_server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveApple);
  g_server.on("/library/test/success.html", HTTP_GET, handleCaptiveApple);
  g_server.on("/connecttest.txt", HTTP_GET, handleCaptiveWindows);
  g_server.on("/redirect", HTTP_GET, handleCaptiveWindows);
  g_server.on("/ncsi.txt", HTTP_GET, handleCaptiveWindows);
  g_server.on("/fwlink", HTTP_GET, handleCaptiveWindows);
  g_server.on("/canonical.html", HTTP_GET, handleCaptiveWindows);
  g_server.on("/success.txt", HTTP_GET, handleCaptiveWindows);

  g_server.on("/api/ping", HTTP_GET, handlePing);
  g_server.on("/api/state", HTTP_GET, handleApiState);
  g_server.on("/api/stats", HTTP_GET, handleApiStats);
  g_server.on("/api/diag", HTTP_GET, handleApiDiag);
  g_server.on("/api/config", HTTP_GET, handleApiConfigGet);
  g_server.on("/api/config", HTTP_POST, handleApiConfigPost);
  g_server.on("/api/config/defaults", HTTP_POST, handleApiConfigDefaults);
  g_server.on("/api/backup", HTTP_GET, handleApiBackup);
  g_server.on("/api/restore", HTTP_POST, handleApiConfigPost);
  g_server.on("/api/channel", HTTP_POST, handleApiChannel);
  g_server.on("/api/credit", HTTP_POST, handleApiCredit);
  g_server.on("/api/coin", HTTP_POST, handleApiCoinSimulate);
  g_server.on("/api/buzzer", HTTP_POST, handleApiBuzzer);
  g_server.on("/api/stats/reset", HTTP_POST, handleApiResetStats);
  g_server.on("/api/stats/save", HTTP_POST, handleApiStatsSave);
  g_server.on("/api/sessions/clear", HTTP_POST, handleApiClearSessions);
  g_server.on("/api/factory-reset", HTTP_POST, handleApiFactoryReset);
  g_server.on("/api/reboot", HTTP_POST, handleApiReboot);

  g_server.onNotFound(handleNotFoundRoute);
}

// ======================================================
// WEB SERVICE TASK
// ======================================================

static void webServiceTask(void* param) {
  (void)param;
  for (;;) {
    g_dns.processNextRequest();
    g_server.handleClient();
    vTaskDelay(1);
  }
}

// ======================================================
// CONTROL TASK COMMAND DISPATCH
// ======================================================

static void commandExecute(const Command& cmd) {
  switch (cmd.type) {
    case CMD_CH_START:
      channelStart(cmd.channel);
      break;
    case CMD_CH_PAUSE:
      channelPause(cmd.channel);
      break;
    case CMD_CH_RESUME:
      channelResume(cmd.channel);
      break;
    case CMD_CH_STOP:
      channelStop(cmd.channel, 4);
      break;
    case CMD_CH_TOGGLE:
      channelToggle(cmd.channel);
      break;
    case CMD_CH_ADD:
      channelAddTime(cmd.channel, cmd.seconds);
      break;
    case CMD_ADD_CREDIT:
      creditAddSeconds(cmd.value);
      buzzerPlay(BUZ_COIN);
      break;
    case CMD_SIM_COIN: {
      CoinAward award = coinResolve(cmd.value);
      if (award.seconds > 0) {
        coinAcceptAward(award, cmd.value);
      } else {
        buzzerPlay(BUZ_ERROR);
      }
      break;
    }
    case CMD_RESET_STATS: {
      uint64_t uptime = g_stats.uptimeSeconds;
      uint32_t boots = g_stats.boots;
      statsSetDefaults(g_stats);
      g_stats.uptimeSeconds = uptime;
      g_stats.boots = boots;
      g_stats.dayIndex = dayIndexFromUptime(uptime);
      g_stats.dayStartUptime = (uint64_t)g_stats.dayIndex * 86400ULL;
      statsSaveNow();
      buzzerPlay(BUZ_RESET);
      break;
    }
    case CMD_CLEAR_SESSIONS:
      g_stats.sessionHead = 0;
      g_stats.sessionCount = 0;
      memset(g_stats.sessions, 0, sizeof(g_stats.sessions));
      statsSaveNow();
      break;
    case CMD_SAVE_STATS:
      statsSaveNow();
      break;
    case CMD_BUZZER_TEST:
      buzzerPlay((uint8_t)cmd.value);
      break;
    case CMD_FACTORY_RESET:
      factoryWipeAndRestart();
      break;
    case CMD_REBOOT:
      relayForceOffAll();
      buzzerSilence();
      statsSaveNow();
      ESP.restart();
      break;
    default:
      break;
  }
}

static bool cmdTakeDue(uint64_t nowUs, Command& out) {
  bool found = false;
  portENTER_CRITICAL(&g_mux);
  for (uint8_t offset = 0; offset < g_cmdCount; ++offset) {
    uint8_t slot = (uint8_t)((g_cmdHead + offset) % CMD_QUEUE_LEN);
    if (g_cmdQueue[slot].executeAtUs > nowUs) continue;
    out = g_cmdQueue[slot];
    for (uint8_t i = offset; i + 1 < g_cmdCount; ++i) {
      uint8_t dst = (uint8_t)((g_cmdHead + i) % CMD_QUEUE_LEN);
      uint8_t src = (uint8_t)((g_cmdHead + i + 1) % CMD_QUEUE_LEN);
      g_cmdQueue[dst] = g_cmdQueue[src];
    }
    g_cmdTail = (uint8_t)((g_cmdTail + CMD_QUEUE_LEN - 1) % CMD_QUEUE_LEN);
    g_cmdCount--;
    found = true;
    break;
  }
  portEXIT_CRITICAL(&g_mux);
  return found;
}

static void commandProcess(uint64_t nowUs) {
  Command cmd;
  uint8_t guard = 0;
  while (guard < CMD_QUEUE_LEN && cmdTakeDue(nowUs, cmd)) {
    guard++;
    commandExecute(cmd);
  }
}

// ======================================================
// HARDWARE PIN ATTACHMENT
// ======================================================

static void hardwareAttachPins() {
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    uint8_t pin = g_cfg.relayPin[i];
    if (pin <= 39) {
      digitalWrite(pin, relayInactiveLevel());
      pinMode(pin, OUTPUT);
      digitalWrite(pin, relayInactiveLevel());
    }
    g_ch[i].relayPin = pin;
    g_ch[i].relayOn = 0;
    uint8_t btn = g_cfg.btnPin[i];
    if (btn <= 39) {
      pinMode(btn, INPUT_PULLUP);
    }
    g_ch[i].buttonPin = btn;
  }
  if (g_cfg.buzzerPin <= 39) {
    pinMode(g_cfg.buzzerPin, OUTPUT);
    digitalWrite(g_cfg.buzzerPin, LOW);
  }
  if (g_cfg.bootPin <= 39) {
    pinMode(g_cfg.bootPin, INPUT_PULLUP);
  }
  pulseOutputBegin();
  coinInputDetach();
  coinInputAttach();
  g_pinsAttached = 1;
}

static void hardwareReleasePins() {
  relayForceOffAll();
  buzzerSilence();
  coinInputDetach();
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    if (g_ch[i].relayPin <= 39) {
      digitalWrite(g_ch[i].relayPin, relayInactiveLevel());
    }
  }
  if (g_cfg.buzzerPin <= 39) {
    noTone(g_cfg.buzzerPin);
    digitalWrite(g_cfg.buzzerPin, LOW);
  }
  if (g_cfg.pulseOutPin <= 39) {
    digitalWrite(g_cfg.pulseOutPin, LOW);
  }
  g_pinsAttached = 0;
}

// ======================================================
// CONFIGURATION APPLICATION
// ======================================================

static void configApplyLive(const Config& next) {
  bool pinsChanged = (g_cfg.relayActiveLow != next.relayActiveLow) ||
                     (g_cfg.bootPin != next.bootPin) ||
                     (memcmp(g_cfg.relayPin, next.relayPin, sizeof(next.relayPin)) != 0) ||
                     (memcmp(g_cfg.btnPin, next.btnPin, sizeof(next.btnPin)) != 0) ||
                     (g_cfg.coinPin != next.coinPin) ||
                     (g_cfg.pulseOutPin != next.pulseOutPin) ||
                     (g_cfg.pulseOutEnabled != next.pulseOutEnabled) ||
                     (g_cfg.buzzerPin != next.buzzerPin);
  bool lcdChanged = (g_cfg.lcdAddr != next.lcdAddr) || (g_cfg.lcdSda != next.lcdSda) ||
                    (g_cfg.lcdScl != next.lcdScl) || (g_cfg.lcdCols != next.lcdCols) ||
                    (g_cfg.lcdRows != next.lcdRows) || (g_cfg.lcdBacklight != next.lcdBacklight);
  bool apChanged = (strcmp(g_cfg.apSsid, next.apSsid) != 0) ||
                   (strcmp(g_cfg.apPass, next.apPass) != 0) ||
                   (g_cfg.apChannel != next.apChannel) ||
                   (g_cfg.apHidden != next.apHidden) ||
                   (g_cfg.apMaxClients != next.apMaxClients);

  if (pinsChanged) {
    hardwareReleasePins();
  }
  if (g_cfg.timeModel != next.timeModel) {
    for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
      if (g_ch[i].state != CHS_IDLE) {
        channelFinish(i, 5);
      }
    }
    if (g_cfg.timeModel == TMO_ALLOCATED) {
      for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
        if (g_ch[i].remainMs > 0) {
          g_poolMs += g_ch[i].remainMs;
          g_ch[i].remainMs = 0;
        }
      }
    }
    g_usResidual = 0;
  }
  g_cfg = next;
  g_cfg.magic = CONFIG_MAGIC;
  g_cfg.version = CONFIG_VERSION;
  if (pinsChanged) {
    hardwareAttachPins();
    for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
      buttonResetRuntime(i);
    }
  }
  if (g_cfg.buzzerEnabled == 0) {
    buzzerSilence();
  }
  if (lcdChanged) {
    lcdInit();
  }
  if (apChanged) {
    apRestartDeferred(250);
  }
  g_lcdNextRefreshUs = timeNowUs();
  g_lcdNextProbeUs = timeNowUs();
}

static void configApplyPending() {
  Config next;
  if (!configTakePending(next)) {
    return;
  }
  char err[32];
  if (!validateConfig(next, err, sizeof(err))) {
    configFinishPending();
    return;
  }
  if (!configSave(next)) {
    configFinishPending();
    return;
  }
  configApplyLive(next);
  g_share.cfgRevision = g_share.cfgRevision + 1;
  g_statsSaveSuspended = 0;
  publishStatsAndConfig();
  publishState();
  configFinishPending();
}

// ======================================================
// STATISTICS TICK
// ======================================================

static void statsTick(uint64_t deltaUs) {
  static uint64_t residualUs = 0;
  residualUs += deltaUs;
  if (residualUs < 1000000ULL) {
    return;
  }
  uint64_t wholeSeconds = residualUs / 1000000ULL;
  residualUs -= wholeSeconds * 1000000ULL;
  g_stats.uptimeSeconds += wholeSeconds;
  dayAdvance(g_stats.uptimeSeconds);
  g_statsDirty = 1;
}

static void statsSaveTick(uint64_t nowUs) {
  if (g_statsSaveSuspended == 1) {
    return;
  }
  if (g_statsDirty == 0 && g_statsForceSave == 0) {
    return;
  }
  uint64_t intervalUs = (uint64_t)g_cfg.statsSaveIntervalSec * 1000000ULL;
  if (g_statsForceSave == 0 && (nowUs - g_lastStatsSaveUs) < intervalUs) {
    return;
  }
  g_statsForceSave = 0;
  statsSaveNow();
}

// ======================================================
// STATE PUBLISHING
// ======================================================

static void publishTick(uint64_t nowUs) {
  static uint64_t lastFastUs = 0;
  static uint64_t lastSlowUs = 0;
  if (nowUs - lastFastUs < 100000ULL) {
    return;
  }
  lastFastUs = nowUs;
  lcdRenderRows(nowUs);

  uint8_t active = 0;
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    g_share.channelState[i] = g_ch[i].state;
    g_share.channelRelay[i] = g_ch[i].relayOn;
    g_share.channelRemainMs[i] = channelRemainingMs(i);
    g_share.channelUsedMs[i] = g_ch[i].usedMs;
    if (g_ch[i].state == CHS_RUNNING) {
      active++;
    }
    uint8_t phase = 0;
    if (g_ch[i].state == CHS_PAUSED) {
      uint64_t intervalUs = (uint64_t)g_cfg.lcdBlinkMs * 1000ULL;
      if (intervalUs > 0) {
        uint64_t sinceUs = (nowUs >= g_ch[i].lastBlinkToggleUs) ? (nowUs - g_ch[i].lastBlinkToggleUs) : 0ULL;
        phase = (uint8_t)(((sinceUs / intervalUs) % 2ULL) == 1ULL ? 1 : 0);
      }
    }
    g_share.blinkPhase[i] = phase;
    snprintf(g_share.lcdLine[i], sizeof(g_share.lcdLine[i]), "%-20.20s", g_lcdRendered[i]);
  }
  g_share.activeChannels = active;
  g_share.poolMs = g_poolMs;
  g_share.bootUs = g_bootUs;
  publishState();

  if (nowUs - lastSlowUs >= 1000000ULL) {
    lastSlowUs = nowUs;
    publishStatsAndConfig();
  }
}

// ======================================================
// SETUP SEQUENCE
// ======================================================

static void setupStorage() {
  g_nvsReady = 0;
  if (!g_prefs.begin(NVS_NAMESPACE, false)) {
    configSetDefaults(g_cfg);
    statsSetDefaults(g_stats);
    metaSetDefaults(g_meta);
    return;
  }
  g_nvsReady = 1;
  metaLoad();
  statsLoad();
  uint8_t slot = 0;
  if (!configLoad(g_cfg, slot)) {
    configSetDefaults(g_cfg);
    configSave(g_cfg);
    slot = g_meta.activeSlot;
  }
  g_cfgLoadedSlot = slot;
  g_cfgShadow = g_cfg;
}

static void setupRuntime() {
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    memset(&g_ch[i], 0, sizeof(ChannelRuntime));
    g_ch[i].state = CHS_IDLE;
    g_ch[i].relayPin = g_cfg.relayPin[i];
    g_ch[i].buttonPin = g_cfg.btnPin[i];
    g_ch[i].lastBlinkToggleUs = timeNowUs();
  }
  g_poolMs = 0;
  g_usResidual = 0;
  memset(&g_buzz, 0, sizeof(g_buzz));
  g_share.lastButtonChannel = 0;
  g_share.cfgRevision = 0;
  g_share.bonusUsedSeconds = 0;
  g_share.creditAddedSeconds = 0;
  g_share.coinPulsesTotal = 0;
  g_share.bootUs = g_bootUs;
  cmdQueueReset();
}

static void setupStatisticsCounters() {
  g_stats.boots = g_stats.boots + 1;
  if (g_stats.dayStartUptime == 0) {
    g_stats.dayStartUptime = (uint64_t)g_stats.dayIndex * 86400ULL;
  }
  g_statsForceSave = 1;
  g_statsSaveSuspended = 0;
}

void setup() {
  esp_log_level_set("*", ESP_LOG_NONE);
  g_bootUs = timeNowUs();
  g_lastTickUs = g_bootUs;
  g_lastHealthUs = g_bootUs;
  g_lastApGuardUs = g_bootUs;
  g_lastStatsSaveUs = g_bootUs;
  g_lcdNextProbeUs = g_bootUs;
  g_lcdNextRefreshUs = g_bootUs;
  g_bootComplete = 0;
  g_factoryHoldArmed = 0;
  g_factoryHoldUs = 0;
  g_coinIsrCount = 0;
  g_coinIsrSeen = 0;
  g_coinRingHead = 0;
  g_coinRingTail = 0;
  g_coinRingDropped = 0;
  g_coinGroupActive = 0;
  g_coinGroupPulses = 0;
  g_coinLastPulseUs = 0;

  setupStorage();
  setupRuntime();

  if (g_cfg.bootPin <= 39) {
    pinMode(g_cfg.bootPin, INPUT_PULLUP);
  }
  hardwareAttachPins();
  for (uint8_t i = 0; i < CHANNEL_COUNT; ++i) {
    buttonResetRuntime(i);
  }
  setupStatisticsCounters();
  lcdInit();

  snprintf(g_overlay[0], sizeof(g_overlay[0]), "  %s", "CARWASH 4-IN-1");
  snprintf(g_overlay[1], sizeof(g_overlay[1]), "  CONTROLLER %s", FW_VERSION);
  snprintf(g_overlay[2], sizeof(g_overlay[2]), "  %s", g_cfg.apSsid);
  snprintf(g_overlay[3], sizeof(g_overlay[3]), "  192.168.4.1");
  g_overlayActive = 1;
  g_overlayUntilUs = timeNowUs() + 3000000ULL;
  lcdRenderRows(timeNowUs());
  lcdFlushRows();

  apStartServices();
  registerRoutes();
  g_server.begin();
  xTaskCreatePinnedToCore(webServiceTask, "websvc", WEB_TASK_STACK, nullptr,
                          WEB_TASK_PRIORITY, nullptr, WEB_TASK_CORE);
  g_webReady = 1;

  buzzerPlay(BUZ_BOOT);
  publishStatsAndConfig();
  publishState();
}

// ======================================================
// MAIN CONTROL LOOP
// ======================================================

void loop() {
  uint64_t nowUs = timeNowUs();
  uint64_t deltaUs = (nowUs >= g_lastTickUs) ? (nowUs - g_lastTickUs) : 0ULL;
  g_lastTickUs = nowUs;

  if (g_bootComplete == 0 && (nowUs - g_bootUs) > 2500000ULL) {
    g_bootComplete = 1;
    g_overlayActive = 0;
    g_lcdNextRefreshUs = nowUs;
  }

  engineTick(deltaUs);
  statsTick(deltaUs);
  commandProcess(nowUs);
  configApplyPending();
  buttonPollAll(nowUs);
  coinProcessRing(nowUs);
  pulseOutputTick(nowUs);
  buzzerTick(nowUs);
  factoryResetPoll(nowUs);
  lcdTick(nowUs);
  healthTick(nowUs);
  apGuardTick(nowUs);
  statsSaveTick(nowUs);
  publishTick(nowUs);

  vTaskDelay(1);
}
