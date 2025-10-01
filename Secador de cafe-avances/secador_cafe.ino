/**********************************************************
-Brayan Casanova Fernandez
-Valentina Tobar Monsalve
-Samuel Zuñiga Bolaños
**********************************************************/

/*********************************************************
  ESP32 - Control de secado de café
  - DHT22
  - 2x HX711
  - OLED SSD1306 I2C
  - 3 LEDs (ROJO, AMARILLO, VERDE)
  - Botón para TARE / Wakeup
  - Comunica duty PWM al ATtiny por I2C (addr 0x08)
  - Deep sleep 5 min (o wake por botón)
  - Conserva startWeight y lastDuty con RTC_DATA_ATTR
*********************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "time.h"
#include "DHT.h"
#include "HX711.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ------------------ CONFIG ------------------
#define DHTPIN 15
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// HX711 pins (ajusta si hace falta)
#define HX1_DOUT 26
#define HX1_SCK  14
#define HX2_DOUT 27
#define HX2_SCK  12
HX711 scale1;
HX711 scale2;

// analog temp (si la necesitas)
#define TEMP_ANALOG_PIN 35

// OLED I2C
#define I2C_SDA 21
#define I2C_SCL 22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);

// LEDs
#define LED_RED    13
#define LED_YELLOW 2
#define LED_GREEN  4

// Button (debe ser RTC GPIO para wake ext0). Usamos GPIO33 (RTC capable)
#define BUTTON_PIN 33

// ATtiny I2C address
#define ATTINY_ADDR 0x08

// PWM info (ESP no genera PWM continuo, ATtiny lo mantiene)
RTC_DATA_ATTR int lastDuty = 128;     // valor enviado al ATtiny y que se conserva
RTC_DATA_ATTR float startWeight = -1; // peso inicial (masa del café) - se conserva entre wakes

// Peso objetivo y tolerancias
const float TARGET_WEIGHT = 105.0;   // gramos (meta de secado)
const float START_ASSUMED = 200.0;   // si no has tareado, valor por defecto

// Sleep
#define uS_TO_S_FACTOR 1000000ULL
#define TIME_TO_SLEEP 300  // segundos (5 minutos)

// WiFi/NTP (opcional, si quieres timestamp; si no quitar WiFi)
const char* ssid     = "FAMILIA_TOBAR";
const char* password = "2166683693";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -18000;  // UTC -5
const int   daylightOffset_sec = 0;

// ------------------ FUNCIONES AUX ------------------

// Enviar duty al ATtiny por I2C (0-255)
void sendDutyToAttiny(uint8_t duty) {
  Wire.beginTransmission(ATTINY_ADDR);
  Wire.write(duty);
  Wire.endTransmission();
  lastDuty = duty;
}

// Leer HX711 (retorna valor en unidades si has configurado scale)
float readScaleUnits(HX711 &s, int samples = 5) {
  if (!s.is_ready()) return NAN;
  double sum = 0;
  for (int i = 0; i < samples; ++i) sum += s.get_units(1); // usa get_units de librería
  return (float)(sum / samples);
}

// Mapear peso a duty: define la lógica que quieras.
// Aquí mapeamos linealmente entre startWeight -> TARGET_WEIGHT a duty 50->200 (ejemplo)
uint8_t weightToDuty(float weight) {
  // Si startWeight desconocido -> usar START_ASSUMED
  float start = (startWeight > 0) ? startWeight : START_ASSUMED;

  // Si ya llegó a target -> mandar máximo (para enfriar)
  if (weight <= TARGET_WEIGHT) return 255;

  // Mapear rango [start..TARGET] a [80..200] (menos agresivo)
  float duty = map(constrain(weight, TARGET_WEIGHT, start), TARGET_WEIGHT, start, 255, 80);
  duty = constrain(round(duty), 0, 255);
  return (uint8_t)duty;
}

// Actualiza LEDs según peso total o promedio
void updateLEDs(float weight) {
  // Rango simple:
  // rojo: muy pesado (recién puesto)
  // amarillo: en proceso
  // verde: cerca o en objetivo
  if (weight <= TARGET_WEIGHT) {
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_YELLOW, LOW);
    digitalWrite(LED_RED, LOW);
  } else {
    float start = (startWeight > 0) ? startWeight : START_ASSUMED;
    float pct = 100.0 * (start - weight) / (start - TARGET_WEIGHT); // porcentaje de secado
    if (pct < 30) { // poco secado -> rojo
      digitalWrite(LED_RED, HIGH);
      digitalWrite(LED_YELLOW, LOW);
      digitalWrite(LED_GREEN, LOW);
    } else if (pct < 80) { // medio -> amarillo
      digitalWrite(LED_RED, LOW);
      digitalWrite(LED_YELLOW, HIGH);
      digitalWrite(LED_GREEN, LOW);
    } else { // casi listo -> verde
      digitalWrite(LED_RED, LOW);
      digitalWrite(LED_YELLOW, LOW);
      digitalWrite(LED_GREEN, HIGH);
    }
  }
}

// Mostrar en OLED
void showOLED(float w1, float w2, float total, float tempDHT, float humDHT) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);

  display.printf("Peso1: %.1fg\n", w1);
  display.printf("Peso2: %.1fg\n", w2);
  display.printf("Total: %.1fg\n", total);

  if (startWeight > 0) display.printf("Start: %.1fg\n", startWeight);
  else display.printf("Start: N/A\n");

  display.printf("Target: %.1fg\n", TARGET_WEIGHT);
  display.printf("T: %.1fC H: %.1f%%\n", tempDHT, humDHT);
  display.printf("Duty: %d\n", lastDuty);

  display.display();
}

// Debounced check for button press (returns true if pressed)
bool isButtonPressed() {
  static uint32_t lastMillis = 0;
  static bool lastState = HIGH;
  bool state = digitalRead(BUTTON_PIN);
  uint32_t now = millis();
  if (state != lastState) {
    lastMillis = now;
    lastState = state;
  }
  if ((now - lastMillis) > 50 && state == LOW) { // botón activo LOW
    return true;
  }
  return false;
}

// Tare scales (restar peso de maquinaria)
void tareScales() {
  // Si tu librería HX711 tiene tare() o set_offset -> usa esas
  // Usamos set_scale/set_offset? Aquí usamos tare() de librería común
  Serial.println("Tare: ajustando 0...");
  scale1.tare(10);
  scale2.tare(10);
  // Guardar startWeight como suma actual (después de tare será solo café si colocas café despues)
  float w1 = readScaleUnits(scale1, 5);
  float w2 = readScaleUnits(scale2, 5);
  startWeight = w1 + w2;
  Serial.printf("Nuevo startWeight guardado: %.2f g\n", startWeight);
}

// ------------------ SETUP ------------------
void setup() {
  // Inicial
  Serial.begin(115200);
  delay(200);

  // Pines
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_GREEN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP); // botón a GND

  // I2C (para OLED y ATtiny comms)
  Wire.begin(I2C_SDA, I2C_SCL);

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
  } else {
    display.clearDisplay();
    display.display();
  }

  // DHT
  dht.begin();

  // HX711 init
  scale1.begin(HX1_DOUT, HX1_SCK);
  scale2.begin(HX2_DOUT, HX2_SCK);

  // Conexión WiFi y NTP (opcional, si no la necesitas comenta)
  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  } else {
    Serial.println("\nWiFi NO conectado (continuo sin NTP)");
  }

  // ----------------------------------------------------------------
  // Comportamiento principal en cada wake:
  // 1) Leer sensores
  // 2) Calcular peso total
  // 3) Si botón PRESIONADO (corto) -> tareScales()
  // 4) Calcular duty y enviar ATtiny
  // 5) Mostrar en OLED, actualizar LEDs
  // 6) Entrar en deep sleep (o si botón presionado mantener awake para mostrar)
  // ----------------------------------------------------------------

  // Leer sensores
  float tempDHT = NAN, humDHT = NAN;
  tempDHT = dht.readTemperature();
  humDHT  = dht.readHumidity();

  // Leer celdas
  float w1 = readScaleUnits(scale1, 5);
  float w2 = readScaleUnits(scale2, 5);
  float totalWeight = (isnan(w1) ? 0.0 : w1) + (isnan(w2) ? 0.0 : w2);

  // Si startWeight no definido, lo dejamos en -1 y el display lo marcará
  if (startWeight < 0) {
    // si deseas fijar startWeight al primer valor lectura, descomenta:
    // startWeight = totalWeight;
  }

  // Si el botón está presionado en el arranque -> tare y guardar startWeight
  // Botón activo LOW
  if (digitalRead(BUTTON_PIN) == LOW) {
    // Espera breve para debounce y confirmar
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      // Long press detection (2s) para tare
      unsigned long t0 = millis();
      while (digitalRead(BUTTON_PIN) == LOW && millis() - t0 < 2000) {
        delay(10);
      }
      if (millis() - t0 >= 2000) {
        // Long press -> tare
        tareScales();
        // actualizar pesos tras tare
        w1 = readScaleUnits(scale1, 5);
        w2 = readScaleUnits(scale2, 5);
        totalWeight = w1 + w2;
      } else {
        // Short press -> solo mostrar (no tare). We'll keep awake for a short period to show.
        Serial.println("Boton corto: mostrando info y no taring.");
      }
    }
  }

  // Calcular duty según peso total
  uint8_t duty = weightToDuty(totalWeight);

  // Si peso <= target, enviar duty máximo para enfriar
  if (totalWeight <= TARGET_WEIGHT) duty = 255;

  // Enviar al ATtiny
  sendDutyToAttiny(duty);

  // Actualizar LEDs y OLED
  updateLEDs(totalWeight);
  showOLED(w1, w2, totalWeight, tempDHT, humDHT);

  // Si boton fue corto (solo mostrar) -> quedamos awake durante un tiempo para ver la pantalla
  bool shortShow = false;
  if (digitalRead(BUTTON_PIN) == LOW) {
    // si queremos mostrar más tiempo
    shortShow = true;
  }

  if (shortShow) {
    // mostrar 20s y luego dormir
    delay(20000);
  }

  // Preparar deep sleep: si prefieres que el botón pueda despertar, habilitar ext0 wakeup
  // ext0 requiere que el pin esté RTC-capable (ej. GPIO33). wake on LOW
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  // timer wake
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);

  Serial.printf("Entrando en deep sleep por %d s...\n", TIME_TO_SLEEP);
  delay(200);
  esp_deep_sleep_start();
}

// LOOP (no usado)
void loop() {
  // no llega aquí; ESP32 reinicia desde setup() en cada wake
}
