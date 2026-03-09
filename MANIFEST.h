/**
 * ============================================================================
 *  ALCHEMY ESCAPE ROOMS — FIRMWARE MANIFEST
 * ============================================================================
 *
 *  THIS FILE IS THE SINGLE SOURCE OF TRUTH.
 *
 *  It serves two masters simultaneously:
 *
 *    1. THE COMPILER — Every constant the firmware needs (pins, IPs, ports,
 *       thresholds, timing) is defined here as real C++ code. The firmware
 *       #includes this file and uses these values directly.
 *
 *    2. THE GRIMOIRE PARSER — A Python script running on M3 at 6 AM reads
 *       this file as plain text and extracts values tagged with @FIELD_NAME
 *       in the comments. Those values populate the WatchTower Grimoire
 *       device registry, wiring reference, and operations manual.
 *
 *  Because both systems read from the same lines, the documentation can
 *  never drift from the firmware. Change a pin number here, and the Grimoire
 *  updates automatically. There is no second file to keep in sync.
 *
 *  RULES:
 *    1. Every field marked [REQUIRED] must be filled in before deployment.
 *    2. Update this file FIRST when changing hardware, pins, or topics.
 *    3. The 6 AM parser looks for @TAG patterns — don't rename them.
 *    4. Descriptive-only sections (operations, quirks) are pure comments.
 *       Constants sections are real code + comment tags on the same line.
 *    5. This file is the sole source of configuration values — the .ino
 *       file should reference these constants, not hardcode its own.
 *
 *  LAST UPDATED: 2026-02-12
 *  MANIFEST VERSION: 2.0
 * ============================================================================
 */

#pragma once
#include <cstdint>

// ============================================================================
//  SECTION 1 — IDENTITY
// ============================================================================
//
// @MANIFEST:IDENTITY
// @PROP_NAME:        JungleDoor
// @INSTANCE_COUNT:   1
//
// @DESCRIPTION:      Secret automatic sliding door that transitions players
//                    from the Pirate Ship room into the Jungle room. The door
//                    is disguised as part of the ship wall — players do not
//                    know it exists until it opens. A Cytron MD13S motor driver
//                    controls a DC motor that slides the door along a track.
//                    Two limit switches define the open and closed positions:
//                    a mechanical switch at the open end, and a laser beam
//                    sensor at the closed end (hidden to preserve the secret).
//                    The door responds to OPEN, CLOSE, and STOP commands via
//                    MQTT, with smooth ramped motor acceleration and
//                    deceleration for theatrical effect.
//
// @ROOM:             Pirate Ship (transitions to Jungle)
// @BOARD:            ESP32-S3
// @FRAMEWORK:        Arduino IDE (.ino)
// @REPO:             https://github.com/Alchemy-Escape-Rooms-Inc/JungleDoor
// @BUILD_STATUS:     INSTALLED
// @CODE_HEALTH:      GOOD
// @WATCHTOWER:       COMPLIANT
// @END:IDENTITY

namespace manifest {

// ── Device Identity ─────────────────────────────────────────────────────────
inline constexpr const char* DEVICE_NAME    = "JungleDoor";       // @DEVICE_NAME  (MQTT client ID + topic base)
inline constexpr const char* FIRMWARE_VERSION = "2.7.0";          // @FIRMWARE_VERSION


// ============================================================================
//  SECTION 2 — NETWORK CONFIGURATION
// ============================================================================
// @MANIFEST:NETWORK

// ── WiFi ────────────────────────────────────────────────────────────────────
inline constexpr const char* WIFI_SSID     = "AlchemyGuest";      // @WIFI_SSID
inline constexpr const char* WIFI_PASSWORD = "VoodooVacation5601"; // @WIFI_PASS

// ── MQTT Broker ─────────────────────────────────────────────────────────────
inline constexpr const char* MQTT_SERVER   = "10.1.10.115";       // @BROKER_IP
inline constexpr int         MQTT_PORT     = 1883;                // @BROKER_PORT
// MQTT Client ID: "JungleDoor" (matches DEVICE_NAME)

// ── Heartbeat ───────────────────────────────────────────────────────────────
inline constexpr unsigned long HEARTBEAT_INTERVAL = 30000;        // @HEARTBEAT_MS  (30 seconds)

//  ── TOPIC MAP ──────────────────────────────────────────────────────────────
//  Topics are built dynamically from DEVICE_NAME at runtime:
//    "MermaidsTale/" + DEVICE_NAME + "/{suffix}"
//
//  SUBSCRIPTIONS:
//  @SUBSCRIBE:  MermaidsTale/JungleDoor/command    | All commands (standard + door control)
//
//  PUBLICATIONS:
//  @PUBLISH:  MermaidsTale/JungleDoor/status       | State changes + heartbeat    | retain:no
//  @PUBLISH:  MermaidsTale/JungleDoor/log          | Mirrored serial output       | retain:no
//  @PUBLISH:  MermaidsTale/JungleDoor/limit        | Limit switch events          | retain:no
//
//  SUPPORTED COMMANDS (via /command topic):
//  @COMMAND:  PING          | Responds PONG on /status topic          | Health check
//  @COMMAND:  STATUS        | Sends state, uptime, RSSI, version     | Full diagnostic
//  @COMMAND:  RESET         | Stops motor, reboots ESP32              | Also accepts REBOOT, RESTART
//  @COMMAND:  OPEN          | Opens the door (ramp up → full → ramp down)
//  @COMMAND:  CLOSE         | Closes the door (ramp up → full → ramp down)
//  @COMMAND:  STOP          | Emergency stop — kills motor immediately
//
//  LIMIT SWITCH EVENTS (published on /limit topic):
//  @LIMIT_EVENT:  LIMIT_OPEN_HIT      | Door reached fully open position
//  @LIMIT_EVENT:  LIMIT_CLOSED_HIT    | Door reached fully closed position
//  @LIMIT_EVENT:  LIMIT_OPEN_CLEAR    | Door moved away from open limit
//  @LIMIT_EVENT:  LIMIT_CLOSED_CLEAR  | Door moved away from closed limit
//
//  STATUS MESSAGES (published on /status topic):
//  @STATUS_MSG:  ONLINE          | Sent on boot and MQTT reconnect
//  @STATUS_MSG:  OPENING         | Door motor started, opening direction
//  @STATUS_MSG:  CLOSING         | Door motor started, closing direction
//  @STATUS_MSG:  OPEN            | Door reached open position (limit or timeout)
//  @STATUS_MSG:  CLOSED          | Door reached closed position (limit or timeout)
//  @STATUS_MSG:  STOPPED         | Motor stopped via STOP command
//  @STATUS_MSG:  ALREADY_OPEN    | OPEN command received but door already open
//  @STATUS_MSG:  ALREADY_CLOSED  | CLOSE command received but door already closed
//  @STATUS_MSG:  RESETTING       | RESET command received, about to reboot
//  @STATUS_MSG:  HEARTBEAT:...   | Periodic heartbeat with state, uptime, RSSI
//
// @END:NETWORK


// ============================================================================
//  SECTION 3 — PIN CONFIGURATION
// ============================================================================
// @MANIFEST:PINS

// ── Motor Driver (Cytron MD13S) ─────────────────────────────────────────────
inline constexpr int DIR_PIN = 4;                                 // @PIN:DIR    | MD13S DIR — direction control (HIGH=open, LOW=close)
inline constexpr int PWM_PIN = 5;                                 // @PIN:PWM    | MD13S PWM — speed control (0-255 via ledcWrite)

// ── Limit Switches ──────────────────────────────────────────────────────────
inline constexpr int LIMIT_OPEN   = 8;                            // @PIN:LIMIT_OPEN   | Mechanical switch, INPUT_PULLUP, active LOW
inline constexpr int LIMIT_CLOSED = 19;                           // @PIN:LIMIT_CLOSED | Laser beam sensor, analog read, active below threshold

// ── Status LEDs (UNUSED — flagged for removal) ─────────────────────────────
inline constexpr int STATUS_LED_OPEN   = 21;                      // @PIN:LED_OPEN   | UNUSED — can be reclaimed
inline constexpr int STATUS_LED_CLOSED = 22;                      // @PIN:LED_CLOSED | UNUSED — can be reclaimed
inline constexpr int STATUS_LED_MOVING = 23;                      // @PIN:LED_MOVING | UNUSED — can be reclaimed

// @END:PINS


// ============================================================================
//  SECTION 4 — MOTOR CONFIGURATION
// ============================================================================
// @MANIFEST:MOTOR

inline constexpr int MOTOR_SPEED     = 150;                       // @MOTOR:SPEED      | PWM duty 0-255 (150 ≈ 59%)
inline constexpr int DIR_OPEN        = HIGH;                      // @MOTOR:DIR_OPEN   | HIGH = opening direction
inline constexpr int DIR_CLOSE       = LOW;                       // @MOTOR:DIR_CLOSE  | LOW = closing direction

// ── PWM Configuration ───────────────────────────────────────────────────────
inline constexpr int PWM_FREQ        = 5000;                      // @PWM:FREQ       | 5kHz PWM frequency
inline constexpr int PWM_RESOLUTION  = 8;                         // @PWM:RESOLUTION | 8-bit (0-255)

// ── Door Movement Timing ────────────────────────────────────────────────────
inline constexpr int DOOR_RAMP_UP_MS    = 500;                    // @DOOR:RAMP_UP    | 0.5s acceleration to full speed
inline constexpr int DOOR_FULL_SPEED_MS = 3000;                   // @DOOR:FULL_SPEED | 3s at full speed
inline constexpr int DOOR_RAMP_DOWN_MS  = 500;                    // @DOOR:RAMP_DOWN  | 0.5s deceleration to stop
inline constexpr int DOOR_TOTAL_TIME_MS = 4000;                   // @DOOR:TOTAL_TIME | 4s total movement window

// @END:MOTOR


// ============================================================================
//  SECTION 5 — SENSOR THRESHOLDS
// ============================================================================
// @MANIFEST:THRESHOLDS

// ── Laser Beam Sensor (Closed Limit) ────────────────────────────────────────
inline constexpr int LIMIT_CLOSED_THRESHOLD = 3600;               // @THRESHOLD:LASER | Analog value below 3600 = beam blocked (door present)
//  Reference: ~3200 (2.58V) when blocked, ~4095 (3.3V) when clear.
//  Threshold at ~2.9V gives reliable detection margin.

// ── Debounce ────────────────────────────────────────────────────────────────
inline constexpr int LIMIT_DEBOUNCE_MS = 150;                     // @DEBOUNCE:LIMIT  | 150ms debounce for both limit switches

// @END:THRESHOLDS


// ============================================================================
//  SECTION 6 — TIMING CONSTANTS
// ============================================================================
// @MANIFEST:TIMING

inline constexpr unsigned long WIFI_CHECK_INTERVAL     = 30000;   // @TIMING:WIFI_CHECK     | Check WiFi connection every 30s
inline constexpr unsigned long MQTT_RECONNECT_INTERVAL = 5000;    // @TIMING:MQTT_RECONNECT | Retry MQTT connection every 5s
inline constexpr unsigned long BLINK_INTERVAL          = 300;     // @TIMING:BLINK          | LED blink rate during movement (UNUSED)

// @END:TIMING

} // namespace manifest


// ============================================================================
//  SECTION 7 — COMPONENTS
// ============================================================================
//
// @MANIFEST:COMPONENTS
//
// @COMPONENT:  Cytron MD13S Motor Driver
//   @PURPOSE:  Controls the DC motor that slides the door along its track
//   @DETAIL:   2-pin interface (DIR + PWM). DIR sets direction, PWM sets speed
//              via ESP32 LEDC peripheral at 5kHz/8-bit resolution. Motor runs
//              at 59% duty (150/255) with smooth ramp-up and ramp-down
//              for theatrical door movement.
//
// @COMPONENT:  DC Sliding Door Motor
//   @PURPOSE:  Physically moves the door panel along a track
//   @DETAIL:   Driven by MD13S. Opens on DIR=HIGH, closes on DIR=LOW.
//              Total travel time approximately 4 seconds with ramping.
//
// @COMPONENT:  Mechanical Limit Switch (Open Position)
//   @PURPOSE:  Detects when door has fully opened
//   @DETAIL:   Pin 8, INPUT_PULLUP, active LOW. Located at the open end
//              of the door track. Visible from the Jungle side (secret
//              already revealed at this point).
//
// @COMPONENT:  Laser Beam Sensor (Closed Position)
//   @PURPOSE:  Detects when door has fully closed
//   @DETAIL:   Pin 19, analog read. Laser beam breaks when door panel is
//              present (blocked ≈ 2.58V/3200, clear ≈ 3.3V/4095, threshold
//              at 3600). Hidden to preserve the secret door illusion — no
//              visible mechanical switch on the player-facing wall.
//
// @COMPONENT:  Status LEDs (x3)
//   @PURPOSE:  UNUSED — originally for diagnostic indication
//   @DETAIL:   Pins 21 (open), 22 (closed), 23 (moving/blink). Code exists
//              but LEDs are not installed. Pins can be reclaimed for future
//              features. Flagged for code removal.
//
// @END:COMPONENTS


// ============================================================================
//  SECTION 8 — OPERATIONS
// ============================================================================
//
// @MANIFEST:OPERATIONS
//
//  ── PHYSICAL LOCATION ──────────────────────────────────────────────────────
//
// @LOCATION:  The Shattic (Ship's Attic) — the attic space above the Pirate
//             Ship room. The ESP32 and Cytron MD13S motor driver are both
//             located here, connected to the door motor and limit switches
//             below.
//
//  ── RESET PROCEDURES ───────────────────────────────────────────────────────
//
// @RESET:SOFTWARE
//   Send "RESET" (or "REBOOT" or "RESTART") to MermaidsTale/JungleDoor/command
//   Device stops motor, publishes "RESETTING" on /status, then reboots
//   After reboot: reconnects WiFi, reconnects MQTT, reads limit switches
//   to determine initial door position, publishes ONLINE
//   Expected recovery time: 10-15 seconds
//
// @RESET:HARDWARE
//   The ESP32 and motor driver are located in the Shattic (attic above the
//   Pirate Ship room). Access the Shattic, locate the ESP32, disconnect and
//   reconnect power. After power-on, monitor MermaidsTale/JungleDoor/status
//   for ONLINE message. The door will read its limit switches on boot to
//   determine whether it's currently open, closed, or in an unknown position.
//
//  ── DOOR OPERATION ─────────────────────────────────────────────────────────
//
// @OPERATION:OPEN
//   Send "OPEN" to MermaidsTale/JungleDoor/command
//   Motor ramps up over 0.5s, runs at PWM 150 for 3s, ramps down over 0.5s
//   Publishes "OPENING" immediately, then "OPEN" when complete
//   Stops early if open limit switch triggers
//   Falls back to 4-second timer if limit switch does not trigger
//
// @OPERATION:CLOSE
//   Send "CLOSE" to MermaidsTale/JungleDoor/command
//   Same ramping profile as OPEN but in reverse direction
//   Publishes "CLOSING" immediately, then "CLOSED" when complete
//   Stops early if closed limit switch triggers
//   Falls back to 4-second timer if limit switch does not trigger
//
// @OPERATION:EMERGENCY_STOP
//   Send "STOP" to MermaidsTale/JungleDoor/command
//   Motor PWM immediately set to 0 — no ramp-down
//   Publishes "STOPPED" on /status
//   Door remains in whatever position it stopped at
//
//  ── TEST PROCEDURE ─────────────────────────────────────────────────────────
//
// @TEST:STEP1  Send PING to /command → expect PONG on /status (confirms MQTT)
// @TEST:STEP2  Send STATUS to /command → expect state, uptime, RSSI, version, limit states
// @TEST:STEP3  Send OPEN to /command → door should slide open, expect OPENING then OPEN
// @TEST:STEP4  Send CLOSE to /command → door should slide closed, expect CLOSING then CLOSED
// @TEST:STEP5  Send STOP during movement → motor should stop immediately
// @TEST:STEP6  Monitor /limit topic during open/close → check for LIMIT_OPEN_HIT / LIMIT_CLOSED_HIT
// @TEST:STEP7  If no limit events fire, confirm timer-based stop is working correctly
//
//  ── KNOWN QUIRKS ───────────────────────────────────────────────────────────
//
//
// @QUIRK:DEVICE_NAME_SPACE  [RESOLVED]
//   PREVIOUSLY: DEVICE_NAME was "Jungle Door" with a space, creating
//   MQTT topics like "MermaidsTale/Jungle Door/command".
//   FIXED: DEVICE_NAME is now "JungleDoor" (no space) via MANIFEST.h.
//   ACTION REQUIRED: Verify that WatchTower, BAC, and all external systems
//   subscribe/publish to the corrected topics (no space). If any system
//   still uses the old "Jungle Door" topic, commands will never reach
//   this device — this is a likely cause of "not deploying" if heartbeats
//   are visible but commands have no effect.
//
// @QUIRK:NO_WATCHDOG
//   Unlike the Cannons, this firmware does not implement a hardware watchdog
//   timer. If the main loop hangs (e.g., WiFi blocking, MQTT stall), the
//   device will not auto-recover. A manual RESET command or hardware power
//   cycle is required. Consider adding esp_task_wdt for future versions.
//
//
// @QUIRK:MOTOR_STOPS_ON_RESET
//   The RESET command explicitly calls stopMotor() before rebooting. This is
//   important — if the door is mid-travel when someone sends RESET, the motor
//   will stop before the ESP32 reboots. The door may end up in a partial
//   position. On reboot, if neither limit switch is active, the state will
//   be DOOR_STOPPED (unknown position).
//
// @QUIRK:UNUSED_LEDS
//   Three status LED pins (21, 22, 23) are defined and initialized in code
//   but no LEDs are physically installed. The LED control code runs every
//   loop iteration updating pins that connect to nothing. Safe but wasteful.
//   Flagged for removal — would free 3 GPIO pins and simplify the loop.
//
// @END:OPERATIONS


// ============================================================================
//  SECTION 9 — DEPENDENCIES
// ============================================================================
//
// @MANIFEST:DEPENDENCIES
//
// @LIB:  WiFi              | ESP32 WiFi driver          | Built-in
// @LIB:  PubSubClient      | MQTT client                | v2.8+
//
// @END:DEPENDENCIES


// ============================================================================
//  SECTION 10 — WIRING SUMMARY
// ============================================================================
//
// @MANIFEST:WIRING
//
//   ESP32-S3 Pin 4  (DIR) ─────── Cytron MD13S DIR input
//   ESP32-S3 Pin 5  (PWM) ─────── Cytron MD13S PWM input
//
//   ESP32-S3 Pin 8  ───────────── Mechanical Limit Switch (OPEN position)
//                                 Switch connects pin to GND when triggered
//                                 INPUT_PULLUP, active LOW
//
//   ESP32-S3 Pin 19 ───────────── Laser Beam Sensor (CLOSED position)
//                                 Analog read: ~3200 blocked, ~4095 clear
//                                 Threshold: 3600
//
//   ESP32-S3 Pin 21 ───────────── UNUSED (was Status LED Open)
//   ESP32-S3 Pin 22 ───────────── UNUSED (was Status LED Closed)
//   ESP32-S3 Pin 23 ───────────── UNUSED (was Status LED Moving)
//
//   Cytron MD13S POWER ────────── Motor power supply (in Shattic)
//   Cytron MD13S MOTOR OUT ────── DC sliding door motor
//
//   ESP32-S3 Power ────────────── USB in the Shattic (Ship's Attic)
//
//   Physical Location: The Shattic — attic space above the Pirate Ship room
//
// @END:WIRING
