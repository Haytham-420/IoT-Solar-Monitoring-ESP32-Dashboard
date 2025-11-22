#include <WiFi.h>
#include <FirebaseESP32.h>
#include "RTClib.h"
#include "DHTesp.h"

// Firebase Configuration
#define API_KEY "" // deleted the API key for security
#define DATABASE_URL "" // deleted for security

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Hardware objects
DHTesp dht_sensor_battery;
DHTesp dht_sensor_inverter;
RTC_DS1307 rtc;

// Day names for display
char days_of_week[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Network credentials
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// System alert thresholds
const float LOW_BATTERY_THRESHOLD = 23.5;
const float OVERLOAD_THRESHOLD = 2000.0;
const float OVERHEAT_THRESHOLD = 60.0;

// GPIO pin assignments
const int PIN_LED_LOW_BATTERY       = 12;
const int PIN_LED_OVERLOAD          = 13;
const int PIN_LED_BATTERY_OVERHEAT  = 16;
const int PIN_LED_INVERTER_OVERHEAT = 17;
const int PIN_BUZZER                = 5;
const int PIN_DHT_BATTERY           = 26;
const int PIN_DHT_INVERTER          = 25;
const int PIN_POT_LOAD              = 33;
const int PIN_POT_BATTERY           = 32;
const int PIN_POT_CURRENT           = 35;
const int PIN_POT_VOLTAGE           = 34;
const int PIN_RELAY                 = 4;
const int PIN_EMERGENCY_BUTTON      = 27;

// System state variables
bool load_enabled = true;
bool last_button_state = HIGH;

// Non-blocking timing control (instead of "delay()" function)
unsigned long last_main_loop_time = 0;
unsigned long last_firebase_check_time = 0;
unsigned long last_temperature_read_time = 0;

const unsigned long MAIN_LOOP_INTERVAL = 3000;        // Main monitoring cycle
const unsigned long FIREBASE_CHECK_INTERVAL = 3000;   // Remote command check
const unsigned long TEMPERATURE_READ_INTERVAL = 2000; // Temperature sensor update

// Cached sensor readings (for performance)
float cached_battery_temperature = 25.0;
float cached_inverter_temperature = 25.0;

// Alert status flags
bool alert_low_battery = false;
bool alert_overload = false;
bool alert_battery_overheat = false;
bool alert_inverter_overheat = false;

/**
   Initialize Firebase connection with retry logic
*/
void initialize_firebase() {
  Serial.println("🔥 Initializing Firebase...");

  config.database_url = DATABASE_URL;
  config.api_key = API_KEY;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Anonymous sign-up with retry mechanism
  while (!Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("❌ Sign-up failed: " + String(config.signer.signupError.message.c_str()));
    delay(1000);
  }
  Serial.println("✅ Firebase signed up successfully");
}

/**
   Handle emergency shutdown button with debouncing
*/
void handle_emergency_button() {
  int current_button_state = digitalRead(PIN_EMERGENCY_BUTTON);

  // Detect button press (HIGH to LOW transition)
  if (current_button_state == LOW && last_button_state == HIGH) {
    load_enabled = !load_enabled;
    if (load_enabled) {
      digitalWrite(PIN_RELAY, HIGH);
      Serial.println("🟢 Emergency button pressed! Load ENABLED");

    } else {
      digitalWrite(PIN_RELAY, LOW);
      Serial.println("🔴 Emergency button pressed! Load DISABLED");

    }

    // Update Firebase state (non-blocking)
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      Firebase.setBool(fbdo, "/control/shutdownLoad", !load_enabled);
    }
  }

  last_button_state = current_button_state;
}

/**
   Check for remote shutdown commands from Firebase
*/
void check_firebase_shutdown_command() {
  if (!Firebase.ready()) return;

  if (Firebase.getBool(fbdo, "/control/shutdownLoad")) {
    bool should_shutdown = fbdo.boolData();
    bool currently_enabled = load_enabled;

    if (should_shutdown && currently_enabled) {
      load_enabled = false;
      digitalWrite(PIN_RELAY, LOW);
      Serial.println("🔴 Remote load shutdown command received!");
    } else if (!should_shutdown && !currently_enabled) {
      load_enabled = true;
      digitalWrite(PIN_RELAY, HIGH);
      Serial.println("🟢 Remote load enable command received!");
    }
  }
}

/**
   Update temperature readings with caching for performance
*/
void update_temperature_readings() {
  if (millis() - last_temperature_read_time >= TEMPERATURE_READ_INTERVAL) {
    last_temperature_read_time = millis();

    TempAndHumidity battery_data = dht_sensor_battery.getTempAndHumidity();
    TempAndHumidity inverter_data = dht_sensor_inverter.getTempAndHumidity();

    // Update cache only if readings are valid
    if (!isnan(battery_data.temperature)) {
      cached_battery_temperature = battery_data.temperature;
    }
    if (!isnan(inverter_data.temperature)) {
      cached_inverter_temperature = inverter_data.temperature;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("⚡ Starting ESP32 Solar Monitor System...");
  delay(1000);

  // Initialize Real Time Clock
  if (!rtc.begin()) {
    Serial.println("❌ Couldn't find RTC - continuing without real time");
    Serial.println("⚠️ Check RTC wiring: SDA->21, SCL->22, VCC->5V, GND->GND");
  } else {
    Serial.println("✅ RTC initialized successfully");
  }

  // Configure all GPIO pins
  pinMode(PIN_LED_LOW_BATTERY, OUTPUT);
  pinMode(PIN_LED_OVERLOAD, OUTPUT);
  pinMode(PIN_LED_BATTERY_OVERHEAT, OUTPUT);
  pinMode(PIN_LED_INVERTER_OVERHEAT, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_EMERGENCY_BUTTON, INPUT_PULLUP);

  // Initialize relay state (load enabled by default)
  digitalWrite(PIN_RELAY, HIGH);
  load_enabled = true;

  // Connect to WiFi with timeout protection
  WiFi.begin(ssid, password);
  Serial.print("📶 Connecting to Wi-Fi");
  unsigned long wifi_start_time = millis();

  while (WiFi.status() != WL_CONNECTED && (millis() - wifi_start_time < 10000)) {
    delay(100);
    Serial.print(".");
    handle_emergency_button(); // Ensure button works even during WiFi connection
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n✅ WiFi connected - IP: ");
    Serial.println(WiFi.localIP());
    initialize_firebase();
  } else {
    Serial.println("\n❌ WiFi connection failed");
  }

  // Initialize temperature sensors
  dht_sensor_battery.setup(PIN_DHT_BATTERY, DHTesp::DHT22);
  dht_sensor_inverter.setup(PIN_DHT_INVERTER, DHTesp::DHT22);

  Serial.println("✅ System initialization complete!");
  Serial.println("🔴 Emergency shutdown ready - Press button to toggle load");
  Serial.println("====================================================");
}

void loop() {
  // Priority 1: Always handle emergency button first
  handle_emergency_button();

  // Priority 2: Update temperature readings with caching
  update_temperature_readings();

  // Priority 3: Handle Firebase commands periodically
  if (millis() - last_firebase_check_time >= FIREBASE_CHECK_INTERVAL) {
    last_firebase_check_time = millis();
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      check_firebase_shutdown_command();
    }
  }

  // Priority 4: Main monitoring and data logging cycle
  if (millis() - last_main_loop_time >= MAIN_LOOP_INTERVAL) {
    last_main_loop_time = millis();

    // Monitor WiFi connection (non-blocking reconnection)
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("📶 WiFi disconnected, attempting reconnection...");
      WiFi.reconnect();
    }

    // Get current timestamp from RTC
    DateTime now;
    unsigned long long timestamp;

    if (rtc.begin()) {
      now = rtc.now();
      timestamp = (now.unixtime() - 10800) * 1000ULL; // Timezone adjustment
    }

    // Read solar panel performance data
    float solar_voltage = map(analogRead(PIN_POT_VOLTAGE), 0, 4095, 350, 370) / 10.0;
    float solar_current = map(analogRead(PIN_POT_CURRENT), 0, 4095, 0, 5 * 80) / 10.0;
    float pv_power = solar_voltage * solar_current;

    // Calculate load consumption (actual vs potential)
    float actual_load_watts = load_enabled ? map(analogRead(PIN_POT_LOAD), 0, 4095, 0, 30000) / 10.0 : 0.0;
    float potential_load_watts = map(analogRead(PIN_POT_LOAD), 0, 4095, 0, 30000) / 10.0;

    // Determine load status
    String load_type = "Disconnected";
    if (load_enabled) {
      if (actual_load_watts > OVERLOAD_THRESHOLD) load_type = "Overload";
      else if (actual_load_watts > 1000) load_type = "High";
      else load_type = "Normal";
    }

    // Battery status monitoring
    float battery_voltage = map(analogRead(PIN_POT_BATTERY), 0, 4095, 220, 280) / 10.0;
    float battery_level = constrain((battery_voltage - 22.0) / 6.0 * 100, 0, 100);
    float battery_power = pv_power - actual_load_watts;
    String battery_status = (battery_power >= 0) ? "Charging" : "Discharging";

    // Use cached temperature readings for performance
    float battery_temp = cached_battery_temperature;
    float inverter_temp = cached_inverter_temperature;

    // Alert system evaluation
    alert_low_battery = battery_voltage < LOW_BATTERY_THRESHOLD;
    alert_overload = actual_load_watts > OVERLOAD_THRESHOLD;
    alert_battery_overheat = battery_temp > OVERHEAT_THRESHOLD;
    alert_inverter_overheat = inverter_temp > OVERHEAT_THRESHOLD;

    // Control alert indicators
    digitalWrite(PIN_LED_LOW_BATTERY, alert_low_battery ? HIGH : LOW);
    digitalWrite(PIN_LED_OVERLOAD, alert_overload ? HIGH : LOW);
    digitalWrite(PIN_LED_BATTERY_OVERHEAT, alert_battery_overheat ? HIGH : LOW);
    digitalWrite(PIN_LED_INVERTER_OVERHEAT, alert_inverter_overheat ? HIGH : LOW);

    // Audio alert control
    if (alert_low_battery || alert_overload || alert_battery_overheat || alert_inverter_overheat)
      tone(PIN_BUZZER, 1000);
    else
      noTone(PIN_BUZZER);

    // Serial monitor output (unchanged formatting)
    Serial.print("\n[✓] Time: ");
    if (rtc.begin()) {
      Serial.print(now.year(), DEC); Serial.print('/'); Serial.print(now.month(), DEC); Serial.print('/'); Serial.print(now.day(), DEC);
      Serial.print(" ("); Serial.print(days_of_week[now.dayOfTheWeek()]); Serial.print(") ");
      Serial.print(now.hour(), DEC); Serial.print(':'); Serial.print(now.minute(), DEC); Serial.print(':'); Serial.print(now.second(), DEC);
    }

    Serial.println();
    Serial.printf("⚡ PV Power: %.2f W | Voltage: %.2f V | Current: %.2f A\n", pv_power, solar_voltage, solar_current);
    Serial.printf("🔌 Load: %.2f W (%s) [%s]\n", actual_load_watts, load_type.c_str(), load_enabled ? "CONNECTED" : "DISCONNECTED");
    if (!load_enabled) Serial.printf("🔍 Potential Load: %.2f W (if connected)\n", potential_load_watts);
    Serial.printf("🔋 Battery: %.1f V | %.1f %% (%s, %.1f W)\n", battery_voltage, battery_level, battery_status.c_str(), battery_power);
    Serial.printf("🌡️ Temp: Batt %.1f °C | Inv %.1f °C\n", battery_temp, inverter_temp);

    // Display active alerts
    if (alert_low_battery) Serial.println("⚠️ Battery low alert!");
    if (alert_overload) Serial.println("⚠️ Overload alert!");
    if (alert_battery_overheat) Serial.println("⚠️ Battery Overheat alert!");
    if (alert_inverter_overheat) Serial.println("⚠️ Inverter Overheat alert!");

    // Send telemetry data to Firebase
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      FirebaseJson json;
      json.set("timestamp", String(timestamp));
      json.set("pvPower", pv_power);
      json.set("loadWatts", actual_load_watts);
      json.set("loadType", load_type);
      json.set("loadEnabled", load_enabled);
      json.set("batteryLevel", battery_level);
      json.set("batteryVoltage", battery_voltage);
      json.set("batteryStatus", battery_status);
      json.set("batteryPower", battery_power);
      json.set("batteryTemp", battery_temp);
      json.set("inverterTemp", inverter_temp);
      json.set("solarVoltage", solar_voltage);
      json.set("solarCurrent", solar_current);
      json.set("alertLowBattery", alert_low_battery);
      json.set("alertOverload", alert_overload);
      json.set("alertBatteryOverheat", alert_battery_overheat);
      json.set("alertInverterOverheat", alert_inverter_overheat);

      if (Firebase.pushJSON(fbdo, "/readings", json)) {
        Serial.println("🔥 Firebase → ✅ Data uploaded successfully");
      } else {
        Serial.printf("🔥 Firebase → ❌ Upload failed: %s\n", fbdo.errorReason().c_str());
      }
    } else {
      Serial.println("🔥 Firebase not ready, skipping upload");
    }

    Serial.println("====================================================");
  }
}
