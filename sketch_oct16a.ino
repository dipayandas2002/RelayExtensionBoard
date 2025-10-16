#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

const char* ssid = "POCOX3";
const char* password = "dipayandas2002";

const char* mqtt_server = "e6e7dac5f1a2475382f05acc556ef6d7.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "dipudas";
const char* mqtt_pass = "Dipu2002";

#define NUM_RELAYS 4
int relayPins[NUM_RELAYS] = {5, 4, 14, 12};
bool relayState[NUM_RELAYS] = {false, false, false, false};

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

WiFiClientSecure wifiSecureClient;
PubSubClient client(wifiSecureClient);

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
      }
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

    // Publish existing states immediately
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
  wifiSecureClient.setInsecure(); // skip cert verification

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!client.connected()) connectMQTT();
  client.loop(); // process incoming messages
}
