// FINAL MQTT MASTER — ESP32 Arduino core 3.x
// Libraries: PubSubClient, Blynk, LiquidCrystal_I2C.
// 3 nodes publish sound/nodeN (numeric) and impact/nodeN ("peak,duration").
// IR is on Node 1. LCD I2C: SDA21 SCL22 address 0x27. Buzzer signal: GPIO14.
// DEMO / PROTOTYPE ONLY. Public MQTT is not suitable for dependable emergency care.
#define BLYNK_TEMPLATE_ID "TMPL348_0DO7P"
#define BLYNK_TEMPLATE_NAME "Elderly Monitoring System"
#define BLYNK_AUTH_TOKEN "1JUQUfUH_1j-tPB63urvXnFoCJD-bTLm"

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <BlynkSimpleEsp32.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char* WIFI_SSID = "motorolaedge_8523";
const char* WIFI_PASSWORD = "kumar3477";
const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;
const char* PROJECT_ID = "elderly_monitor_7K92P4X1";

const int BUZZER_PIN = 14;
// Choose ACTIVE for a buzzer that sounds with steady 3.3 V,
// or PASSIVE for a piezo that needs an oscillating tone.
// If an active buzzer is active-LOW, change BUZZER_ACTIVE_HIGH to false.
const bool BUZZER_IS_PASSIVE = false;
const bool BUZZER_ACTIVE_HIGH = true;
const unsigned int PASSIVE_TONE_HZ = 2300;
const unsigned long BUZZER_ON_MS = 350;
const unsigned long BUZZER_OFF_MS = 350;
const unsigned long BUZZER_SELF_TEST_MS = 1000;

LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
char soundFilter[120], impactFilter[120], irTopic[120], clientId[100];

int sound[3] = {0, 0, 0};
unsigned long receivedAt[3] = {0, 0, 0};
bool everReceived[3] = {false, false, false};
bool remoteObstruction = false, irEverReceived = false;
unsigned long irReceivedAt = 0;
bool impactPending[3] = {false, false, false};
int impactPeak[3] = {0, 0, 0};
bool groupActive = false;
unsigned long groupStartedAt = 0;

const unsigned long FRESH_MS = 2500;
const unsigned long IR_FRESH_MS = 3000;
const unsigned long EVENT_GROUP_MS = 150;
const unsigned long ALERT_DISPLAY_MS = 4500;
const unsigned long ALERT_COOLDOWN_MS = 30000;
const unsigned long SCREEN_MS = 500;
const unsigned long BLYNK_STATUS_MS = 1000;
const int MIN_REPORTED_IMPACT_PEAK = 150;

unsigned long lastWifiAttempt = 0, lastMqttAttempt = 0, lastBlynkAttempt = 0;
unsigned long lastSoundAlert = 0, lastHazardAlert = 0;
unsigned long lastScreen = 0, lastBlynkStatus = 0;
bool soundAlertEverSent = false, hazardAlertEverSent = false;
unsigned long alertShownAt = 0;
int estimatedAngle = -1;
String estimatedZone = "WAITING";
bool alarmSound = false;

// Nonblocking buzzer scheduler. The startup self-test is intentionally audible.
unsigned long buzzerTestStartedAt = 0;
bool buzzerOn = false;
unsigned long buzzerPhaseAt = 0;

void setBuzzer(bool on) {
  if (BUZZER_IS_PASSIVE) {
    if (on) ledcWriteTone(BUZZER_PIN, PASSIVE_TONE_HZ);
    else ledcWriteTone(BUZZER_PIN, 0);
  } else {
    digitalWrite(BUZZER_PIN, on == BUZZER_ACTIVE_HIGH ? HIGH : LOW);
  }
}

void updateBuzzer(bool hazard, unsigned long now) {
  bool selfTest = (now - buzzerTestStartedAt < BUZZER_SELF_TEST_MS);
  bool wantAlarm = hazard || alarmSound;
  if (selfTest) {
    if (!buzzerOn) { setBuzzer(true); buzzerOn = true; }
    return;
  }
  if (!wantAlarm) {
    if (buzzerOn) setBuzzer(false);
    buzzerOn = false;
    buzzerPhaseAt = now;
    return;
  }
  unsigned long phaseDuration = buzzerOn ? BUZZER_ON_MS : BUZZER_OFF_MS;
  if (now - buzzerPhaseAt >= phaseDuration) {
    buzzerOn = !buzzerOn;
    setBuzzer(buzzerOn);
    buzzerPhaseAt = now;
  }
}

void showLCD(const String& top, const String& bottom) {
  lcd.setCursor(0, 0); lcd.print("                ");
  lcd.setCursor(0, 1); lcd.print("                ");
  lcd.setCursor(0, 0); lcd.print(top.substring(0, 16));
  lcd.setCursor(0, 1); lcd.print(bottom.substring(0, 16));
}

int matchNodeTopic(const char* incoming, const char* kind) {
  char expected[120];
  for (int i = 1; i <= 3; ++i) {
    snprintf(expected, sizeof(expected), "elderly/%s/%s/node%d", PROJECT_ID, kind, i);
    if (strcmp(incoming, expected) == 0) return i - 1;
  }
  return -1;
}

bool parseLevel(const byte* data, unsigned int len, int& out) {
  if (len == 0 || len > 4) return false;
  int value = 0;
  for (unsigned int i = 0; i < len; ++i) {
    if (data[i] < '0' || data[i] > '9') return false;
    value = value * 10 + data[i] - '0';
  }
  if (value > 4095) return false;
  out = value;
  return true;
}

bool parseImpact(const byte* data, unsigned int len, int& peak, int& duration) {
  if (len < 3 || len > 20) return false;
  char buf[24];
  memcpy(buf, data, len);
  buf[len] = '\0';
  char* comma = strchr(buf, ',');
  if (!comma || strchr(comma + 1, ',')) return false;
  *comma = '\0';
  if (!buf[0] || !comma[1]) return false;
  for (char* p = buf; *p; ++p) if (*p < '0' || *p > '9') return false;
  for (char* p = comma + 1; *p; ++p) if (*p < '0' || *p > '9') return false;
  long p = atol(buf), d = atol(comma + 1);
  if (p < 0 || p > 4095 || d < 0 || d > 400) return false;
  peak = (int)p;
  duration = (int)d;
  return true;
}

void onMqtt(char* topic, byte* payload, unsigned int length) {
  unsigned long now = millis();
  if (strcmp(topic, irTopic) == 0) {
    if (length != 1 || (payload[0] != '0' && payload[0] != '1')) return;
    remoteObstruction = payload[0] == '1';
    irReceivedAt = now;
    irEverReceived = true;
    return;
  }
  int id = matchNodeTopic(topic, "sound");
  if (id >= 0) {
    int level;
    if (!parseLevel(payload, length, level)) return;
    sound[id] = level;
    receivedAt[id] = now;
    everReceived[id] = true;
    return;
  }
  id = matchNodeTopic(topic, "impact");
  if (id >= 0) {
    int peak, duration;
    if (!parseImpact(payload, length, peak, duration)) return;
    if (peak < MIN_REPORTED_IMPACT_PEAK) return;
    if (!groupActive) {
      groupActive = true;
      groupStartedAt = now;
    }
    impactPending[id] = true;
    if (peak > impactPeak[id]) impactPeak[id] = peak;
    Serial.printf("Impact from Node %d: peak=%d duration=%d ms\n",
                  id + 1, peak, duration);
  }
}

void connectIfNeeded() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (lastWifiAttempt == 0 || now - lastWifiAttempt >= 5000) {
      lastWifiAttempt = now;
      Serial.println("Master Wi-Fi connecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }
  if (!mqtt.connected() &&
      (lastMqttAttempt == 0 || now - lastMqttAttempt >= 5000)) {
    lastMqttAttempt = now;
    if (mqtt.connect(clientId)) {
      Serial.println("MQTT connected");
      if (!mqtt.subscribe(soundFilter)) Serial.println("Sound subscription failed");
      if (!mqtt.subscribe(impactFilter)) Serial.println("Impact subscription failed");
      if (!mqtt.subscribe(irTopic)) Serial.println("IR subscription failed");
    } else {
      Serial.printf("MQTT connection failed: %d\n", mqtt.state());
    }
  }
  if (!Blynk.connected() &&
      (lastBlynkAttempt == 0 || now - lastBlynkAttempt >= 15000)) {
    lastBlynkAttempt = now;
    Blynk.connect(1000);
  }
}

bool nodesFresh() {
  unsigned long now = millis();
  for (int i = 0; i < 3; ++i)
    if (!everReceived[i] || now - receivedAt[i] > FRESH_MS) return false;
  return true;
}

void processImpacts() {
  unsigned long now = millis();
  if (!groupActive || now - groupStartedAt < EVENT_GROUP_MS) return;
  const int referenceAngle[3] = {0, 90, 180};
  long total = 0, weighted = 0;
  int strongestNode = -1, strongestPeak = -1;
  for (int i = 0; i < 3; ++i) {
    if (impactPending[i]) {
      total += impactPeak[i];
      weighted += (long)impactPeak[i] * referenceAngle[i];
      if (impactPeak[i] > strongestPeak) {
        strongestPeak = impactPeak[i];
        strongestNode = i;
      }
    }
    impactPending[i] = false;
    impactPeak[i] = 0;
  }
  groupActive = false;
  if (total == 0) return;

  estimatedAngle = (int)roundf((float)weighted / total);
  estimatedZone = estimatedAngle < 60 ? "LEFT" :
                  (estimatedAngle < 120 ? "CENTER" : "RIGHT");
  alarmSound = true;
  alertShownAt = now;
  Serial.printf("POSSIBLE IMPACT Node %d | zone %s | index %d\n",
                strongestNode + 1, estimatedZone.c_str(), estimatedAngle);

  if (Blynk.connected()) {
    Blynk.virtualWrite(V0, estimatedAngle);
    Blynk.virtualWrite(V1, 1);
    Blynk.virtualWrite(V3, estimatedZone);
    if (!soundAlertEverSent || now - lastSoundAlert >= ALERT_COOLDOWN_MS) {
      String message = String("Possible thud/crash near ") + estimatedZone +
                       " zone (~" + String(estimatedAngle) + " deg). Check room.";
      Blynk.logEvent("sound_alert", message);
      lastSoundAlert = now;
      soundAlertEverSent = true;
      Serial.println("Blynk push event requested: " + message);
    }
  } else {
    Serial.println("Blynk offline: no push request");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  if (BUZZER_IS_PASSIVE) {
    // Arduino ESP32 core 3.x LEDC pin-based API
    if (!ledcAttach(BUZZER_PIN, PASSIVE_TONE_HZ, 8))
      Serial.println("ERROR: LEDC buzzer setup failed");
  }
  setBuzzer(false);
  buzzerTestStartedAt = millis();
  buzzerPhaseAt = millis();
  Serial.println("BUZZER SELF-TEST: should sound for ~1 second after startup");
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  showLCD("REMOTE MONITOR", "STARTING...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  snprintf(soundFilter, sizeof(soundFilter), "elderly/%s/sound/+", PROJECT_ID);
  snprintf(impactFilter, sizeof(impactFilter), "elderly/%s/impact/+", PROJECT_ID);
  snprintf(irTopic, sizeof(irTopic), "elderly/%s/ir/node1", PROJECT_ID);
  snprintf(clientId, sizeof(clientId), "elderly-%s-master", PROJECT_ID);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setSocketTimeout(2);
  Blynk.config(BLYNK_AUTH_TOKEN);
  Serial.printf("Master MAC: %s\n", WiFi.macAddress().c_str());
  Serial.println("Ready: waiting for 3 MQTT Nodes");
}

void loop() {
  connectIfNeeded();
  if (mqtt.connected()) mqtt.loop();
  if (Blynk.connected()) Blynk.run();

  unsigned long now = millis();
  processImpacts();
  if (alarmSound && now - alertShownAt >= ALERT_DISPLAY_MS) {
    alarmSound = false;
    if (Blynk.connected()) Blynk.virtualWrite(V1, 0);
  }
  bool irOnline = mqtt.connected() && irEverReceived &&
                  now - irReceivedAt <= IR_FRESH_MS;
  bool remoteHazard = irOnline && remoteObstruction;

  if (remoteHazard && Blynk.connected() &&
      (!hazardAlertEverSent || now - lastHazardAlert >= ALERT_COOLDOWN_MS)) {
    Blynk.logEvent("hazard_alert", "IR obstruction at Node 1. Please check.");
    lastHazardAlert = now;
    hazardAlertEverSent = true;
    Serial.println("Blynk IR hazard push event requested");
  }

  updateBuzzer(remoteHazard, now);

  if (now - lastBlynkStatus >= BLYNK_STATUS_MS) {
    lastBlynkStatus = now;
    if (Blynk.connected()) Blynk.virtualWrite(V2, remoteHazard ? 1 : 0);
  }
  if (now - lastScreen >= SCREEN_MS) {
    lastScreen = now;
    if (remoteHazard) showLCD("REMOTE IR ALERT", "CHECK ROOM");
    else if (alarmSound) showLCD("POSSIBLE IMPACT", "ZONE: " + estimatedZone);
    else if (!irOnline) showLCD("IR SENSOR OFFLINE", "CHECK NODE 1");
    else if (!mqtt.connected() || !nodesFresh()) showLCD("WAITING FOR", "3 REMOTE NODES");
    else showLCD("REMOTE MONITOR", "SCANNING ROOM");
  }
  delay(5);
}
