// ADAPTIVE v2 MQTT ACOUSTIC NODE — ESP32 Arduino core 3.x
// Requires: PubSubClient. MAX4466 OUT -> GPIO32, VCC -> 3V3, GND -> GND.
// NODE_ID: 1=LEFT, 2=CENTER, 3=RIGHT. IR is connected ONLY to Node 1.
// MQTT payloads: sound/nodeN = decimal 0..4095; impact/nodeN = "peak,duration"
// MQTT is public and unencrypted: demonstration only, not a clinical alarm.
#include <WiFi.h>
#include <PubSubClient.h>

const int NODE_ID = 3;
const char* WIFI_SSID = "motorolaedge_8523";
const char* WIFI_PASSWORD = "kumar3477";
const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;
const char* PROJECT_ID = "elderly_monitor_7K92P4X1";
const int MIC_PIN = 32;
const int IR_PIN = 27;
const bool HAS_IR = NODE_ID == 1;
const bool IR_ACTIVE_LOW = true;

const unsigned long WINDOW_US = 12000UL;
const unsigned long LEVEL_PUBLISH_MS = 500;
const unsigned long IR_PUBLISH_MS = 500;
const unsigned long EVENT_COOLDOWN_MS = 900;
const unsigned long MAX_IMPACT_MS = 320;
const unsigned long QUIET_CONFIRM_MS = 36;
// Experimental settings: calibrate using Serial readings.
// Starting values only: tune from home + lab serial recordings.
const int MIN_ABSOLUTE_LEVEL = 180;
const int MIN_RISE_ABOVE_BACKGROUND = 110;
const int QUIET_MARGIN = 50;
const float BACKGROUND_ALPHA = 0.025f;
const unsigned long CALIBRATION_MS = 5000;
const float ADAPTIVE_MULTIPLIER = 2.8f;
const int ADAPTIVE_NOISE_MARGIN = 55;
// When many similar impacts arrive at roughly regular intervals, suppress them.
// This is deliberately conservative: first music beats can still trigger.
const unsigned long BEAT_MIN_MS = 180;
const unsigned long BEAT_MAX_MS = 1200;
const unsigned long BEAT_TOLERANCE_MS = 110;
const unsigned long MUSIC_SUPPRESSION_MS = 6000;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
char soundTopic[120], impactTopic[120], irTopic[120], clientId[100];
unsigned long lastWifiAttempt = 0, lastMqttAttempt = 0;
unsigned long lastLevelPublish = 0, lastIRPublish = 0, lastImpact = 0;
float backgroundLevel = 30.0f;
float backgroundDeviation = 12.0f;
unsigned long calibrationStartedAt = 0;
unsigned long lastCandidateAt = 0, lastCandidateInterval = 0;
int regularBeatCount = 0;
unsigned long musicSuppressedUntil = 0;
bool tracking = false;
unsigned long eventStart = 0, quietSince = 0;
int eventPeak = 0;

int measurePeakToPeak() {
  int low = 4095, high = 0;
  unsigned long start = micros();
  while ((unsigned long)(micros() - start) < WINDOW_US) {
    int value = analogRead(MIC_PIN);
    if (value < low) low = value;
    if (value > high) high = value;
    delayMicroseconds(150);
  }
  return high - low;
}

void connectIfNeeded() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (lastWifiAttempt == 0 || now - lastWifiAttempt >= 5000) {
      lastWifiAttempt = now;
      Serial.println("Wi-Fi connecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }
  if (!mqtt.connected() &&
      (lastMqttAttempt == 0 || now - lastMqttAttempt >= 5000)) {
    lastMqttAttempt = now;
    if (mqtt.connect(clientId)) Serial.println("MQTT connected");
    else Serial.printf("MQTT connection failed: %d\n", mqtt.state());
  }
}

void publishImpact(int peak, unsigned long duration) {
  Serial.printf("IMPACT CANDIDATE node=%d peak=%d duration=%lu ms\n",
                NODE_ID, peak, duration);
  if (!mqtt.connected()) {
    Serial.println("Impact not delivered (MQTT offline)");
    return;
  }
  char payload[32];
  snprintf(payload, sizeof(payload), "%d,%lu", peak, duration);
  if (!mqtt.publish(impactTopic, payload, false))
    Serial.println("Impact publish failed");
}

void setup() {
  Serial.begin(115200);
  calibrationStartedAt = millis();
  analogReadResolution(12);
  pinMode(MIC_PIN, INPUT);
  if (HAS_IR) pinMode(IR_PIN, INPUT);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  snprintf(soundTopic, sizeof(soundTopic), "elderly/%s/sound/node%d", PROJECT_ID, NODE_ID);
  snprintf(impactTopic, sizeof(impactTopic), "elderly/%s/impact/node%d", PROJECT_ID, NODE_ID);
  snprintf(irTopic, sizeof(irTopic), "elderly/%s/ir/node1", PROJECT_ID);
  snprintf(clientId, sizeof(clientId), "elderly-%s-node%d", PROJECT_ID, NODE_ID);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(2);
  Serial.printf("Node %d ready | MAC %s\n", NODE_ID, WiFi.macAddress().c_str());
  Serial.printf("Sound %s\nImpact %s\n", soundTopic, impactTopic);
}

void loop() {
  connectIfNeeded();
  if (mqtt.connected()) mqtt.loop();

  int level = measurePeakToPeak();
  unsigned long now = millis();
  int baseline = (int)backgroundLevel;
  int rise = level - baseline;
  // During the first 5 s, learn room noise without reporting impacts.
  // Start with normal background noise, not an intentionally loud sound.
  bool calibrated = now - calibrationStartedAt >= CALIBRATION_MS;
  int dynamicRise = max(MIN_RISE_ABOVE_BACKGROUND,
                        (int)(ADAPTIVE_MULTIPLIER * backgroundDeviation +
                              ADAPTIVE_NOISE_MARGIN));
  bool candidate = calibrated && level >= MIN_ABSOLUTE_LEVEL &&
                   rise >= dynamicRise;

  if (!tracking) {
    if (candidate && (lastImpact == 0 || now - lastImpact >= EVENT_COOLDOWN_MS)) {
      // Keep a simple rhythm estimate for repeating music beats.
      if (lastCandidateAt != 0) {
        unsigned long interval = now - lastCandidateAt;
        bool rhythmic = interval >= BEAT_MIN_MS && interval <= BEAT_MAX_MS &&
                        lastCandidateInterval >= BEAT_MIN_MS &&
                        lastCandidateInterval <= BEAT_MAX_MS &&
                        labs((long)interval - (long)lastCandidateInterval) <=
                          (long)BEAT_TOLERANCE_MS;
        if (rhythmic) regularBeatCount++;
        else regularBeatCount = 0;
        lastCandidateInterval = interval;
      }
      lastCandidateAt = now;
      if (regularBeatCount >= 2) {
        musicSuppressedUntil = now + MUSIC_SUPPRESSION_MS;
        Serial.println("RHYTHMIC PATTERN: suppressing impact alerts briefly");
      }
      tracking = true;
      eventStart = now;
      eventPeak = level;
      quietSince = 0;
      Serial.printf("Candidate node=%d level=%d baseline=%d thresholdRise=%d\n",
                    NODE_ID, level, baseline, dynamicRise);
    } else {
      // Limit each update to avoid a transient immediately raising the baseline.
      float diff = level - backgroundLevel;
      float limited = constrain(diff, -40.0f, 40.0f);
      backgroundLevel += BACKGROUND_ALPHA * limited;
      float deviation = fabsf(level - backgroundLevel);
      backgroundDeviation = 0.985f * backgroundDeviation +
                            0.015f * min(deviation, 180.0f);
    }
  } else {
    if (level > eventPeak) eventPeak = level;
    if (level <= baseline + QUIET_MARGIN) {
      if (quietSince == 0) quietSince = now;
      if (now - quietSince >= QUIET_CONFIRM_MS) {
        unsigned long duration = now - eventStart;
        if (duration <= MAX_IMPACT_MS &&
            (long)(now - musicSuppressedUntil) >= 0) {
          publishImpact(eventPeak, duration);
          lastImpact = now;
        } else if ((long)(now - musicSuppressedUntil) < 0) {
          Serial.println("Impact candidate suppressed: repeated beat pattern");
        }
        tracking = false;
        quietSince = 0;
      }
    } else {
      quietSince = 0;
    }
    if (tracking && now - eventStart > MAX_IMPACT_MS) {
      Serial.println("Rejected: sustained sound or long decay");
      tracking = false;
      quietSince = 0;
    }
  }

  if (now - lastLevelPublish >= LEVEL_PUBLISH_MS) {
    lastLevelPublish = now;
    Serial.printf("Node %d p-p=%d background=%.1f dev=%.1f riseThreshold=%d %s\n",
                  NODE_ID, level, backgroundLevel, backgroundDeviation,
                  dynamicRise, calibrated ? "READY" : "CALIBRATING");
    if (mqtt.connected()) {
      char payload[16];
      snprintf(payload, sizeof(payload), "%d", level);
      if (!mqtt.publish(soundTopic, payload, false))
        Serial.println("Sound publish failed");
    }
  }

  if (HAS_IR && now - lastIRPublish >= IR_PUBLISH_MS) {
    lastIRPublish = now;
    bool blocked = IR_ACTIVE_LOW ? digitalRead(IR_PIN) == LOW
                                 : digitalRead(IR_PIN) == HIGH;
    if (mqtt.connected() &&
        !mqtt.publish(irTopic, blocked ? "1" : "0", false))
      Serial.println("IR publish failed");
    Serial.printf("Node 1 IR: %s\n", blocked ? "OBSTRUCTED" : "CLEAR");
  }
}
