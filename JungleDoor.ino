/*
 * ============================================
 * ALCHEMY ESCAPE ROOM - JUNGLE DOOR CONTROLLER
 * ESP32-S3 VERSION WITH FULL MQTT INTEGRATION
 * ============================================
 *
 * Automatic Sliding Door System
 * ESP32-S3 + Cytron MD13S Motor Driver
 *
 * STANDARD COMMANDS (handled automatically):
 *   PING   -> responds with "PONG"
 *   STATUS -> responds with current door state
 *   RESET  -> restarts the microcontroller
 *
 * DOOR COMMANDS:
 *   OPEN   -> opens the door
 *   CLOSE  -> closes the door
 *   STOP   -> emergency stop
 *
 * MQTT TOPICS:
 *   Subscribe: MermaidsTale/JungleDoor/command
 *   Publish:   MermaidsTale/JungleDoor/status   (state changes, heartbeat)
 *   Publish:   MermaidsTale/JungleDoor/log      (mirrored serial output)
 *   Publish:   MermaidsTale/JungleDoor/limit    (limit switch events)
 *
 * LIMIT SWITCH MESSAGES (on /limit topic):
 *   LIMIT_OPEN_HIT      -> Door reached fully open position
 *   LIMIT_CLOSED_HIT    -> Door reached fully closed position
 *   LIMIT_OPEN_CLEAR    -> Door moved away from open limit
 *   LIMIT_CLOSED_CLEAR  -> Door moved away from closed limit
 *
 * ============================================
 */

#include <WiFi.h>
#include <PubSubClient.h>

// ============================================
// DEVICE CONFIGURATION — Sourced from MANIFEST.h
// ============================================
#include "MANIFEST.h"

// Bridge: all code below still uses these names, but values come from manifest
#define DEVICE_NAME       manifest::DEVICE_NAME
#define FIRMWARE_VERSION  manifest::FIRMWARE_VERSION

const char* WIFI_SSID     = manifest::WIFI_SSID;
const char* WIFI_PASSWORD = manifest::WIFI_PASSWORD;

const char* MQTT_SERVER   = manifest::MQTT_SERVER;
const int   MQTT_PORT     = manifest::MQTT_PORT;

// ============================================
// PIN DEFINITIONS — Sourced from MANIFEST.h
// ============================================
#define DIR_PIN         manifest::DIR_PIN
#define PWM_PIN         manifest::PWM_PIN
#define LIMIT_OPEN      manifest::LIMIT_OPEN
#define LIMIT_CLOSED    manifest::LIMIT_CLOSED

#define LIMIT_CLOSED_THRESHOLD manifest::LIMIT_CLOSED_THRESHOLD
#define STATUS_LED_OPEN    manifest::STATUS_LED_OPEN    // UNUSED — flagged for removal
#define STATUS_LED_CLOSED  manifest::STATUS_LED_CLOSED  // UNUSED — flagged for removal
#define STATUS_LED_MOVING  manifest::STATUS_LED_MOVING  // UNUSED — flagged for removal

// Motor Configuration — Sourced from MANIFEST.h
#define MOTOR_SPEED     manifest::MOTOR_SPEED
#define DIR_OPEN        manifest::DIR_OPEN
#define DIR_CLOSE       manifest::DIR_CLOSE

// PWM Configuration — Sourced from MANIFEST.h
#define PWM_FREQ        manifest::PWM_FREQ
#define PWM_RESOLUTION  manifest::PWM_RESOLUTION

// Debounce — Sourced from MANIFEST.h
#define LIMIT_DEBOUNCE_MS manifest::LIMIT_DEBOUNCE_MS

// Door timing — Sourced from MANIFEST.h
#define DOOR_RAMP_UP_MS     manifest::DOOR_RAMP_UP_MS
#define DOOR_FULL_SPEED_MS  manifest::DOOR_FULL_SPEED_MS
#define DOOR_RAMP_DOWN_MS   manifest::DOOR_RAMP_DOWN_MS
#define DOOR_TOTAL_TIME_MS  manifest::DOOR_TOTAL_TIME_MS

// ============================================
// MQTT TOPICS
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
// GLOBAL VARIABLES
// ============================================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastHeartbeat = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastMqttReconnect = 0;
unsigned long bootTime = 0;
unsigned long lastBlinkTime = 0;
unsigned long motorStartTime = 0;

const unsigned long HEARTBEAT_INTERVAL = manifest::HEARTBEAT_INTERVAL;
const unsigned long WIFI_CHECK_INTERVAL = manifest::WIFI_CHECK_INTERVAL;
const unsigned long MQTT_RECONNECT_INTERVAL = manifest::MQTT_RECONNECT_INTERVAL;
const unsigned long BLINK_INTERVAL = manifest::BLINK_INTERVAL;

bool blinkState = false;
bool systemReady = false;

// Buffer for MQTT log messages
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
void updateStatusLEDs();
const char* getStateString(DoorState state);
void publishLimitEvent(const char* event);

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

// ============================================
// LIMIT SWITCH EVENT PUBLISHER
// ============================================
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

  Serial.println("\n");
  Serial.println("============================================");
  Serial.println("   JUNGLE DOOR CONTROLLER - ESP32-S3");
  Serial.println("============================================");
  Serial.print("Device Name: ");
  Serial.println(DEVICE_NAME);
  Serial.print("Firmware:    ");
  Serial.println(FIRMWARE_VERSION);
  Serial.print("Compiled:    ");
  Serial.print(__DATE__);
  Serial.print(" ");
  Serial.println(__TIME__);
  Serial.println("============================================\n");

  // Build MQTT topics
  mqtt_topic_command = "MermaidsTale/" + String(DEVICE_NAME) + "/command";
  mqtt_topic_status = "MermaidsTale/" + String(DEVICE_NAME) + "/status";
  mqtt_topic_log = "MermaidsTale/" + String(DEVICE_NAME) + "/log";
  mqtt_topic_limit = "MermaidsTale/" + String(DEVICE_NAME) + "/limit";

  Serial.print("[MQTT] Command topic: ");
  Serial.println(mqtt_topic_command);
  Serial.print("[MQTT] Status topic:  ");
  Serial.println(mqtt_topic_status);
  Serial.print("[MQTT] Log topic:     ");
  Serial.println(mqtt_topic_log);
  Serial.print("[MQTT] Limit topic:   ");
  Serial.println(mqtt_topic_limit);
  Serial.println();

  // Initialize motor control pins
  pinMode(DIR_PIN, OUTPUT);
  digitalWrite(DIR_PIN, LOW);

  // Setup PWM for motor speed control
  ledcAttach(PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(PWM_PIN, 0);
  Serial.println("[INIT] Motor pins configured (DIR + PWM)");

  // Initialize limit switch pins
  pinMode(LIMIT_OPEN, INPUT_PULLUP);  // Mechanical switch with pullup
  pinMode(LIMIT_CLOSED, INPUT);        // Laser beam - analog read
  Serial.println("[INIT] Limit switches configured (OPEN=digital pullup, CLOSED=analog)");

  // Initialize status LED pins
  pinMode(STATUS_LED_OPEN, OUTPUT);
  pinMode(STATUS_LED_CLOSED, OUTPUT);
  pinMode(STATUS_LED_MOVING, OUTPUT);
  digitalWrite(STATUS_LED_OPEN, LOW);
  digitalWrite(STATUS_LED_CLOSED, LOW);
  digitalWrite(STATUS_LED_MOVING, LOW);
  Serial.println("[INIT] Status LEDs configured");

  // Stop motor initially
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
  
  Serial.print("[INIT] LIMIT_CLOSED raw analog: ");
  Serial.println(analogRead(LIMIT_CLOSED));

  // Set initial state based on limit switches
  if (debouncedLimitOpen && debouncedLimitClosed) {
    Serial.println("[WARNING] BOTH limit switches triggered - check wiring!");
    currentState = DOOR_STOPPED;
  } else if (debouncedLimitClosed) {
    currentState = DOOR_CLOSED;
    Serial.println("[INIT] Door is CLOSED (limit switch active)");
  } else if (debouncedLimitOpen) {
    currentState = DOOR_OPEN;
    Serial.println("[INIT] Door is OPEN (limit switch active)");
  } else {
    currentState = DOOR_STOPPED;
    Serial.println("[INIT] Door position UNKNOWN (no limit active)");
  }
  previousState = currentState;
  updateStatusLEDs();

  // Connect to WiFi
  setup_wifi();

  // Setup MQTT
  setup_mqtt();

  // Record boot time
  bootTime = millis();

  // Mark system ready
  systemReady = true;

  // Send initial status via MQTT
  if (mqtt.connected()) {
    send_status("ONLINE");
    mqttLogf("[READY] System initialized - State: %s", getStateString(currentState));
  }

  Serial.println("\n============================================");
  Serial.println("   DEVICE READY");
  Serial.println("============================================\n");
}

// ============================================
// MAIN LOOP
// ============================================
void loop() {
  // Check WiFi and MQTT connections
  check_connections();

  // Process MQTT messages
  if (mqtt.connected()) {
    mqtt.loop();
  }

  // Send periodic heartbeat
  send_heartbeat();

  // Check limit switches with debouncing
  checkLimitSwitches();

  // Handle motor ramping for opening and closing
  if (currentState == DOOR_OPENING || currentState == DOOR_CLOSING) {
    unsigned long elapsed = millis() - motorStartTime;
    int speed = 0;
    
    if (elapsed < DOOR_RAMP_UP_MS) {
      // Ramping up
      speed = map(elapsed, 0, DOOR_RAMP_UP_MS, 0, MOTOR_SPEED);
    } else if (elapsed < (DOOR_TOTAL_TIME_MS - DOOR_RAMP_DOWN_MS)) {
      // Full speed
      speed = MOTOR_SPEED;
    } else if (elapsed < DOOR_TOTAL_TIME_MS) {
      // Ramping down
      unsigned long rampDownElapsed = elapsed - (DOOR_TOTAL_TIME_MS - DOOR_RAMP_DOWN_MS);
      speed = map(rampDownElapsed, 0, DOOR_RAMP_DOWN_MS, MOTOR_SPEED, 0);
    } else {
      // Done
      speed = 0;
      stopMotor();
      if (currentState == DOOR_OPENING) {
        mqttLog("[TIMEOUT] Door open complete");
        currentState = DOOR_OPEN;
        send_status("OPEN");
      } else {
        mqttLog("[TIMEOUT] Door close complete");
        currentState = DOOR_CLOSED;
        send_status("CLOSED");
      }
    }
    
    // Apply speed if still moving
    if (currentState == DOOR_OPENING || currentState == DOOR_CLOSING) {
      ledcWrite(PWM_PIN, speed);
    }
  }

  // Handle blinking animation for moving states
  if (currentState == DOOR_OPENING || currentState == DOOR_CLOSING) {
    if (millis() - lastBlinkTime > BLINK_INTERVAL) {
      blinkState = !blinkState;
      lastBlinkTime = millis();
      digitalWrite(STATUS_LED_MOVING, blinkState ? HIGH : LOW);
    }
  }

  // Update status LEDs
  updateStatusLEDs();

  // Publish state changes
  if (currentState != previousState) {
    mqttLogf("[STATE] %s -> %s", getStateString(previousState), getStateString(currentState));
    previousState = currentState;

    if (mqtt.connected()) {
      send_status(getStateString(currentState));
    }
  }

  delay(10);
}

// ============================================
// WIFI FUNCTIONS
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
    Serial.print("[WIFI] IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println(" FAILED!");
    Serial.println("[WIFI] Will retry in background...");
  }
}

void check_connections() {
  if (millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Disconnected - reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) {
    mqtt_reconnect();
  }
}

// ============================================
// MQTT FUNCTIONS
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
  if (millis() - lastMqttReconnect < MQTT_RECONNECT_INTERVAL) {
    return;
  }
  lastMqttReconnect = millis();

  Serial.print("[MQTT] Connecting to ");
  Serial.print(MQTT_SERVER);
  Serial.print(":");
  Serial.print(MQTT_PORT);
  Serial.print("...");

  if (mqtt.connect(DEVICE_NAME)) {
    Serial.println(" Connected!");

    mqtt.subscribe(mqtt_topic_command.c_str());
    Serial.print("[MQTT] Subscribed to: ");
    Serial.println(mqtt_topic_command);

    send_status("ONLINE");
    mqttLogf("[MQTT] Connected - Current state: %s", getStateString(currentState));

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

  mqttLogf("[MQTT] Received: %s -> '%s'", topic, message.c_str());

  // Standard commands
  if (message == "PING") {
    send_status("PONG");
    mqttLog("[CMD] PING -> PONG");
    return;
  }

  if (message == "STATUS") {
    unsigned long uptime = (millis() - bootTime) / 1000;
    String fullStatus = String(getStateString(currentState));
    fullStatus += "|UP:" + String(uptime) + "s";
    fullStatus += "|RSSI:" + String(WiFi.RSSI());
    fullStatus += "|VER:" + String(FIRMWARE_VERSION);
    fullStatus += "|LIMIT_OPEN:" + String(debouncedLimitOpen ? "ACTIVE" : "CLEAR");
    fullStatus += "|LIMIT_CLOSED:" + String(debouncedLimitClosed ? "ACTIVE" : "CLEAR");

    send_status(fullStatus.c_str());
    mqttLogf("[CMD] STATUS -> %s", fullStatus.c_str());
    return;
  }

  if (message == "RESET" || message == "REBOOT" || message == "RESTART") {
    mqttLog("[CMD] RESET command received!");
    send_status("RESETTING");
    stopMotor();
    delay(500);
    mqttLog("[SYSTEM] Restarting...");
    delay(100);
    ESP.restart();
    return;
  }

  // Door control commands
  if (message == "OPEN") {
    if (currentState == DOOR_OPEN) {
      mqttLog("[CMD] Door already OPEN");
      send_status("ALREADY_OPEN");
    } else if (currentState == DOOR_OPENING) {
      mqttLog("[CMD] Door already OPENING");
    } else {
      mqttLog("[CMD] OPEN command via MQTT");
      startOpening();
    }
    return;
  }

  if (message == "CLOSE") {
    if (currentState == DOOR_CLOSED) {
      mqttLog("[CMD] Door already CLOSED");
      send_status("ALREADY_CLOSED");
    } else if (currentState == DOOR_CLOSING) {
      mqttLog("[CMD] Door already CLOSING");
    } else {
      mqttLog("[CMD] CLOSE command via MQTT");
      startClosing();
    }
    return;
  }

  if (message == "STOP") {
    mqttLog("[CMD] STOP command via MQTT");
    stopMotor();
    currentState = DOOR_STOPPED;
    send_status("STOPPED");
    return;
  }

  mqttLogf("[CMD] Unknown command: %s", message.c_str());
}

void send_status(const char* status) {
  if (!mqtt.connected()) return;
  mqtt.publish(mqtt_topic_status.c_str(), status, false);
}

void send_heartbeat() {
  if (!mqtt.connected()) return;

  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = millis();

    unsigned long uptime = (millis() - bootTime) / 1000;
    String heartbeat = "HEARTBEAT:";
    heartbeat += getStateString(currentState);
    heartbeat += ":UP" + String(uptime) + "s";
    heartbeat += ":RSSI" + String(WiFi.RSSI());

    mqtt.publish(mqtt_topic_status.c_str(), heartbeat.c_str(), false);
    mqttLogf("[HEARTBEAT] %s", heartbeat.c_str());
  }
}

// ============================================
// MOTOR CONTROL FUNCTIONS
// ============================================
void startOpening() {
  if (debouncedLimitOpen) {
    mqttLog("[INFO] Already at OPEN limit");
    currentState = DOOR_OPEN;
    stopMotor();
    return;
  }

  mqttLog("[MOTOR] Opening door...");
  currentState = DOOR_OPENING;
  motorStartTime = millis();
  digitalWrite(DIR_PIN, DIR_OPEN);
  ledcWrite(PWM_PIN, 0);  // Ramp will handle speed
  blinkState = true;
  lastBlinkTime = millis();

  if (mqtt.connected()) {
    send_status("OPENING");
  }
}

void startClosing() {
  if (debouncedLimitClosed) {
    mqttLog("[INFO] Already at CLOSED limit");
    currentState = DOOR_CLOSED;
    stopMotor();
    return;
  }

  mqttLog("[MOTOR] Closing door...");
  currentState = DOOR_CLOSING;
  motorStartTime = millis();
  digitalWrite(DIR_PIN, DIR_CLOSE);
  ledcWrite(PWM_PIN, 0);  // Ramp will handle speed
  blinkState = true;
  lastBlinkTime = millis();

  if (mqtt.connected()) {
    send_status("CLOSING");
  }
}

void stopMotor() {
  ledcWrite(PWM_PIN, 0);
}

// ============================================
// LIMIT SWITCH HANDLING WITH DEBOUNCING
// ============================================
void checkLimitSwitches() {
  rawLimitOpen = (digitalRead(LIMIT_OPEN) == LOW);
  rawLimitClosed = (analogRead(LIMIT_CLOSED) < LIMIT_CLOSED_THRESHOLD);

  // Debounce OPEN limit switch
  if (rawLimitOpen != lastRawLimitOpen) {
    lastRawLimitOpen = rawLimitOpen;
    limitOpenStableTime = millis();
  } else if (rawLimitOpen != debouncedLimitOpen) {
    if (millis() - limitOpenStableTime >= LIMIT_DEBOUNCE_MS) {
      debouncedLimitOpen = rawLimitOpen;
      
      if (debouncedLimitOpen) {
        publishLimitEvent("LIMIT_OPEN_HIT");
        if (currentState == DOOR_OPENING) {
          mqttLog("[LIMIT] Door reached OPEN position");
          stopMotor();
          currentState = DOOR_OPEN;
        }
      } else {
        publishLimitEvent("LIMIT_OPEN_CLEAR");
      }
    }
  }

  // Debounce CLOSED limit switch
  if (rawLimitClosed != lastRawLimitClosed) {
    lastRawLimitClosed = rawLimitClosed;
    limitClosedStableTime = millis();
  } else if (rawLimitClosed != debouncedLimitClosed) {
    if (millis() - limitClosedStableTime >= LIMIT_DEBOUNCE_MS) {
      debouncedLimitClosed = rawLimitClosed;
      
      if (debouncedLimitClosed) {
        publishLimitEvent("LIMIT_CLOSED_HIT");
        if (currentState == DOOR_CLOSING) {
          mqttLog("[LIMIT] Door reached CLOSED position");
          stopMotor();
          currentState = DOOR_CLOSED;
        }
      } else {
        publishLimitEvent("LIMIT_CLOSED_CLEAR");
      }
    }
  }

  // Safety check: stop motor if limit hit while moving
  if (currentState == DOOR_OPENING && debouncedLimitOpen) {
    stopMotor();
    currentState = DOOR_OPEN;
  }

  if (currentState == DOOR_CLOSING && debouncedLimitClosed) {
    stopMotor();
    currentState = DOOR_CLOSED;
  }
}

// ============================================
// STATUS LED CONTROL
// ============================================
void updateStatusLEDs() {
  switch (currentState) {
    case DOOR_OPEN:
      digitalWrite(STATUS_LED_OPEN, HIGH);
      digitalWrite(STATUS_LED_CLOSED, LOW);
      digitalWrite(STATUS_LED_MOVING, LOW);
      break;

    case DOOR_CLOSED:
      digitalWrite(STATUS_LED_OPEN, LOW);
      digitalWrite(STATUS_LED_CLOSED, HIGH);
      digitalWrite(STATUS_LED_MOVING, LOW);
      break;

    case DOOR_OPENING:
    case DOOR_CLOSING:
      digitalWrite(STATUS_LED_OPEN, LOW);
      digitalWrite(STATUS_LED_CLOSED, LOW);
      // STATUS_LED_MOVING handled by blink in loop()
      break;

    case EMERGENCY_STOP:
      digitalWrite(STATUS_LED_OPEN, HIGH);
      digitalWrite(STATUS_LED_CLOSED, HIGH);
      digitalWrite(STATUS_LED_MOVING, HIGH);
      break;

    default:
      digitalWrite(STATUS_LED_OPEN, LOW);
      digitalWrite(STATUS_LED_CLOSED, LOW);
      digitalWrite(STATUS_LED_MOVING, LOW);
      break;
  }
}

// ============================================
// UTILITY FUNCTIONS
// ============================================
const char* getStateString(DoorState state) {
  switch (state) {
    case DOOR_CLOSED:     return "CLOSED";
    case DOOR_OPEN:       return "OPEN";
    case DOOR_OPENING:    return "OPENING";
    case DOOR_CLOSING:    return "CLOSING";
    case DOOR_STOPPED:    return "STOPPED";
    case EMERGENCY_STOP:  return "EMERGENCY";
    default:              return "UNKNOWN";
  }
}
