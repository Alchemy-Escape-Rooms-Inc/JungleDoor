/*
 * ============================================
 * ALCHEMY ESCAPE ROOM - JUNGLE DOOR CONTROLLER
 * ESP32-S3 + Cytron MD13S Motor Driver
 * ============================================
 *
 * Automatic sliding door controlled via MQTT.
 *
 * COMMANDS (via MermaidsTale/JungleDoor/command):
 *   OPEN   -> slides door open until OPEN limit switch
 *   CLOSE  -> slides door closed until CLOSED limit switch + 500ms overrun
 *   STOP   -> emergency stop
 *   PING   -> responds PONG
 *   STATUS -> responds with full diagnostic
 *   RESET  -> stops motor, reboots
 *
 * LIMIT SWITCH HIERARCHY:
 *   Open switch active           -> door is OPEN
 *   Closed switch active         -> door is CLOSED
 *   Both inactive                -> door is in the middle (STOPPED)
 *   Both active                  -> EMERGENCY STOP
 *
 * SAFETY:
 *   6-second timeout stops motor if no limit switch is hit
 *   Both switches active = immediate emergency stop
 *
 * ============================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include "MANIFEST.h"

// ============================================
// CONFIGURATION FROM MANIFEST
// ============================================
#define DEVICE_NAME       manifest::DEVICE_NAME
#define FIRMWARE_VERSION  manifest::FIRMWARE_VERSION

const char* WIFI_SSID     = manifest::WIFI_SSID;
const char* WIFI_PASSWORD = manifest::WIFI_PASSWORD;
const char* MQTT_SERVER   = manifest::MQTT_SERVER;
const int   MQTT_PORT     = manifest::MQTT_PORT;

#define DIR_PIN              manifest::DIR_PIN
#define PWM_PIN              manifest::PWM_PIN
#define LIMIT_OPEN           manifest::LIMIT_OPEN
#define LIMIT_CLOSED         manifest::LIMIT_CLOSED
#define LIMIT_CLOSED_THRESHOLD manifest::LIMIT_CLOSED_THRESHOLD

#define MOTOR_SPEED          manifest::MOTOR_SPEED
#define DIR_OPEN             manifest::DIR_OPEN
#define DIR_CLOSE            manifest::DIR_CLOSE
#define PWM_FREQ             manifest::PWM_FREQ
#define PWM_RESOLUTION       manifest::PWM_RESOLUTION
#define LIMIT_DEBOUNCE_MS    manifest::LIMIT_DEBOUNCE_MS
#define MOTOR_TIMEOUT_MS     manifest::MOTOR_TIMEOUT_MS
#define CLOSE_OVERRUN_MS     manifest::CLOSE_OVERRUN_MS

// ============================================
// MQTT TOPICS (built at runtime)
// ============================================
String mqtt_topic_command;
String mqtt_topic_status;
String mqtt_topic_log;
String mqtt_topic_limit;

// ============================================
// DOOR STATES
// ============================================
enum DoorState {
  DOOR_CLOSED,
  DOOR_OPEN,
  DOOR_OPENING,
  DOOR_CLOSING,
  DOOR_STOPPED,
  EMERGENCY_STOP
};

DoorState currentState = DOOR_STOPPED;
DoorState previousState = DOOR_STOPPED;

// ============================================
// LIMIT SWITCH DEBOUNCING
// ============================================
bool rawLimitOpen = false;
bool rawLimitClosed = false;
bool debouncedLimitOpen = false;
bool debouncedLimitClosed = false;
unsigned long limitOpenStableTime = 0;
unsigned long limitClosedStableTime = 0;
bool lastRawLimitOpen = false;
bool lastRawLimitClosed = false;

// ============================================
// TIMING
// ============================================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastHeartbeat = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastMqttReconnect = 0;
unsigned long bootTime = 0;
unsigned long motorStartTime = 0;
unsigned long closeOverrunStart = 0;  // non-zero = overrun phase active

const unsigned long HEARTBEAT_INTERVAL     = manifest::HEARTBEAT_INTERVAL;
const unsigned long WIFI_CHECK_INTERVAL    = manifest::WIFI_CHECK_INTERVAL;
const unsigned long MQTT_RECONNECT_INTERVAL = manifest::MQTT_RECONNECT_INTERVAL;

bool systemReady = false;
char mqttLogBuffer[256];

// ============================================
// FUNCTION PROTOTYPES
// ============================================
void setup_wifi();
void setup_mqtt();
void mqtt_callback(char* topic, byte* payload, unsigned int length);
void mqtt_reconnect();
void send_heartbeat();
void send_status(const char* status);
void check_connections();
void mqttLog(const char* message);
void mqttLogf(const char* format, ...);
void startOpening();
void startClosing();
void stopMotor();
void checkLimitSwitches();
void checkTimeout();
void publishLimitEvent(const char* event);
const char* getStateString(DoorState state);

// ============================================
// MQTT LOGGING
// ============================================
void mqttLog(const char* message) {
  Serial.println(message);
  if (mqtt.connected()) {
    mqtt.publish(mqtt_topic_log.c_str(), message, false);
  }
}

void mqttLogf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(mqttLogBuffer, sizeof(mqttLogBuffer), format, args);
  va_end(args);
  mqttLog(mqttLogBuffer);
}

void publishLimitEvent(const char* event) {
  mqttLogf("[LIMIT] %s", event);
  if (mqtt.connected()) {
    mqtt.publish(mqtt_topic_limit.c_str(), event, false);
  }
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n============================================");
  Serial.println("   JUNGLE DOOR CONTROLLER - ESP32-S3");
  Serial.println("============================================");
  Serial.print("Firmware: ");
  Serial.println(FIRMWARE_VERSION);
  Serial.println("============================================\n");

  // Build MQTT topics
  mqtt_topic_command = "MermaidsTale/" + String(DEVICE_NAME) + "/command";
  mqtt_topic_status  = "MermaidsTale/" + String(DEVICE_NAME) + "/status";
  mqtt_topic_log     = "MermaidsTale/" + String(DEVICE_NAME) + "/log";
  mqtt_topic_limit   = "MermaidsTale/" + String(DEVICE_NAME) + "/limit";

  // Motor pins
  pinMode(DIR_PIN, OUTPUT);
  digitalWrite(DIR_PIN, LOW);
  ledcAttach(PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(PWM_PIN, 0);

  // Limit switch pins
  pinMode(LIMIT_OPEN, INPUT_PULLUP);
  pinMode(LIMIT_CLOSED, INPUT);

  stopMotor();

  // Read initial limit switch states
  rawLimitOpen = (digitalRead(LIMIT_OPEN) == LOW);
  rawLimitClosed = (analogRead(LIMIT_CLOSED) < LIMIT_CLOSED_THRESHOLD);
  debouncedLimitOpen = rawLimitOpen;
  debouncedLimitClosed = rawLimitClosed;
  lastRawLimitOpen = rawLimitOpen;
  lastRawLimitClosed = rawLimitClosed;
  limitOpenStableTime = millis();
  limitClosedStableTime = millis();

  // Determine initial door position from limit switches
  if (debouncedLimitOpen && debouncedLimitClosed) {
    currentState = EMERGENCY_STOP;
    Serial.println("[INIT] EMERGENCY — both limit switches active!");
  } else if (debouncedLimitOpen) {
    currentState = DOOR_OPEN;
    Serial.println("[INIT] Door is OPEN");
  } else if (debouncedLimitClosed) {
    currentState = DOOR_CLOSED;
    Serial.println("[INIT] Door is CLOSED");
  } else {
    currentState = DOOR_STOPPED;
    Serial.println("[INIT] Door position UNKNOWN (no limit active)");
  }
  previousState = currentState;

  setup_wifi();
  setup_mqtt();
  bootTime = millis();
  systemReady = true;

  if (mqtt.connected()) {
    send_status("ONLINE");
    mqttLogf("[READY] State: %s", getStateString(currentState));
  }
}

// ============================================
// MAIN LOOP
// ============================================
void loop() {
  check_connections();

  if (mqtt.connected()) {
    mqtt.loop();
  }

  send_heartbeat();
  checkLimitSwitches();
  checkTimeout();

  // Handle close overrun: motor continues 500ms after CLOSED limit hit
  if (closeOverrunStart > 0 && millis() - closeOverrunStart >= CLOSE_OVERRUN_MS) {
    mqttLog("[OVERRUN] Close overrun complete — stopping motor");
    stopMotor();
    closeOverrunStart = 0;
    currentState = DOOR_CLOSED;
    send_status("CLOSED");
  }

  // Publish state changes
  if (currentState != previousState) {
    mqttLogf("[STATE] %s -> %s", getStateString(previousState), getStateString(currentState));
    previousState = currentState;
    send_status(getStateString(currentState));
  }

  delay(10);
}

// ============================================
// WIFI
// ============================================
void setup_wifi() {
  Serial.print("[WIFI] Connecting to ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" Connected!");
    Serial.print("[WIFI] IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" FAILED — will retry in background");
  }
}

void check_connections() {
  if (millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) {
    mqtt_reconnect();
  }
}

// ============================================
// MQTT
// ============================================
void setup_mqtt() {
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqtt_callback);
  mqtt.setBufferSize(512);

  if (WiFi.status() == WL_CONNECTED) {
    mqtt_reconnect();
  }
}

void mqtt_reconnect() {
  if (millis() - lastMqttReconnect < MQTT_RECONNECT_INTERVAL) return;
  lastMqttReconnect = millis();

  Serial.print("[MQTT] Connecting...");

  if (mqtt.connect(DEVICE_NAME)) {
    Serial.println(" Connected!");
    mqtt.subscribe(mqtt_topic_command.c_str());
    send_status("ONLINE");
    mqttLogf("[MQTT] Connected — State: %s", getStateString(currentState));
  } else {
    Serial.print(" Failed (rc=");
    Serial.print(mqtt.state());
    Serial.println(")");
  }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();
  message.toUpperCase();

  mqttLogf("[MQTT] Received: '%s'", message.c_str());

  // --- Watchtower standard commands ---
  if (message == "PING") {
    send_status("PONG");
    return;
  }

  if (message == "STATUS") {
    unsigned long uptime = (millis() - bootTime) / 1000;
    String info = String(getStateString(currentState));
    info += "|UP:" + String(uptime) + "s";
    info += "|RSSI:" + String(WiFi.RSSI());
    info += "|VER:" + String(FIRMWARE_VERSION);
    info += "|LIMIT_OPEN:" + String(debouncedLimitOpen ? "ACTIVE" : "CLEAR");
    info += "|LIMIT_CLOSED:" + String(debouncedLimitClosed ? "ACTIVE" : "CLEAR");
    send_status(info.c_str());
    return;
  }

  if (message == "PUZZLE_RESET") {
    mqttLog("[CMD] PUZZLE_RESET — stopping motor, resetting to CLOSED");
    stopMotor();
    closeOverrunStart = 0;
    currentState = DOOR_CLOSED;
    send_status("CLOSED");
    return;
  }

  if (message == "RESET" || message == "REBOOT" || message == "RESTART") {
    mqttLog("[CMD] RESET received");
    send_status("RESETTING");
    stopMotor();
    delay(500);
    ESP.restart();
    return;
  }

  // --- Door commands ---
  if (message == "OPEN") {
    if (currentState == EMERGENCY_STOP) {
      mqttLog("[CMD] Cannot OPEN — emergency stop active");
    } else if (currentState == DOOR_OPEN) {
      mqttLog("[CMD] Already OPEN");
      send_status("ALREADY_OPEN");
    } else if (currentState == DOOR_OPENING) {
      mqttLog("[CMD] Already OPENING");
    } else {
      startOpening();
    }
    return;
  }

  if (message == "CLOSE") {
    if (currentState == EMERGENCY_STOP) {
      mqttLog("[CMD] Cannot CLOSE — emergency stop active");
    } else if (currentState == DOOR_CLOSED) {
      mqttLog("[CMD] Already CLOSED");
      send_status("ALREADY_CLOSED");
    } else if (currentState == DOOR_CLOSING) {
      mqttLog("[CMD] Already CLOSING");
    } else {
      startClosing();
    }
    return;
  }

  if (message == "STOP") {
    mqttLog("[CMD] STOP");
    stopMotor();
    closeOverrunStart = 0;
    currentState = DOOR_STOPPED;
    send_status("STOPPED");
    return;
  }

  mqttLogf("[CMD] Unknown: %s", message.c_str());
}

void send_status(const char* status) {
  if (!mqtt.connected()) return;
  mqtt.publish(mqtt_topic_status.c_str(), status, false);
}

void send_heartbeat() {
  if (!mqtt.connected()) return;
  if (millis() - lastHeartbeat < HEARTBEAT_INTERVAL) return;
  lastHeartbeat = millis();

  unsigned long uptime = (millis() - bootTime) / 1000;
  String hb = "HEARTBEAT:";
  hb += getStateString(currentState);
  hb += ":UP" + String(uptime) + "s";
  hb += ":RSSI" + String(WiFi.RSSI());
  mqtt.publish(mqtt_topic_status.c_str(), hb.c_str(), false);
}

// ============================================
// MOTOR CONTROL
// ============================================
void startOpening() {
  if (debouncedLimitOpen) {
    currentState = DOOR_OPEN;
    stopMotor();
    return;
  }

  mqttLog("[MOTOR] Opening");
  closeOverrunStart = 0;
  currentState = DOOR_OPENING;
  motorStartTime = millis();
  digitalWrite(DIR_PIN, DIR_OPEN);
  ledcWrite(PWM_PIN, MOTOR_SPEED);
  send_status("OPENING");
}

void startClosing() {
  if (debouncedLimitClosed) {
    currentState = DOOR_CLOSED;
    stopMotor();
    return;
  }

  mqttLog("[MOTOR] Closing");
  closeOverrunStart = 0;
  currentState = DOOR_CLOSING;
  motorStartTime = millis();
  digitalWrite(DIR_PIN, DIR_CLOSE);
  ledcWrite(PWM_PIN, MOTOR_SPEED);
  send_status("CLOSING");
}

void stopMotor() {
  ledcWrite(PWM_PIN, 0);
}

// ============================================
// TIMEOUT — 6 second safety cutoff
// ============================================
void checkTimeout() {
  if (currentState != DOOR_OPENING && currentState != DOOR_CLOSING) return;
  if (millis() - motorStartTime < MOTOR_TIMEOUT_MS) return;

  mqttLogf("[TIMEOUT] Motor ran %dms without limit switch — stopping", MOTOR_TIMEOUT_MS);
  stopMotor();
  closeOverrunStart = 0;
  currentState = DOOR_STOPPED;
  send_status("TIMEOUT");
}

// ============================================
// LIMIT SWITCH HANDLING
// ============================================
void checkLimitSwitches() {
  rawLimitOpen = (digitalRead(LIMIT_OPEN) == LOW);
  rawLimitClosed = (analogRead(LIMIT_CLOSED) < LIMIT_CLOSED_THRESHOLD);

  // Debounce OPEN limit
  if (rawLimitOpen != lastRawLimitOpen) {
    lastRawLimitOpen = rawLimitOpen;
    limitOpenStableTime = millis();
  } else if (rawLimitOpen != debouncedLimitOpen) {
    if (millis() - limitOpenStableTime >= LIMIT_DEBOUNCE_MS) {
      debouncedLimitOpen = rawLimitOpen;
      publishLimitEvent(debouncedLimitOpen ? "LIMIT_OPEN_HIT" : "LIMIT_OPEN_CLEAR");
    }
  }

  // Debounce CLOSED limit
  if (rawLimitClosed != lastRawLimitClosed) {
    lastRawLimitClosed = rawLimitClosed;
    limitClosedStableTime = millis();
  } else if (rawLimitClosed != debouncedLimitClosed) {
    if (millis() - limitClosedStableTime >= LIMIT_DEBOUNCE_MS) {
      debouncedLimitClosed = rawLimitClosed;
      publishLimitEvent(debouncedLimitClosed ? "LIMIT_CLOSED_HIT" : "LIMIT_CLOSED_CLEAR");
    }
  }

  // EMERGENCY: both switches active = something is very wrong
  if (debouncedLimitOpen && debouncedLimitClosed) {
    if (currentState != EMERGENCY_STOP) {
      mqttLog("[EMERGENCY] Both limit switches active — stopping motor!");
      stopMotor();
      closeOverrunStart = 0;
      currentState = EMERGENCY_STOP;
      send_status("EMERGENCY");
    }
    return;
  }

  // Act on limit switches during movement
  if (currentState == DOOR_OPENING && debouncedLimitOpen) {
    mqttLog("[LIMIT] Door reached OPEN position");
    stopMotor();
    currentState = DOOR_OPEN;
  }

  if (currentState == DOOR_CLOSING && debouncedLimitClosed && closeOverrunStart == 0) {
    mqttLog("[LIMIT] Door reached CLOSED position — running 500ms overrun");
    closeOverrunStart = millis();
    // Motor keeps running — loop() will stop it after CLOSE_OVERRUN_MS
  }
}

// ============================================
// UTILITY
// ============================================
const char* getStateString(DoorState state) {
  switch (state) {
    case DOOR_CLOSED:    return "CLOSED";
    case DOOR_OPEN:      return "OPEN";
    case DOOR_OPENING:   return "OPENING";
    case DOOR_CLOSING:   return "CLOSING";
    case DOOR_STOPPED:   return "STOPPED";
    case EMERGENCY_STOP: return "EMERGENCY";
    default:             return "UNKNOWN";
  }
}

