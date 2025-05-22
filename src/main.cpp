#include <Arduino.h>
#include <USB.h>
#include <SimpleCLI.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <timedLoop.h>
#include <timeout.h>
#include "wifiManager.h"
#undef cli  // avoid USB.h macro conflict
#include "timeControl.h"

// ----------------------- Configuration Constants -----------------------
// Serial speed, device ID, pin assignments, default watering delay, MQTT topic
static const long    SERIAL_SPEED         = 115200;
static const char*   DEVICE_ID            = "garden_irrigator";
static const uint8_t VALVE_OUTPUT_PIN     = 2;
static const uint8_t MOISTURE_INPUT_PIN   = 3;
static const uint32_t DEFAULT_WATER_DELAY = 10000UL;
static const char*   MQTT_TOPIC           = "graph/data";

// ----------------------- Valve Driver -----------------------
// Controls the relay/valve for irrigation
class ValveDriver {
  uint8_t pin_;      // GPIO pin for valve
  bool    isOpen_;   // Valve state

public:
  ValveDriver() : pin_(255), isOpen_(false) {}

  // Initialize the valve pin and set it to closed (HIGH)
  void init(uint8_t pin) {
    pin_ = pin;
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, HIGH);  // start closed (HIGH)
  }

  // Open the valve (set pin LOW)
  void open() {
    if (!isOpen_) {
      digitalWrite(pin_, LOW);
      isOpen_ = true;
    }
  }

  // Close the valve (set pin HIGH)
  void close() {
    if (isOpen_) {
      digitalWrite(pin_, HIGH);
      isOpen_ = false;
    }
  }

  // Return current valve state
  bool status() const { return isOpen_; }
};

// ----------------------- Watering Scheduler -----------------------
// Manages watering timing and persistence
class WaterManager {
  ValveDriver    valve_;    // Valve control
  timeout        timer_;    // Watering timer
  Preferences    prefs_;    // Store watering delay
  uint32_t       delayMs_;  // Watering duration

public:
  // Load watering delay from preferences or use default
  WaterManager(uint32_t defaultDelay)
    : timer_(), delayMs_(defaultDelay)
  {
    prefs_.begin("water_cfg", false);
    delayMs_ = prefs_.getULong("delay", defaultDelay);
    timer_.setTime(delayMs_);
  }

  // Initialize valve hardware
  void begin(uint8_t valvePin) {
    valve_.init(valvePin);
  }

  // Check if watering timer finished and stop if needed
  void update() {
    if (timer_.finished()) {
      stop();
    }
  }

  // Start watering and timer
  void start() {
    Serial.println(">>> Watering STARTED");
    valve_.open();
    timer_.start();
  }

  // Stop watering and timer
  void stop() {
    Serial.println(">>> Watering STOPPED");
    valve_.close();
    timer_.stop();
  }

  // Return if watering is active
  bool active() {
    return timer_.isRunning();
  }

  // Set new watering delay and save to preferences
  void setDelay(uint32_t ms) {
    delayMs_ = ms;
    timer_.setTime(delayMs_);
    prefs_.putULong("delay", delayMs_);
  }

  // Get current watering delay
  uint32_t getDelay() const {
    return delayMs_;
  }
};

// ----------------------- Moisture Sensor -----------------------
// Reads and averages soil moisture sensor values
class SoilSensor {
  uint8_t pin_;  // Analog pin for sensor

public:
  SoilSensor() : pin_(255) {}

  // Initialize sensor pin
  void begin(uint8_t pin) {
    pin_ = pin;
    pinMode(pin_, INPUT);
  }

  // Read and average multiple samples for stability
  int readAverage() const {
    const int samples = 100;
    long total = 0;
    for (int i = 0; i < samples; ++i) {
      total += analogRead(pin_);
    }
    return total / samples;
  }
};

// ----------------------- Irrigation Logic -----------------------
// Combines sensor, watering, and threshold logic
class IrrigationManager {
  SoilSensor      sensor_;        // Soil moisture sensor
  WaterManager    waterMgr_;      // Watering scheduler
  timedLoop       sampleLoop_;    // Sampling interval
  Preferences     prefs_;         // Store threshold
  int             threshold_;     // Moisture threshold

public:
  // Load threshold from preferences or use default
  IrrigationManager(uint32_t defaultDelay)
    : waterMgr_(defaultDelay),
      sampleLoop_(1000),
      threshold_(0)
  {
    prefs_.begin("irrig_cfg", false);
    threshold_ = prefs_.getInt("thresh", 0);
  }

  // Initialize sensor and valve
  void begin(uint8_t valvePin, uint8_t sensorPin) {
    Serial.println("Initializing Irrigation Manager...");
    sensor_.begin(sensorPin);
    waterMgr_.begin(valvePin);
  }

  // Periodically sample moisture and trigger watering if needed
  void update() {
    waterMgr_.update();

    if (!waterMgr_.active() && sampleLoop_.check()) {
      int moisture = sensor_.readAverage();
      Serial.print("Moisture reading: ");
      Serial.println(moisture);

      if (moisture > threshold_) {
        waterMgr_.start();
      }
    }
  }

  // Set and save new moisture threshold
  void setThreshold(int t) {
    threshold_ = t;
    prefs_.putInt("thresh", threshold_);
    Serial.print("New moisture threshold: ");
    Serial.println(threshold_);
  }

  // Get current threshold
  int getThreshold() const {
    return threshold_;
  }

  // Read current moisture value
  int    readMoisture() const         { return sensor_.readAverage(); }
  // Return if watering is active
  bool   isCurrentlyWatering()       { return waterMgr_.active(); }
};

// ----------------------- Time and Irrigation Controllers -----------------------
timeControl timeCtrl("south-america.pool.ntp.org", -10800, 0); // NTP time sync
IrrigationManager irrigationCtrl(DEFAULT_WATER_DELAY);         // Main irrigation logic

// ----------------------- MQTT Service -----------------------
// Handles MQTT connection, publishing, and configuration
class MqttService {
  WiFiClient    wifiConn_;        // WiFi client for MQTT
  PubSubClient  client_;          // MQTT client
  Preferences   prefs_;           // Store broker/port
  timedLoop     reconnectLoop_;   // Reconnect interval
  timedLoop     publishLoop_;     // Publish interval
  String        broker_;          // MQTT broker address
  int           port_;            // MQTT broker port

public:
  MqttService()
    : client_(wifiConn_),
      reconnectLoop_(10000),
      publishLoop_(1000),
      broker_(""),
      port_(1883)
  {}

  // Load broker/port from preferences and set up MQTT client
  void begin() {
    prefs_.begin("mqtt_cfg", false);
    broker_ = prefs_.getString("broker", "");
    port_   = prefs_.getInt("port", 1883);
    client_.setCallback(onMessage);
    client_.setServer(broker_.c_str(), port_);
  }

  // Handle incoming MQTT messages
  static void onMessage(char* topic, byte* payload, unsigned int length) {
    String msg;
    for (unsigned int i = 0; i < length; ++i) {
      msg += char(payload[i]);
    }
    Serial.print("MQTT @"); Serial.print(topic); Serial.print(" => "); Serial.println(msg);
  }

  // Set and save new broker address
  void setBroker(const String& b) {
    broker_ = b;
    prefs_.putString("broker", broker_);
    client_.setServer(broker_.c_str(), port_);
  }

  // Set and save new port
  void setPort(int p) {
    port_ = p;
    prefs_.putInt("port", port_);
    client_.setServer(broker_.c_str(), port_);
  }

  // Return if MQTT is connected
  bool connected() {
    return client_.connected();
  }

  // Handle MQTT connection, reconnection, and publishing
  void loop() {
    if (!client_.connected() && reconnectLoop_.check() && WiFi.status() == WL_CONNECTED) {
      reconnect();
    }

    client_.loop();

    // Publish time, moisture, and watering status periodically
    if (client_.connected() && publishLoop_.check()) {
        String payload = timeCtrl.getTimeString();
        payload += "," + String(irrigationCtrl.readMoisture());
        payload += "," + String(irrigationCtrl.isCurrentlyWatering());
        client_.publish(MQTT_TOPIC, payload.c_str());
    }
  }

private:
  // Attempt to reconnect to MQTT broker
  void reconnect() {
    if (broker_.length() == 0) {
      Serial.println("MQTT broker not set");
      return;
    }
    Serial.print("Connecting to MQTT...");
    if (client_.connect(DEVICE_ID)) {
      Serial.println("connected");
      client_.subscribe(MQTT_TOPIC);
    } else {
      Serial.print("failed, rc=");
      Serial.println(client_.state());
    }
  }
};

// ----------------------- Global Objects -----------------------
wifiManager       netMgr(1);   // WiFi manager
MqttService       mqttSrv;     // MQTT service

// ----------------------- Arduino Setup & Loop -----------------------
void setup() {
  Serial.begin(SERIAL_SPEED);                                      // Start serial
  netMgr.begin(true);                                              // Start WiFi
  irrigationCtrl.begin(VALVE_OUTPUT_PIN, MOISTURE_INPUT_PIN);      // Init irrigation
  mqttSrv.begin();                                                 // Init MQTT
  timeCtrl.begin();                                                // Init NTP time
}

void loop() {
  netMgr.handle();         // Handle WiFi events
  irrigationCtrl.update(); // Run irrigation logic
  mqttSrv.loop();          // Handle MQTT
  timeCtrl.handle();       // Update time
}