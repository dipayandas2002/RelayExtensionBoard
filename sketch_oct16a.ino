#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <EEPROM.h>             // --- ADDED ---
#include <time.h>               // --- ADDED ---

const char* ssid = "POCOX3";
const char* password = "dipayandas2002";

const char* mqtt_server = "e6e7dac5f1a2475382f05acc556ef6d7.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "dipudas";
const char* mqtt_pass = "Dipu2002";

#define NUM_RELAYS 4
int relayPins[NUM_RELAYS] = {5, 4, 14, 12};
bool relayState[NUM_RELAYS] = {false, false, false, false};

// --- ADDED: SCHEDULE STRUCT ---
struct Schedule {
  int onHour, onMinute;
  int offHour, offMinute;
  bool enabled;
} relaySchedule[NUM_RELAYS];

// MQTT Topics
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
const char* topicSetTimer = "home/relay/set_timer"; // --- ADDED ---

WiFiClientSecure wifiSecureClient;
PubSubClient client(wifiSecureClient);

// ----------------------
// EEPROM FUNCTIONS
// ----------------------
void saveToEEPROM() { // --- ADDED ---
  EEPROM.begin(512);
  int addr = 0;

  // Save relay states
  for (int i = 0; i < NUM_RELAYS; i++) {
    EEPROM.write(addr++, relayState[i]);
  }

  // Save schedules
  for (int i = 0; i < NUM_RELAYS; i++) {
    EEPROM.put(addr, relaySchedule[i]);
    addr += sizeof(Schedule);
  }

  EEPROM.commit();
}

void loadFromEEPROM() { // --- ADDED ---
  EEPROM.begin(512);
  int addr = 0;

  // Load relay states
  for (int i = 0; i < NUM_RELAYS; i++) {
    relayState[i] = EEPROM.read(addr++);
  }

  // Load schedules
  for (int i = 0; i < NUM_RELAYS; i++) {
    EEPROM.get(addr, relaySchedule[i]);
    addr += sizeof(Schedule);
  }

  EEPROM.end();

  // Apply states
  for (int i = 0; i < NUM_RELAYS; i++) {
    digitalWrite(relayPins[i], relayState[i] ? HIGH : LOW);
  }
}

// ----------------------
// Publish a relay state
// ----------------------
void publishStatus(int ch) {
  char payload[100];
  snprintf(payload, sizeof(payload),
           "{\"relay\": %d, \"state\": \"%s\", \"timestamp\": %lu}",
           ch + 1, relayState[ch] ? "ON" : "OFF", millis());
  client.publish(topicStatus[ch], payload, true);
  Serial.printf("[OUT] Published status -> Topic: %s, Payload: %s\n", topicStatus[ch], payload);
}

// ----------------------
// Handle command for a relay
// ----------------------
void handleCommand(const char* topic, const String& cmd) {
  Serial.printf("[IN ] Received command -> Topic: %s, Payload: %s\n", topic, cmd.c_str());
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (String(topic) == topicCommand[i]) {
      bool newState = relayState[i];
      if (cmd.equalsIgnoreCase("ON")) newState = true;
      else if (cmd.equalsIgnoreCase("OFF")) newState = false;

      if (newState != relayState[i]) {
        relayState[i] = newState;
        digitalWrite(relayPins[i], relayState[i] ? HIGH : LOW);
        Serial.printf("[INFO] Relay %d set to %s\n", i + 1, relayState[i] ? "ON" : "OFF");
        publishStatus(i);
        saveToEEPROM(); // --- ADDED ---
      }
    }
  }
}

// ----------------------
// Handle timer setup (REPLACE EXISTING FUNCTION)
// ----------------------
void handleTimerSetup(const String& payload) {
  // Expected JSON: {"relay":1,"on":"12:30","off":"14:45","enabled":true}
  Serial.printf("[TIMER-IN] raw payload: %s\n", payload.c_str());

  // Default invalid index
  int relayIndex = -1;

  // --- parse relay ---
  int pRelay = payload.indexOf("\"relay\":");
  if (pRelay >= 0) {
    int start = pRelay + 8;
    int end = payload.indexOf(',', start);
    if (end == -1) end = payload.indexOf('}', start);
    if (end > start) {
      relayIndex = payload.substring(start, end).toInt() - 1;
    }
  }

  if (relayIndex < 0 || relayIndex >= NUM_RELAYS) {
    Serial.println("[TIMER-ERR] invalid relay index");
    return;
  }

  // Helper lambda to parse a "HH:MM" string after a key like "\"on\":\""
  auto parseTimeField = [&](const String &key, int &outH, int &outM) -> bool {
    outH = outM = 0;
    String needle = String("\"") + key + String("\":\"");
    int p = payload.indexOf(needle);
    if (p < 0) return false;
    int start = p + needle.length();               // start points at first digit of HH
    int colonPos = payload.indexOf(':', start);    // look for the colon inside the time
    if (colonPos < 0) return false;
    int endQuote = payload.indexOf('"', colonPos); // closing quote after MM
    if (endQuote < 0) return false;

    String sh = payload.substring(start, colonPos);
    String sm = payload.substring(colonPos + 1, endQuote);
    // sanity trim
    sh.trim(); sm.trim();
    if (sh.length() == 0 || sm.length() == 0) return false;

    outH = sh.toInt();
    outM = sm.toInt();
    // validate ranges
    if (outH < 0 || outH > 23) return false;
    if (outM < 0 || outM > 59) return false;
    return true;
  };

  // --- parse on time ---
  int onH=0, onM=0, offH=0, offM=0;
  bool gotOn = parseTimeField("on", onH, onM);
  bool gotOff = parseTimeField("off", offH, offM);

  // --- parse enabled ---
  bool enabled = false;
  int pEnabled = payload.indexOf("\"enabled\":");
  if (pEnabled >= 0) {
    int startE = pEnabled + 10;
    // check substring true/false
    String rem = payload.substring(startE);
    rem.trim();
    if (rem.startsWith("true")) enabled = true;
    else enabled = false;
  }

  // If at least one time parsed (prefer both) then set schedule
  if (!gotOn || !gotOff) {
    Serial.println("[TIMER-ERR] could not parse on/off times correctly");
    Serial.printf("[TIMER-ERR] gotOn=%d gotOff=%d\n", gotOn ? 1 : 0, gotOff ? 1 : 0);
    return;
  }

  relaySchedule[relayIndex].onHour = onH;
  relaySchedule[relayIndex].onMinute = onM;
  relaySchedule[relayIndex].offHour = offH;
  relaySchedule[relayIndex].offMinute = offM;
  relaySchedule[relayIndex].enabled = enabled;

  Serial.printf("[TIMER] Relay %d Timer set -> ON: %02d:%02d OFF: %02d:%02d Enabled: %d\n",
                relayIndex + 1, onH, onM, offH, offM, enabled ? 1 : 0);

  saveToEEPROM();
}

// ----------------------
// Check schedules
// ----------------------
void checkSchedules() { // --- ADDED ---
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  int curH = t->tm_hour;
  int curM = t->tm_min;

  for (int i = 0; i < NUM_RELAYS; i++) {
    if (!relaySchedule[i].enabled) continue;

    int onMins = relaySchedule[i].onHour * 60 + relaySchedule[i].onMinute;
    int offMins = relaySchedule[i].offHour * 60 + relaySchedule[i].offMinute;
    int curMins = curH * 60 + curM;

    bool shouldBeOn = (curMins >= onMins && curMins < offMins);
    if (shouldBeOn != relayState[i]) {
      relayState[i] = shouldBeOn;
      digitalWrite(relayPins[i], relayState[i] ? HIGH : LOW);
      publishStatus(i);
      saveToEEPROM();
      Serial.printf("[AUTO] Relay %d turned %s by schedule\n", i + 1, relayState[i] ? "ON" : "OFF");
    }
  }
}

// ----------------------
// MQTT callback
// ----------------------
void callback(char* topic, byte* payload, unsigned int length) {
  String cmd = "";
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];

  if (String(topic) == topicGetStates) {
    Serial.println("[IN ] Get states request received");
    for (int i = 0; i < NUM_RELAYS; i++) publishStatus(i);
  } else if (String(topic) == topicSetTimer) {
    handleTimerSetup(cmd);
  } else {
    handleCommand(topic, cmd);
  }
}

// ----------------------
// Wi-Fi & MQTT Connect
// ----------------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
}

void connectMQTT() {
  if (client.connected()) return;
  if (client.connect("ESP8266_4Relay_TLS", mqtt_user, mqtt_pass)) {
    Serial.println("MQTT connected.");

    for (int i = 0; i < NUM_RELAYS; i++) client.subscribe(topicCommand[i]);
    client.subscribe(topicGetStates);
    client.subscribe(topicSetTimer); // --- ADDED ---

    for (int i = 0; i < NUM_RELAYS; i++) publishStatus(i);
  } else {
    Serial.printf("MQTT connect failed, rc=%d\n", client.state());
  }
}

// ----------------------
// Setup & Loop
// ----------------------
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }

  connectWiFi();
  wifiSecureClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  // --- ADDED ---
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov"); // Sync NTP (IST +5:30)
  loadFromEEPROM();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!client.connected()) connectMQTT();
  client.loop();

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 60000) { // Check every minute
    lastCheck = millis();
    checkSchedules();
  }
}
