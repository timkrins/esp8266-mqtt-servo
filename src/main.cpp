/*
  ESP8266 MQTT Remote Servo\
*/

#include "secrets.h"
#include <ESP8266WiFiMulti.h>
#include <PubSubClient.h>
#include <RingBuf.h>
#include <Servo.h>

#ifdef SECURE
BearSSL::WiFiClientSecure esp_client;
#endif
#ifndef SECURE
WiFiClient esp_client;
#endif

ESP8266WiFiMulti wifiMulti;
PubSubClient client(esp_client);
RingBuf<char, 300> actions_buffer;

#define SERVO_PIN D0
Servo servo;

#define MIN_ANGLE -100
#define MAX_ANGLE 100

unsigned long clock_set = 0;
unsigned long hold_until = 0;
int desired_angle;

// WiFi connect timeout per AP. Increase when connecting takes longer.
const uint32_t connectTimeoutMs = 5000;

void setupWifi() {
  delay(10);
  // Don't save WiFi configuration in flash - optional
  WiFi.persistent(false);

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);

  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to Wifi");

  // Register multi WiFi networks
  wifiMulti.addAP(SECRETS_WIFI_SSID_1, SECRETS_WIFI_PASSWORD_1);
  wifiMulti.addAP(SECRETS_WIFI_SSID_2, SECRETS_WIFI_PASSWORD_2);
  wifiMulti.addAP(SECRETS_WIFI_SSID_3, SECRETS_WIFI_PASSWORD_3);

  randomSeed(micros());

  wifiMulti.run(connectTimeoutMs);
}

void setupClock() {
  if (clock_set) {
    return;
  }
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("");
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.print("Current time: ");
  Serial.print(asctime(&timeinfo));
  clock_set = 1;
}

#ifdef SECURE
void setupTls(BearSSL::WiFiClientSecure esp_client) {
  BearSSL::X509List *serverTrustedCA = new BearSSL::X509List(SECRETS_CA_CERT);
  BearSSL::X509List *serverCertList = new BearSSL::X509List(SECRETS_CLIENT_CERT);
  BearSSL::PrivateKey *serverPrivateKey = new BearSSL::PrivateKey(SECRETS_CLIENT_PRIVATE_KEY);
  esp_client.setTrustAnchors(serverTrustedCA);
  esp_client.setClientRSACert(serverCertList, serverPrivateKey);
}
#endif

void callback(char *topic, byte *payload, unsigned int length) {
  String payload_string = "<callback topic=\"" + String(topic) + ">";
  for (unsigned int i = 0U; i < length; i++) {
    actions_buffer.push((char)payload[i]);
    payload_string.concat((char)payload[i]);
  }
  // in case we forget
  actions_buffer.push('\n');
  payload_string.concat("</callback>");

  Serial.println(payload_string);
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String client_id = "ESP8266Client-";
    client_id += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(client_id.c_str(), SECRETS_MQTT_USERNAME, SECRETS_MQTT_PASSWORD, SECRETS_MQTT_TOPIC_ALIVE, 0, false, "0")) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      client.publish(SECRETS_MQTT_TOPIC_ALIVE, "1");
      // ... and resubscribe
      client.subscribe(SECRETS_MQTT_TOPIC_CONTROL);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void flash(int milliseconds) {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(milliseconds);
  digitalWrite(LED_BUILTIN, LOW);
}

void hold(int milliseconds) { hold_until = millis() + milliseconds; }

void angle(int degrees) {
  if (degrees >= MIN_ANGLE && degrees <= MAX_ANGLE) {
    desired_angle = degrees;
  }
}

void clearBufferItems(int size) {
  for (int i = 0; i < size; i++) {
    char ignored;
    actions_buffer.pop(ignored);
  }
}

void holdOrAngle() {
  unsigned long now = millis();
  if ((hold_until != 0) && (now < hold_until)) {
    servo.attach(SERVO_PIN);
    servo.write(desired_angle);
  } else {
    hold_until = 0;
    servo.detach();
  }
}

void process() {
  if (hold_until) {
    // don't do anything, otherwise we could accidentally reset our hold time
    return;
  }
  for (int i = 0; i < actions_buffer.size(); i++) {
    if (actions_buffer[i] == '\n') {
      // found a newline, now lets process this chunk.
      // all chunks start with a char and then a value
      char command;
      actions_buffer.pop(command);
      // H1000 will hold for one second
      // A0 will set angle to 0 degrees
      // A25 will set angle to 20 degrees
      // might as well bail out early
      if (i <= 1) return;
      String temp_value = "";
      for (int j = 0; j < (i - 1); j++) {
        temp_value.concat(actions_buffer[j]);
      }
      int payload_value = temp_value.toInt();
      switch (command) {
        case 'H':
        case 'h':
          hold(payload_value);
          break;
        case 'A':
        case 'a':
          angle(payload_value);
          break;
        case 'F':
        case 'f':
          angle(payload_value);
          break;
        default:
          break;
      }
      clearBufferItems(i);
      break;
    }
  }
}

void printBuffer() {
  if (actions_buffer.size() < 1) {
    return;
  }

  String buffer_string = "<buffer>";

  for (int i = 0; i < actions_buffer.size(); i++) {
    buffer_string.concat(actions_buffer[i]);
  }
  buffer_string.concat("</buffer>");

  Serial.println(buffer_string);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT); // Initialize the LED_BUILTIN pin as an output
  Serial.begin(115200);
#ifdef SECURE
  setupTls(esp_client);
#endif
  setupWifi();
  client.setServer(SECRETS_MQTT_SERVER, SECRETS_MQTT_SERVER_PORT);
  client.setCallback(callback);
}

void loop() {
  // Maintain WiFi connection
  if (wifiMulti.run(connectTimeoutMs) == WL_CONNECTED) {
    setupClock();
    if (!client.connected()) {
      reconnect();
    }
    client.loop();
    printBuffer();
    process();
    holdOrAngle();
  } else {
    Serial.println("WiFi not connected!");
    delay(1000);
  }
}
