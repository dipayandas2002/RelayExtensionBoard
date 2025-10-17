#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <time.h>

// ------------------------
// CONFIGURATION
// ------------------------
#define NUM_RELAYS 4
const int relayPins[NUM_RELAYS] = {5, 4, 14, 12};  // D1, D2, D5, D6

const char* ssid = "POCOX3";
const char* password = "dipayandas2002";

const char* mqtt_server = "e6e7dac5f1a2475382f05acc556ef6d7.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "dipudas";
const char* mqtt_pass = "Dipu2002";

// ------------------------
// RELAY & TIMER DATA
// ------------------------
struct Schedule {
  int onHour, onMinute;
  int offHour, offMinute;
  bool enabled;
};

bool relayState[NUM_RELAYS] = {false, false, false, false};
Schedule relaySchedule[NUM_RELAYS];

// ------------------------
// MQTT TOPICS
// ------------------------
const char* topicCommand[NUM_RELAYS] = {
  "home/relay1/command",
  "home/relay2/command",
  "home/relay3/command",
  "home/relay4/command"
};
const char* topicStatus[NUM_RELAYS] = {
  "home/relay1/status",
  "home/relay2/status",
  "home/relay3/status",
  "home/relay4/status"
};
const char* topicGetStates = "home/relay/get_states";
const char* topicSetTimer  = "home/relay/set_timer";

WiFiClientSecure wifiSecureClient;
PubSubClient client(wifiSecureClient);

// ------------------------
// EEPROM SAVE / LOAD
// ------------------------
void saveToEEPROM() {
  EEPROM.begin(512);
  int addr = 0;

  for (int i = 0; i < NUM_RELAYS; i++) EEPROM.write(addr++, relayState[i]);
  for (int i = 0; i < NUM_RELAYS; i++) {
    EEPROM.put(addr, relaySchedule[i]);
    addr += sizeof(Schedule);
  }

  EEPROM.commit();
  EEPROM.end();
  Serial.println("[EEPROM] Data saved");
}

void loadFromEEPROM() {
  EEPROM.begin(512);
  int addr = 0;

  for (int i = 0; i < NUM_RELAYS; i++) relayState[i] = EEPROM.read(addr++);
  for (int i = 0; i < NUM_RELAYS; i++) {
    EEPROM.get(addr, relaySchedule[i]);
    addr += sizeof(Schedule);
  }

  EEPROM.end();

  for (int i = 0; i < NUM_RELAYS; i++)
    digitalWrite(relayPins[i], relayState[i] ? HIGH : LOW);

  Serial.println("[EEPROM] Data loaded");
}

// ------------------------
// TIME UTILS
// ------------------------
void printLocalTime() {
  time_t now = getCurrentTimeOfflineSafe();
  struct tm* t = localtime(&now);
  Serial.printf("[TIME] %02d:%02d:%02d %02d/%02d/%04d\n",
                t->tm_hour, t->tm_min, t->tm_sec,
                t->tm_mday, t->tm_mon + 1, t->tm_year + 1900);
}

// ------------------------
// TIME MAINTENANCE
// ------------------------
unsigned long lastNtpSync = 0;
unsigned long lastMillisBase = 0;
time_t lastSyncedEpoch = 0;

time_t getCurrentTimeOfflineSafe() {
  unsigned long elapsed = (millis() - lastMillisBase) / 1000;
  return lastSyncedEpoch + elapsed;
}

void syncNtpIfNeeded() {
  static bool firstSyncDone = false;
  unsigned long nowMillis = millis();

  if (WiFi.status() == WL_CONNECTED &&
      (!firstSyncDone || nowMillis - lastNtpSync > 1800000)) { // 30 min
    Serial.println("[TIME] Re-syncing NTP...");
    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
    delay(1000);

    time_t now = time(nullptr);
    if (now > 100000) {
      lastSyncedEpoch = now;
      lastMillisBase = millis();
      lastNtpSync = nowMillis;
      firstSyncDone = true;
      Serial.println("[TIME] NTP sync OK");
      printLocalTime();
    } else {
      Serial.println("[TIME] NTP sync failed, will retry later");
    }
  }
}

// ------------------------
// MQTT PUBLISH
// ------------------------
void publishStatus(int ch) {
  char payload[150];
  time_t now = getCurrentTimeOfflineSafe();
  struct tm* t = localtime(&now);
  snprintf(payload, sizeof(payload),
           "{\"relay\":%d,\"state\":\"%s\",\"time\":\"%02d:%02d\",\"on\":\"%02d:%02d\",\"off\":\"%02d:%02d\",\"enabled\":%s}",
           ch + 1,
           relayState[ch] ? "ON" : "OFF",
           t->tm_hour, t->tm_min,
           relaySchedule[ch].onHour, relaySchedule[ch].onMinute,
           relaySchedule[ch].offHour, relaySchedule[ch].offMinute,
           relaySchedule[ch].enabled ? "true" : "false");

  client.publish(topicStatus[ch], payload, true);
  Serial.printf("[OUT] %s -> %s\n", topicStatus[ch], payload);
}

// ------------------------
// MQTT COMMAND HANDLER
// ------------------------
void handleCommand(const char* topic, const String& cmd) {
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (String(topic) == topicCommand[i]) {
      bool newState = cmd.equalsIgnoreCase("ON");
      if (relayState[i] != newState) {
        relayState[i] = newState;
        digitalWrite(relayPins[i], relayState[i] ? HIGH : LOW);
        publishStatus(i);
        saveToEEPROM();
        Serial.printf("[CMD] Relay %d -> %s\n", i + 1, newState ? "ON" : "OFF");
      }
    }
  }
}

// ------------------------
// TIMER SETUP HANDLER
// ------------------------
void handleTimerSetup(const String& payload) {
  Serial.printf("[TIMER-IN] %s\n", payload.c_str());
  int relayIndex = -1;

  int pRelay = payload.indexOf("\"relay\":");
  if (pRelay >= 0) {
    int start = pRelay + 8;
    int end = payload.indexOf(',', start);
    if (end == -1) end = payload.indexOf('}', start);
    relayIndex = payload.substring(start, end).toInt() - 1;
  }

  if (relayIndex < 0 || relayIndex >= NUM_RELAYS) {
    Serial.println("[TIMER-ERR] Invalid relay index");
    return;
  }

  auto parseTime = [&](String key, int &h, int &m) {
    int p = payload.indexOf("\"" + key + "\":\"");
    if (p < 0) return false;
    p += key.length() + 4;
    int colon = payload.indexOf(':', p);
    int end = payload.indexOf('"', colon);
    if (colon < 0 || end < 0) return false;
    h = payload.substring(p, colon).toInt();
    m = payload.substring(colon + 1, end).toInt();
    return true;
  };

  int onH, onM, offH, offM;
  if (!parseTime("on", onH, onM) || !parseTime("off", offH, offM)) {
    Serial.println("[TIMER-ERR] Parse failed");
    return;
  }

  bool enabled = payload.indexOf("\"enabled\":true") >= 0;

  relaySchedule[relayIndex] = {onH, onM, offH, offM, enabled};
  saveToEEPROM();

  Serial.printf("[TIMER] Relay %d: %02d:%02d -> %02d:%02d, enabled=%d\n",
                relayIndex + 1, onH, onM, offH, offM, enabled);
  publishStatus(relayIndex);
}

// ------------------------
// SCHEDULE CHECK WITH MANUAL OVERRIDE
// ------------------------
void checkSchedules() {
  time_t now = getCurrentTimeOfflineSafe();
  struct tm* t = localtime(&now);
  int curSecs = t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;

  for (int i = 0; i < NUM_RELAYS; i++) {
    if (!relaySchedule[i].enabled) continue;

    int onSecs  = relaySchedule[i].onHour * 3600 + relaySchedule[i].onMinute * 60;
    int offSecs = relaySchedule[i].offHour * 3600 + relaySchedule[i].offMinute * 60;

    // Turn ON exactly at scheduled ON time if relay is OFF
    if (curSecs == onSecs && !relayState[i]) {
      relayState[i] = true;
      digitalWrite(relayPins[i], HIGH);
      publishStatus(i);
      saveToEEPROM();
      Serial.printf("[AUTO] Relay %d -> ON (by schedule)\n", i + 1);
    }

    // Turn OFF exactly at scheduled OFF time if relay is ON
    if (curSecs == offSecs && relayState[i]) {
      relayState[i] = false;
      digitalWrite(relayPins[i], LOW);
      publishStatus(i);
      saveToEEPROM();
      Serial.printf("[AUTO] Relay %d -> OFF (by schedule)\n", i + 1);
    }
  }
}

// ------------------------
// MQTT CALLBACK
// ------------------------
void callback(char* topic, byte* payload, unsigned int length) {
  String cmd;
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];

  if (String(topic) == topicGetStates) {
    for (int i = 0; i < NUM_RELAYS; i++) publishStatus(i);
  } else if (String(topic) == topicSetTimer) {
    handleTimerSetup(cmd);
  } else {
    handleCommand(topic, cmd);
  }
}

// ------------------------
// WIFI + MQTT CONNECTION
// ------------------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
}

void connectMQTT() {
  if (client.connected()) return;
  Serial.print("[MQTT] Connecting...");
  if (client.connect("ESP8266_4Relay_TLS", mqtt_user, mqtt_pass)) {
    Serial.println("Connected!");
    for (int i = 0; i < NUM_RELAYS; i++) client.subscribe(topicCommand[i]);
    client.subscribe(topicGetStates);
    client.subscribe(topicSetTimer);
    for (int i = 0; i < NUM_RELAYS; i++) publishStatus(i);
  } else {
    Serial.printf("Failed, rc=%d\n", client.state());
  }
}

// ------------------------
// SETUP
// ------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] Starting 4-Relay MQTT Controller...");

  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }

  connectWiFi();
  wifiSecureClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov"); // IST +5:30
  Serial.println("[TIME] Syncing NTP...");
  delay(2000);
  lastSyncedEpoch = time(nullptr);
  lastMillisBase = millis();
  lastNtpSync = millis();
  printLocalTime();

  loadFromEEPROM();
}

// ------------------------
// MAIN LOOP
// ------------------------
void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!client.connected()) connectMQTT();
  client.loop();

  syncNtpIfNeeded();  // maintain time accuracy every 30 min

  static unsigned long lastCheck = 0;
  static unsigned long lastTimePrint = 0;
  unsigned long now = millis();

  if (now - lastCheck > 1000) {  // check schedules every 1 sec
    lastCheck = now;
    checkSchedules();
  }

  if (now - lastTimePrint > 5000) { // print time every 5s
    lastTimePrint = now;
    printLocalTime();
  }
}
