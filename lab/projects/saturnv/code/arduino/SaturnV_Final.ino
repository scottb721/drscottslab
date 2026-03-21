#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <DFRobotDFPlayerMini.h>

// ===== Wi-Fi =====
const char* ssid     = "Gargoyle";
const char* password = "garg4810";

// ===== MQTT (Home Assistant) =====
const char* mqtt_server = "192.168.50.5";
const int   mqtt_port   = 1883;
const char* mqtt_user   = "scottb721";
const char* mqtt_pass   = "pw";

// ===== OTA =====
const char* otaHostname = "saturnv-tower";

// ===== Motor / Endstops / Mister pins =====
#define AIN1        25
#define AIN2        26
#define PWMA        33
#define END_RETRACT 32   // active-low endstop (pull to GND when hit)
#define END_EXTEND  34   // active-low endstop (pull to GND when hit)
#define MISTER_PIN  23

// If your MOSFET board is active-LOW, flip these two
const bool MISTER_ON  = HIGH;
const bool MISTER_OFF = LOW;

// ===== PWM (ESP32 LEDC) =====
const int PWM_FREQ = 2000;   // 2 kHz
const int PWM_RES  = 10;     // 10-bit (0..1023)
int pwmDuty        = 900;    // ~30% (adjust to taste)

// ===== MQTT topics =====
const char* T_CMD        = "saturnv/cmd";          // extend | retract | stop
const char* T_RUN_STATE  = "saturnv/run_state";    // extend | retract | stopped
const char* T_STATUS     = "saturnv/status";       // online | still online | errors
const char* T_MIST_CMD   = "saturnv/mist";         // on | off
const char* T_MIST_STATE = "saturnv/mist_state";   // on | off
const char* T_AUDIO_CMD   = "saturnv/audio/play";   // payload: "play"


// ===== Endstop debounce =====
struct Debounced {
  uint8_t pin;
  bool    rawPrev   = true;
  uint32_t tChange  = 0;
  bool    active    = false;   // true when endstop is pressed (active-low)
};
Debounced esRetract{END_RETRACT};
Debounced esExtend{ END_EXTEND };

// ===== Run mode =====
enum Mode { STOPPED, EXTENDING, RETRACTING };
volatile Mode mode = STOPPED;

// ===== MQTT client =====
WiFiClient      espClient;
PubSubClient    mqtt(espClient);

// ===== DFPlayer (audio) =====
// Wiring: ESP32 TX2 (GPIO17) -> DF RX (via ~1k series), ESP32 RX2 (GPIO16) <- DF TX
HardwareSerial        MP3(2);
DFRobotDFPlayerMini   df;

const int AUDIO_TRIG_PIN = 14;       // pull LOW to trigger audio
static bool     audioTrigPrev       = true; // INPUT_PULLUP: HIGH idle
static uint32_t audioTrigLastChange = 0;
static uint32_t audioLastFire       = 0;
const uint16_t  AUDIO_DEBOUNCE_MS   = 40;
const uint16_t  AUDIO_COOLDOWN_MS   = 300;

// ---------- Motor helpers ----------
inline void motorStop() {
  ledcWrite(PWMA, 0);
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, LOW);
}

inline void motorExtend() {           // move away from retract endstop
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, LOW);
  ledcWrite(PWMA, pwmDuty);
}

inline void motorRetract() {          // move towards retract endstop
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, HIGH);
  ledcWrite(PWMA, pwmDuty);
}

// ---------- Endstop debounce ----------
void updateOne(Debounced& d) {
  bool raw = digitalRead(d.pin);              // INPUT_PULLUP: HIGH=not pressed, LOW=pressed
  uint32_t now = millis();
  if (raw != d.rawPrev) {
    d.tChange = now;
    d.rawPrev = raw;
  }
  // 30 ms debounce
  if ((now - d.tChange) > 30) {
    d.active = (raw == LOW);
  }
}

void updateEndstops() {
  updateOne(esRetract);
  updateOne(esExtend);
}

// ---------- MQTT helpers ----------
inline void publishRunState(const char* state, bool retain=false) {
  mqtt.publish(T_RUN_STATE, state, retain);
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len + 1);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.toLowerCase();

  if (strcmp(topic, T_CMD) == 0) {
    if (msg == "extend") {
      updateEndstops();
      if (esExtend.active) {
        mode = STOPPED; motorStop(); publishRunState("stopped", true);
        mqtt.publish(T_STATUS, "extend blocked (endstop active)");
      } else {
        mode = EXTENDING; publishRunState("extend", true);
      }
    } else if (msg == "retract") {
      updateEndstops();
      if (esRetract.active) {
        mode = STOPPED; motorStop(); publishRunState("stopped", true);
        mqtt.publish(T_STATUS, "retract blocked (endstop active)");
      } else {
        mode = RETRACTING; publishRunState("retract", true);
      }
    } else if (msg == "stop") {
      mode = STOPPED; motorStop(); publishRunState("stopped", true);
    } else if (msg.startsWith("pwm:")) {
      int v = constrain(msg.substring(4).toInt(), 0, 1023);
      pwmDuty = v;
      mqtt.publish(T_STATUS, ("pwm=" + String(pwmDuty)).c_str());
    }

  } else if (strcmp(topic, T_MIST_CMD) == 0) {
    if (msg == "on")  { digitalWrite(MISTER_PIN, MISTER_ON);  mqtt.publish(T_MIST_STATE, "on",  true); }
    if (msg == "off") { digitalWrite(MISTER_PIN, MISTER_OFF); mqtt.publish(T_MIST_STATE, "off", true); }

  // 👉 Audio control over MQTT (same topic, two payloads)
  } else if (strcmp(topic, T_AUDIO_CMD) == 0) {
    if (msg == "play") {
      playTrack1();
      mqtt.publish(T_STATUS, "audio play triggered");
    } else if (msg == "stop") {
      df.stop();
      mqtt.publish(T_STATUS, "audio stop triggered");
    }
  }
} // <— make sure this brace exists!



// ---------- Wi-Fi / OTA / MQTT ----------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    if (millis() - t0 > 15000) break;
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(otaHostname);
  ArduinoOTA.onStart([](){ mqtt.publish(T_STATUS, "ota start"); });
  ArduinoOTA.onEnd([](){ mqtt.publish(T_STATUS, "ota end"); });
  ArduinoOTA.begin();
}

void connectMQTT() {
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(onMqtt);
  while (!mqtt.connected()) {
    if (mqtt.connect("SaturnV-01", mqtt_user, mqtt_pass, T_STATUS, 0, true, "offline")) {
      mqtt.publish(T_STATUS, "online", true);
      mqtt.subscribe(T_CMD);
      mqtt.subscribe(T_MIST_CMD);
      mqtt.subscribe(T_AUDIO_CMD);
      publishRunState("stopped", true);
      mqtt.publish(T_MIST_STATE, "off", true);
    } else {
      delay(2000);
    }
  }
}

// ---------- DFPlayer bring-up & play ----------
static bool dfInit() {
  MP3.begin(9600, SERIAL_8N1, 16, 17);                // RX=16 (from DF TX), TX=17 (to DF RX)
  if (!df.begin(MP3, /*isACK=*/false, /*doReset=*/true)) return false;
  delay(400);
  df.outputDevice(DFPLAYER_DEVICE_SD);
  delay(300);
  df.volume(28);                                      // 0..30 (adjust to taste)
  delay(120);
  return true;
}

static void playTrack1() {
  df.stop();
  delay(100);
  df.play(1);   // plays /0001.mp3 in root (or /MP3/0001.mp3 if present)
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);

  // Pins
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(MISTER_PIN, OUTPUT);
  digitalWrite(MISTER_PIN, MISTER_OFF);

  pinMode(END_RETRACT, INPUT_PULLUP);
  pinMode(END_EXTEND,  INPUT_PULLUP);

  // PWM (ESP32 v3)
  ledcAttach(PWMA, PWM_FREQ, PWM_RES);
  ledcWrite(PWMA, 0);
  motorStop();

  // Net services
  connectWiFi();
  setupOTA();
  connectMQTT();

  // Audio
  pinMode(AUDIO_TRIG_PIN, INPUT_PULLUP);  // HIGH idle; pull LOW to play
  if (!dfInit()) {
    Serial.println("[AUDIO] DF init failed — check 5V/GND/TX2/RX2/SD.");
    mqtt.publish(T_STATUS, "audio init failed");
  } else {
    mqtt.publish(T_STATUS, "audio ready");
  }

  Serial.println("Ready: cmd=extend|retract|stop, mist=on|off, GPIO14 to GND plays 0001.mp3");
}

void loop() {
  ArduinoOTA.handle();
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  updateEndstops();

  // Auto-stop if limit hit while moving
  if (mode == RETRACTING && esRetract.active) {
    mode = STOPPED; motorStop(); publishRunState("stopped", true);
    mqtt.publish(T_STATUS, "auto-stop: retract endstop");
  }
  if (mode == EXTENDING && esExtend.active) {
    mode = STOPPED; motorStop(); publishRunState("stopped", true);
    mqtt.publish(T_STATUS, "auto-stop: extend endstop");
  }

  // Hold motor outputs by mode
  switch (mode) {
    case EXTENDING:  motorExtend();  break;
    case RETRACTING: motorRetract(); break;
    default:         motorStop();    break;
  }

  // ---- Audio trigger: falling edge on GPIO14 → play 0001.mp3 ----
  {
    bool raw = digitalRead(AUDIO_TRIG_PIN);          // HIGH idle, LOW asserted
    uint32_t now = millis();

    // debounce
    static bool debounced = true;
    if (raw != audioTrigPrev) {
      audioTrigLastChange = now;
      audioTrigPrev = raw;
    }
    if ((now - audioTrigLastChange) > AUDIO_DEBOUNCE_MS) {
      debounced = raw;
    }

    // detect falling edge HIGH -> LOW
    static bool prevDebounced = true;
    if (prevDebounced && !debounced) {
      if (now - audioLastFire > AUDIO_COOLDOWN_MS) {
        audioLastFire = now;
        playTrack1();
      }
    }
    prevDebounced = debounced;
  }

  // Heartbeat
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    mqtt.publish(T_STATUS, "still online");
  }
}
