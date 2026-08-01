//===================================
//          MACROS
//===================================

#include <WiFi.h>
#include <PubSubClient.h>

#define VERSION "2.1.1"

#define GAME_NAME "MermaidsTale"
#define PROP_NAME "JungleDoor"

#define MQTT_TOPIC_COMMAND  "MermaidsTale/JungleDoor/command"
#define MQTT_TOPIC_STATUS   "MermaidsTale/JungleDoor/status"
#define MQTT_TOPIC_LOG      "MermaidsTale/JungleDoor/log"
#define MQTT_TOPIC_MESSAGE  "MermaidsTale/JungleDoor/message"
#define MQTT_TOPIC_SYSTEM   "MermaidsTale/JungleDoor/system"
#define MQTT_TOPIC_STATE    "MermaidsTale/JungleDoor/state"
#define MQTT_TOPIC_SAFETY   "MermaidsTale/JungleDoor/safety"
#define MQTT_TOPIC_VERSION  "MermaidsTale/JungleDoor/version"
#define MQTT_TOPIC_DEVICE   "MermaidsTale/JungleDoor/device"
#define MQTT_TOPIC_INFO     "MermaidsTale/JungleDoor/info"
#define MQTT_TOPIC_LASTERROR "MermaidsTale/JungleDoor/lastError"
#define MQTT_TOPIC_UPTIME   "MermaidsTale/JungleDoor/uptime"

#define DIR_PIN 4
#define DIR_OPEN LOW
#define DIR_CLOSE HIGH

#define PWM_PIN 6
#define PWM_FREQ 5000
#define PWM_RESOLUTION 8

#define LIMIT_OPEN_PIN 8                //digital device on this pin
#define LIMIT_CLOSE_PIN 7               //analog device on this pin

#define MOTOR_TIMEOUT_MS 10000
#define MOTOR_SPEED 255
#define LIMIT_CLOSE_THRESHOLD 3600

//===================================
//          GLOBAL VARIABLES
//===================================
const unsigned long heartBeatPulse = 5 * 1000UL;
unsigned long lastTime = 0;
unsigned long bootTime = 0;
String lastError = "NONE";
String currentState = "IDLE";

// --- Retained-command replay guard (added 2026-07-09) ---------------------
// The door was auto-opening on every boot: WatchTower wire logs show the
// board publish "state IDLE" and flip to "state OPENING" 49ms later on every
// reconnect. The firmware never commands motion at boot -- the broker was
// replaying a RETAINED payload (e.g. "OPEN") stuck on .../command to our
// fresh subscription. Since every brownout reboot re-triggered it, the door
// drifted open each time the board browned out.
// Fix: for CMD_GRACE_MS after each (re)subscribe, ignore motion/RESET
// commands (a live command cannot know we just booted, so anything arriving
// that early can only be a retained replay), keep the door where it
// physically is, and erase the retained copy so it can never replay again.
// "Stay put" was chosen over NVS-restore/default-closed on purpose: any
// boot-time motor drive would re-fire after every brownout reboot and could
// loop the brownout itself. M3/operator re-commands the door explicitly.
const unsigned long CMD_GRACE_MS = 3000UL;
unsigned long cmdGraceUntil = 0;



WiFiClient espClient;
PubSubClient mqttClient(espClient);

// WiFi credentials
const char* WIFI_SSID = "AlchemyGuest";
const char* WIFI_PASS = "VoodooVacation5601";

// MQTT broker
const char* MQTT_SERVER = "10.1.10.115";
const int MQTT_PORT = 1883;


bool limitOpenTriggered = false;
bool limitCloseTriggered = false;
bool motorDir = false;

//===================================
//          WIFI (NETWORK)
//===================================
void setupWiFi() {
  delay(1000);
  Serial.println("*********** WIFI ***********");
  Serial.print("Connecting to SSID: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID,WIFI_PASS);

  while(WiFi.status() != WL_CONNECTED){
    delay(100);
    Serial.print("-");
  }
  Serial.println("\nConnected.");
}
//===================================
//          MQTT FUNCTIONS
//===================================
void connectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");

    String clientId = PROP_NAME;
    clientId += "_";
    clientId += String(random(0xffff), HEX);

    // Connect with LWT (Last Will and Testament) for offline detection
    if (mqttClient.connect(clientId.c_str(), NULL, NULL, MQTT_TOPIC_STATUS, 1, true, "OFFLINE")) {
      Serial.println("connected!");

      // Subscribe to command topic
      mqttClient.subscribe(MQTT_TOPIC_COMMAND);

      // Arm the retained-replay guard: broker delivers retained /command
      // payloads immediately after SUBACK, so anything inside this window
      // is treated as a stale replay, not a live command.
      cmdGraceUntil = millis() + CMD_GRACE_MS;

      // Publish retained status topics
      mqttClient.publish(MQTT_TOPIC_STATUS, "ONLINE", true);
      mqttClient.publish(MQTT_TOPIC_VERSION, VERSION, true);
      mqttClient.publish(MQTT_TOPIC_DEVICE, PROP_NAME, true);
      mqttClient.publish(MQTT_TOPIC_STATE, currentState.c_str(), true);
      mqttClient.publish(MQTT_TOPIC_SAFETY, "OK", true);
      mqttClient.publish(MQTT_TOPIC_LASTERROR, lastError.c_str(), true);

      mqttLogf("%s v%s online", PROP_NAME, VERSION);

    } else {
      Serial.printf("failed (rc=%d), retrying in 5s\n", mqttClient.state());
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char topicBuf[128];
  strncpy(topicBuf,topic,sizeof(topicBuf)-1);
  topicBuf[sizeof(topicBuf)-1] = '\0';

  char message[128];
  if(length >= sizeof(message)){
    length = sizeof(message) - 1;
  }

  memcpy(message,payload,length);
  message[length] = '\0';

  char * msg = message;
  while(*msg == ' ' || *msg == '\r' || *msg == '\n')
    msg++;
  char * end = msg + strlen(msg) -1;
  while(end > msg && (*end == ' ' || *end == 't' || *end == '\r' || *end == '\n')){
    *end = '\0';
    end--;
  }

  Serial.printf("[MQTT] Received on %s: %s\n", topicBuf,msg);

  if(strcmp(topicBuf,MQTT_TOPIC_COMMAND) != 0){
    return;
  }

  // Empty payload = our own retained-erase publish (or another guard's).
  // Silently ignore so it doesn't log as an unknown command.
  if(*msg == '\0'){
    return;
  }

  // Retained-replay guard: swallow motion/RESET commands arriving within
  // the boot/reconnect grace window and erase the retained copy. See the
  // comment at CMD_GRACE_MS for why the door must stay put on boot.
  // (RESET is guarded too -- a retained RESET on /command reboot-loops the
  // board, a failure mode already seen on other props in this room.)
  if((strcmp(msg,"OPEN") == 0 || strcmp(msg,"CLOSE") == 0 || strcmp(msg,"RESET") == 0)
      && millis() < cmdGraceUntil){
    mqttClient.publish(MQTT_TOPIC_COMMAND, "", true);   // erase retained payload
    mqttLogf("[GUARD] Ignored '%s' during boot grace (retained replay) - door stays put", msg);
    return;
  }

  if(strcmp(msg,"PING") == 0){
    mqttClient.publish(MQTT_TOPIC_COMMAND,"PONG");
    Serial.println("[MQTT] PING -> PONG");
    return;
  }
  if(strcmp(msg,"STATUS") == 0){
    const char* state = "READY";
    mqttClient.publish(MQTT_TOPIC_COMMAND,state);
    Serial.printf("[MQTT] STATUS -> %s\n",state);
    return;
  }
  if(strcmp(msg,"RESET") == 0){
    mqttClient.publish(MQTT_TOPIC_COMMAND,"OK");
    Serial.println("[MQTT] RESET -> Rebooting...");
    delay(100);
    ESP.restart();
    return;
  }

  if (strcmp(msg,"PROMPTSTATUS") == 0) {
    promptStatus();
    return;
  }
  if (strcmp(msg,"OPEN") == 0) {
    openDoor();
    return;
  }
  if (strcmp(msg,"CLOSE") == 0) {
    closeDoor();
    return;
  }
  if (strcmp(msg,"STOP") == 0) {
    stopDoor();
    return;
  }
  Serial.printf("[MQTT] Unknown command: %s\n", msg);
}

void setupMQTT() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);  // Increase if needed
}

void mqttLogf(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  mqttClient.publish(MQTT_TOPIC_LOG, buffer);
  Serial.println(buffer);
}
void publishUptime() {
  unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
  unsigned long hours = uptimeSeconds / 3600;
  unsigned long minutes = (uptimeSeconds % 3600) / 60;
  unsigned long seconds = uptimeSeconds % 60;

  char uptimeStr[32];
  snprintf(uptimeStr, sizeof(uptimeStr), "%lu:%02lu:%02lu", hours, minutes, seconds);
  mqttClient.publish(MQTT_TOPIC_UPTIME, uptimeStr, true);
}

void publishState(const char* state) {
  currentState = state;
  mqttClient.publish(MQTT_TOPIC_STATE, state, true);
}

void publishError(const char* error) {
  lastError = error;
  mqttClient.publish(MQTT_TOPIC_LASTERROR, error, true);
}

void heartBeat(){
  unsigned long currentTime = millis();
  if(!(currentTime - lastTime > heartBeatPulse))
    return;
  lastTime = currentTime;
  // Announce we're online with retained message
  mqttClient.publish(MQTT_TOPIC_STATUS, "ONLINE", true);
  publishUptime();

  promptStatus();
}
//===================================
//        GENERAL FUNCTIONS
//===================================
void openDoor(){
  //check if door is already open
  //if not, set the direction to open
  //and begin opening the door
  if(!checkLimitSwitch(DIR_OPEN)){
    mqttClient.publish(MQTT_TOPIC_MESSAGE,"Open Limit Switch: Triggered.");
    publishState("OPEN");
    return;
  }
  publishState("OPENING");
  motorDir = DIR_OPEN;              //update global motor direction variable
  digitalWrite(DIR_PIN,DIR_OPEN);   //set direction to open
  ledcWrite(PWM_PIN,MOTOR_SPEED);   //start opening
}

void closeDoor(){
  //check if the door is already close,
  //if not, set the direction to close
  //and begin opening the door

  if(!checkLimitSwitch(DIR_CLOSE)){
    mqttClient.publish(MQTT_TOPIC_MESSAGE,"Close Limit Switch: Triggered.");
    publishState("CLOSED");
    return;
  }
  publishState("CLOSING");
  motorDir = DIR_CLOSE;             //update global motor direction variable
  digitalWrite(DIR_PIN,DIR_CLOSE);  //set direction to close
  ledcWrite(PWM_PIN,MOTOR_SPEED);   //start closing
}

void stopDoor(){
  ledcWrite(PWM_PIN,0);
  publishState("STOPPED");
}

bool checkLimitSwitch(bool dir){
  // Edge-triggered publish: the "limit reached" message is sent only when
  // the switch transitions from not-triggered -> triggered. Previously this
  // function fired the publish every loop iteration while the limit was
  // held, producing ~2.9M duplicate MQTT messages per hour under the AI
  // Character session log. limitOpenTriggered / limitCloseTriggered now
  // gate the publish rather than just being tracked and ignored.
  bool result;
  if(!dir){
    result = digitalRead(LIMIT_OPEN_PIN); //Open limit is triggered low
                                          //make sure that the open conditions are met when triggered
    if(!result){
      ledcWrite(PWM_PIN,0);  // Stop motor directly to avoid state conflict
      if(!limitOpenTriggered){
        mqttClient.publish(MQTT_TOPIC_MESSAGE,"Open limit is reached. Door is stopped.");
        publishState("OPEN");
      }
      limitOpenTriggered = true;
      limitCloseTriggered = false;
    } else {
      limitOpenTriggered = false;
    }

  }else{
    result =  !(analogRead(LIMIT_CLOSE_PIN) < LIMIT_CLOSE_THRESHOLD); //Close limit is triggered with smaller value than threshold
                                                                      //make sure that the close conditions are met when triggereed
    if(!result){
      ledcWrite(PWM_PIN,0);  // Stop motor directly to avoid state conflict
      if(!limitCloseTriggered){
        mqttClient.publish(MQTT_TOPIC_MESSAGE,"Close limit is reached. Door is closed.");
        publishState("CLOSED");
      }
      limitCloseTriggered = true;
      limitOpenTriggered = false;
    } else {
      limitCloseTriggered = false;
    }

  }
  return result;
}

void promptStatus(){
  bool openLimitState = digitalRead(LIMIT_OPEN_PIN);
  int closeLimitValue = analogRead(LIMIT_CLOSE_PIN);
  bool closeLimitState = (closeLimitValue < LIMIT_CLOSE_THRESHOLD);


  char buffer[7];          // Enough for -32768 and null terminator
  itoa(closeLimitValue, buffer, 10); // Convert to decimal (base 10)
  const char* cVal = buffer;

  String topic1 = String(MQTT_TOPIC_SYSTEM) + "/OpenLimitState";
  mqttClient.publish(topic1.c_str(),(openLimitState ? "Not triggered":"Triggered" ));
  String topic2 = String(MQTT_TOPIC_SYSTEM) + "/CloseLimitState";
  mqttClient.publish(topic2.c_str(),(closeLimitState ? "Triggered" : "Not triggered"));
  String topic3 = String(MQTT_TOPIC_SYSTEM) + "/CloseLimitValue";
  mqttClient.publish(topic3.c_str(),cVal);


  String topic4 = String(MQTT_TOPIC_SYSTEM) + "/DoorState";
  if(!openLimitState)
    mqttClient.publish(topic4.c_str(),"Opened.");
  else if (closeLimitState)
    mqttClient.publish(topic4.c_str(),"Closed.");
  else if(openLimitState && !closeLimitState)
    mqttClient.publish(topic4.c_str(),"Partially opened or closed.");
}

//I/O setup
void setupIO(){
  pinMode(DIR_PIN,OUTPUT);
  pinMode(PWM_PIN,OUTPUT);
  digitalWrite(PWM_PIN,LOW);      // motor hard-off at boot (2026-07-09): never
                                  // leave the MD13S PWM input undriven/floating
                                  // between pinMode and ledcAttach

  pinMode(LIMIT_OPEN_PIN,INPUT_PULLUP);
  pinMode(LIMIT_CLOSE_PIN,INPUT);

  //initially configure to open
  digitalWrite(DIR_PIN,DIR_OPEN);

  Serial.println("IO pins initialization complete.");
}
//System initialization
void _init(){
  Serial.begin(9600);
  bootTime = millis();
  //IO setup
  setupIO();
  //WiFi setup
  setupWiFi();
  //Mqtt server setup
  setupMQTT();
  //motor control configuration
  ledcAttach(PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
}
void program(){
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  heartBeat();
  checkLimitSwitch(motorDir);
}
//===================================
//          MAIN SETUP
//===================================
void setup(){
  _init();
}
//===================================
//          MAIN LOOP
//===================================
void loop(){
  program();
}
