/**********************************************************
-Brayan Casanova Fernandez
-Valentina Tobar Monsalve
-Samuel Zuñiga Bolaños
**********************************************************/
/* ESP32 - Secador de café (1xHX711) - Integrado
   - DHT22 (ambient)
   - DS18B20 (temp café)
   - HX711 (1 célula)
   - SSD1306 (I2C)
   - DS1307 (RTC via RTClib)
   - Vector 3-bit -> Pro Mini (0..7)
   - Button wakes device; first-power: auto-calibrate + wait button before normal sleep cycles
   - Persistent during deep-sleep (RTC_DATA_ATTR): calibrated flag, calibration_factor, startWeight, lastLevel
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "time.h"
#include "DHT.h"
#include "HX711.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <RTClib.h>
#include <Preferences.h>

// ------------------ CONFIG ------------------
#define TARGET_WEIGHT 104.0f   // Ajusta aquí el peso objetivo en gramos
#define DHTPIN 15
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// HX711 (1 módulo)
#define HX_DOUT 26
#define HX_SCK  14
HX711 scale;

// DS18B20
#define DS18B20_PIN 32
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);

// OLED I2C
#define I2C_SDA 21
#define I2C_SCL 22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);

// RTC DS1307
RTC_DS1307 rtc;

// LEDs
#define LED_RED    13
#define LED_YELLOW 2
#define LED_GREEN  4

// Wake button (RTC-capable)
#define BUTTON_PIN 33

// Vector pins (3-bit) -> Pro Mini inputs
#define VEC_PIN0 16
#define VEC_PIN1 17
#define VEC_PIN2 5

// WiFi / NTP
const char* ssid     = "FAMILIA_TOBAR";
const char* password = "2166683693";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -18000; // UTC -5
const int   daylightOffset_sec = 0;

// Sleep / active times (ajusta si quieres)
#define uS_TO_S_FACTOR 1000000ULL
#define SLEEP_SECONDS 300   // 5 min
#define ACTIVE_SECONDS 5   // 30 s after wake

// HX711 read params
const int HX_SAMPLES = 10;

// Temperature thresholds (°C)
const float TEMP_MIN = 38.0;
const float TEMP_MAX = 43.0;

// Calibration default (will be preserved in RTC memory during deep sleep)
RTC_DATA_ATTR bool calibrated = false;           // false => first power (auto-tare & wait button)
RTC_DATA_ATTR float calibration_factor = 48100;  // default (you can adjust via Serial)
RTC_DATA_ATTR float startWeight = -1.0;          // grams; -1 = not set
RTC_DATA_ATTR uint8_t lastLevel = 3;             // 0..7
RTC_DATA_ATTR long savedOffset = 0;              // offset persistente en deep sleep


// Preferences namespace to optionally persist across power cycles (optional)
// NOTE: user asked parameters remain only while powered; so we won't persist across power loss.
// Preferences pref; // not used to keep behavior matching request

// OLED address
#define OLED_ADDR 0x3C

// ------------------ Helpers ------------------

// set 3-bit vector
void setVectorLevel(uint8_t level) {
  level &= 0x07;
  digitalWrite(VEC_PIN0, (level & 0x01) ? HIGH : LOW);
  digitalWrite(VEC_PIN1, (level & 0x02) ? HIGH : LOW);
  digitalWrite(VEC_PIN2, (level & 0x04) ? HIGH : LOW);
  lastLevel = level;
}

// read weight (returns grams)
float readWeightGrams(int samples = HX_SAMPLES) {
  if (!scale.is_ready()) return NAN;
  // scale.get_units() returns "units" depending on scale factor; we treat "units" as kilos if calibration set that way.
  // To avoid confusion we will use get_units and convert to grams by *1000
  float units = scale.get_units(samples); // typically returns kg if scale factor chosen that way
  float grams = units * 1000.0f;
  return grams;
}

// compute level 0..7 from weight (g) and coffee temp (°C)
uint8_t computeLevelFromWeightTemp(float weight_g, float tempCafeC) {
  float start = (startWeight > 0.0f) ? startWeight : 200.0f; // default start 200 g
  if (weight_g <= TARGET_WEIGHT) return 7;
  // map weight range TARGET..start => level 7..1
  float w = constrain(weight_g, TARGET_WEIGHT, start);
  float frac = (w - TARGET_WEIGHT) / (start - TARGET_WEIGHT); // 0..1
  float baseLevel = (1.0f - frac) * 6.0f + 1.0f; // ~1..7
  float level = baseLevel;
  if (!isnan(tempCafeC)) {
    if (tempCafeC > TEMP_MAX) level += 1.5f;
    else if (tempCafeC < TEMP_MIN) level -= 1.5f;
  }
  int lvl = (int)roundf(constrain(level, 0.0f, 7.0f));
  return (uint8_t)lvl;
}

// update LEDs by progress (weight in grams)
void updateLEDsByWeight(float weight_g) {
  if (weight_g <= TARGET_WEIGHT) {
    digitalWrite(LED_GREEN, HIGH); digitalWrite(LED_YELLOW, LOW); digitalWrite(LED_RED, LOW);
    return;
  }
  float start = (startWeight > 0.0f) ? startWeight : 200.0f;
  float pct = 100.0f * (start - weight_g) / (start - TARGET_WEIGHT);
  if (pct < 30.0f) {
    digitalWrite(LED_RED, HIGH); digitalWrite(LED_YELLOW, LOW); digitalWrite(LED_GREEN, LOW);
  } else if (pct < 80.0f) {
    digitalWrite(LED_RED, LOW); digitalWrite(LED_YELLOW, HIGH); digitalWrite(LED_GREEN, LOW);
  } else {
    digitalWrite(LED_RED, LOW); digitalWrite(LED_YELLOW, LOW); digitalWrite(LED_GREEN, HIGH);
  }
}

// show on OLED (and Serial)
void showOLEDandSerial(float weight_g, float tempAmbC, float humPct, float tempCafeC, uint8_t level, DateTime now) {
  Serial.println("---------- STATUS ----------");
  Serial.printf("Weight: %.1f g\n", weight_g);
  if (startWeight > 0.0f) Serial.printf("StartWeight: %.1f g\n", startWeight);
  else Serial.println("StartWeight: N/A");
  Serial.printf("Target: %.1f g\n", TARGET_WEIGHT);
  Serial.printf("Ambient: %.1f C  Hum: %.1f %%\n", tempAmbC, humPct);
  Serial.printf("Cafe: %.1f C\n", tempCafeC);
  Serial.printf("Level: %d\n", level);
  Serial.printf("Calibrated flag: %d  calib_factor: %.0f\n", calibrated ? 1 : 0, calibration_factor);
  Serial.printf("RTC: %02d/%02d/%04d %02d:%02d:%02d\n", now.day(), now.month(), now.year(), now.hour(), now.minute(), now.second());
  Serial.println("----------------------------");

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);

  display.printf("Peso: %.0f g\n", weight_g);
  if (startWeight > 0.0f) display.printf("Start: %.0f g\n", startWeight);
  else display.printf("Start: N/A\n");
  display.printf("Target: %.0f g\n", TARGET_WEIGHT);
  display.printf("Amb: %.1fC H:%.1f%%\n", tempAmbC, humPct);
  display.printf("Cafe: %.1fC\n", tempCafeC);
  display.printf("Nivel: %d\n", level);
  display.printf("%02d/%02d/%04d %02d:%02d\n", now.day(), now.month(), now.year(), now.hour(), now.minute());
  display.display();
}

// auto-tare on first power (do once per power cycle)
void autoTareOnce() {
  Serial.println("Auto-tare: dejando la bascula en 0 (primera vez tras energizar)...");
  scale.set_scale();     // set no factor yet
  scale.tare();        // tare (average)
  long zero_factor = scale.read_average();
  Serial.print("Zero factor: "); Serial.println(zero_factor);
  // leave calibration_factor as-is (user can tune via serial + / -). We persist calibration_factor in RTC memory.
  calibrated = true;
  // apply the calibration_factor we have
  scale.set_scale(calibration_factor);
  savedOffset = scale.get_offset();   // Save offset
}

// Sync NTP and update DS1307
void syncTimeToRTCIfWifi() {
  Serial.print("Intentando conectar WiFi para NTP...");
  WiFi.begin(ssid, password);
  unsigned long tstart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - tstart < 4000) {
    delay(200);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado. Pidiendo NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      DateTime dt(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      Serial.print("Hora NTP: "); Serial.println(dt.timestamp());
      if (rtc.begin()) {
        rtc.adjust(dt);
        Serial.println("RTC DS1307 actualizado desde NTP.");
      } else {
        Serial.println("RTC no encontrado para ajustar.");
      }
    } else {
      Serial.println("No se obtuvo hora NTP.");
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  } else {
    Serial.println("No hay WiFi; se usará hora del RTC si existe.");
  }
}

// handle serial calibration adjustment (+ / -)
void handleSerialCalibration() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '+' || c == 'a') {
      calibration_factor += 10; // step
      scale.set_scale(calibration_factor);
      Serial.printf("Calibration factor increased -> %.0f\n", calibration_factor);
    } else if (c == '-' || c == 'z') {
      calibration_factor -= 10;
      scale.set_scale(calibration_factor);
      Serial.printf("Calibration factor decreased -> %.0f\n", calibration_factor);
    } else if (c == 't') {
      // manual tare
      scale.tare(30);
      Serial.println("Tare manual realizado");
    }
  }
}

// ------------------ SETUP ------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n--- ESP32 Secador (inicio) ---");

  // pins
  pinMode(LED_RED, OUTPUT); pinMode(LED_YELLOW, OUTPUT); pinMode(LED_GREEN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP); // button to GND
  pinMode(VEC_PIN0, OUTPUT); pinMode(VEC_PIN1, OUTPUT); pinMode(VEC_PIN2, OUTPUT);
  setVectorLevel(lastLevel);

  // I2C + OLED + RTC
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
  } else {
    display.clearDisplay(); display.display();
  }

  if (!rtc.begin()) {
    Serial.println("RTC DS1307 no detectado!");
  } else {
    if (!rtc.isrunning()) {
      Serial.println("RTC no corriendo (se ajustará si hay NTP).");
    }
  }

  // sensors init
  dht.begin();
  ds18b20.begin();

  // HX711 init
  scale.begin(HX_DOUT, HX_SCK);

  // Configure wakeup on BUTTON_PIN (ext0) - wake on LOW
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);

  // If not calibrated yet (first power), do auto tare and then wait for button to start cycles
  if (!calibrated) {
    autoTareOnce(); // sets calibrated = true and sets scale.set_scale(calibration_factor)

    Serial.println("Esperando presionar el boton para iniciar el ciclo (primera vez tras energizar)...");
    // show message on OLED until button pressed
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0,0);
    display.println("Primera vez: calibrado OK");
    display.println("Presione boton para iniciar");
    display.display();

    // Wait until button pressed (press to start)
    while (digitalRead(BUTTON_PIN) == HIGH) {
      handleSerialCalibration(); // allow user to tweak calibration by serial while waiting
      delay(100);
    }
    // Debounce: wait for release
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    Serial.println("Boton detectado: iniciando ciclo normal.");
  } else {
    // already calibrated (waking from deep sleep); ensure scale uses saved calibration_factor
    scale.set_scale(calibration_factor);
    scale.set_offset(savedOffset);   // restore offset
    Serial.println("Despertado desde deep-sleep: usando calibración previa.");
  }

  // Try sync NTP and update RTC (short attempt)
  syncTimeToRTCIfWifi(); // optional NTP -> RTC update

  // Read sensors
  float tempAmb = NAN, humAmb = NAN, tempCafe = NAN;
  tempAmb = dht.readTemperature();
  humAmb = dht.readHumidity();
  ds18b20.requestTemperatures();
  tempCafe = ds18b20.getTempCByIndex(0);

  // Read weight in grams
  float weight_g = readWeightGrams(HX_SAMPLES);
  if (isnan(weight_g)) weight_g = 0.0f;

  Serial.printf("Lecturas iniciales -> Peso: %.1f g | AmbT: %.1fC H: %.1f%% | CafeT: %.1fC\n",
                weight_g, tempAmb, humAmb, tempCafe);

  // If startWeight==0 (we tared previously) and detect coffee placed (>= MIN_COFFEE_DETECT), register startWeight
  const float MIN_COFFEE_DETECT = 20.0f; // grams
  if (startWeight == 0.0f && weight_g >= MIN_COFFEE_DETECT) {
    startWeight = weight_g;
    Serial.printf("Start weight registrado: %.1f g\n", startWeight);
  }

  // compute level (0..7)
  uint8_t level = computeLevelFromWeightTemp(weight_g, tempCafe);
  setVectorLevel(level);
  updateLEDsByWeight(weight_g);

  // show info and serial
  DateTime now = rtc.now();
  showOLEDandSerial(weight_g, tempAmb, humAmb, tempCafe, level, now);

  // Active period: remain ACTIVE_SECONDS seconds before sleeping
  unsigned long awakeStart = millis();
  while (millis() - awakeStart < (unsigned long)ACTIVE_SECONDS * 1000UL) {
    handleSerialCalibration(); // allow user to tweak calibration during active time
    delay(200);
  }

  // Configure timer wake in addition to ext0 (button)
  esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * uS_TO_S_FACTOR);

  Serial.printf("Entrando deep sleep por %d segundos. Boton despertara.\n", SLEEP_SECONDS);
  delay(100);
  esp_deep_sleep_start();
}

// loop never reached because device restarts from setup() each wake
void loop() { }
