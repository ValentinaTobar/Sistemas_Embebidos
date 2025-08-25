#include "StateMachineLib.h"
#include <DHT.h>
#include <AsyncTaskLib.h>

// ---------------- Pines ----------------
#define PIN_DHT 5
#define DHT_TYPE DHT11
#define PIN_LDR 34
#define PIN_LED_AZUL 4
#define PIN_LED_ROJO 2
#define PIN_BTN_A 27
#define PIN_BTN_B 26

// ---------------- Objetos ----------------
DHT dht(PIN_DHT, DHT_TYPE);

// ---------------- Estados ----------------
enum MachineState {
  ST_INICIO = 0,
  ST_MON_TH = 1,
  ST_ALERTA = 2,
  ST_MON_LUZ = 3,
  ST_ALARMA = 4,
};

enum Event {
  EVT_TIMEOUT = 0,
  EVT_TEMP_ALTA,
  EVT_LUZ_ALTA,
  EVT_BTN_B,
  EVT_BTN_A,
  EVT_NONE
};

Event currentEvent = EVT_NONE;

StateMachine fsm(5, 9);

// ---------------- Variables ----------------
bool ledAzulOn = false;
bool ledRojoOn = false;

// ---------------- Tareas ----------------
void toggleLedAzul();
void toggleLedRojo();
AsyncTask tLedAzul(100, true, toggleLedAzul);
AsyncTask tLedRojo(200, true, toggleLedRojo);

void checkTimeout();
AsyncTask tTimeout(0, false, checkTimeout);

void readTempHum();
AsyncTask tTemp(1000, true, readTempHum);

void readLuz();
AsyncTask tLuz(1000, true, readLuz);

void checkButtons();
AsyncTask tBotones(100, true, checkButtons);

// ---------------- Funciones de eventos ----------------
void checkTimeout() { currentEvent = EVT_TIMEOUT; }

void readTempHum() {
  float temp = dht.readTemperature();
  if (isnan(temp)) return;
  Serial.print("[TH] Temp: "); Serial.println(temp);
  if (temp > 27 && fsm.GetState() == ST_MON_TH) currentEvent = EVT_TEMP_ALTA;
}

void readLuz() {
  int valor = analogRead(PIN_LDR);
  Serial.print("[LUX] Luz: "); Serial.println(valor);
  if (valor > 2000 && fsm.GetState() == ST_MON_LUZ) currentEvent = EVT_LUZ_ALTA;
}

void checkButtons() {
  if (digitalRead(PIN_BTN_A) == LOW && fsm.GetState() == ST_ALERTA) {
    currentEvent = EVT_BTN_A;
  }
  if (digitalRead(PIN_BTN_B) == LOW && fsm.GetState() == ST_ALARMA) {
    currentEvent = EVT_BTN_B;
  }
}

// ---------------- Acciones al entrar en estados ----------------
void onInicio() {
  currentEvent = EVT_NONE;
  tTimeout.SetIntervalMillis(5000); tTimeout.Start();
  tLedAzul.Stop(); tLedRojo.Stop();
  digitalWrite(PIN_LED_AZUL, LOW);
  digitalWrite(PIN_LED_ROJO, LOW);
  Serial.println("==> Estado: INICIO");
}

void onMonTH() {
  currentEvent = EVT_NONE;
  tTimeout.SetIntervalMillis(7000); tTimeout.Start();
  tLedAzul.Stop(); tLedRojo.Stop();
  digitalWrite(PIN_LED_AZUL, LOW);
  digitalWrite(PIN_LED_ROJO, LOW);
  Serial.println("==> Estado: MONITOREO TEMP/HUM");
}

void onAlerta() {
  currentEvent = EVT_NONE;
  tTimeout.SetIntervalMillis(4000); tTimeout.Start();
  tLedRojo.Stop(); digitalWrite(PIN_LED_ROJO, LOW);
  ledAzulOn = false; digitalWrite(PIN_LED_AZUL, LOW);
  tLedAzul.SetIntervalMillis(100); tLedAzul.Start();
  Serial.println("==> Estado: ALERTA (TEMP > 27°C)");
}

void onMonLuz() {
  currentEvent = EVT_NONE;
  tTimeout.SetIntervalMillis(2000); tTimeout.Start();
  tLedAzul.Stop(); tLedRojo.Stop();
  digitalWrite(PIN_LED_AZUL, LOW);
  digitalWrite(PIN_LED_ROJO, LOW);
  Serial.println("==> Estado: MONITOREO LUZ");
}

void onAlarma() {
  currentEvent = EVT_NONE;
  tTimeout.SetIntervalMillis(3000); tTimeout.Start();
  tLedAzul.Stop(); digitalWrite(PIN_LED_AZUL, LOW);
  ledRojoOn = false; digitalWrite(PIN_LED_ROJO, LOW);
  tLedRojo.SetIntervalMillis(200); tLedRojo.Start();
  Serial.println("==> Estado: ALARMA (LUZ > 1024)");
}

// ---------------- Blink LEDs ----------------
void toggleLedAzul() {
  ledAzulOn = !ledAzulOn;
  digitalWrite(PIN_LED_AZUL, ledAzulOn);
}
void toggleLedRojo() {
  ledRojoOn = !ledRojoOn;
  digitalWrite(PIN_LED_ROJO, ledRojoOn);
}

// ---------------- Setup de la FSM ----------------
void setupFSM() {
  fsm.AddTransition(ST_INICIO, ST_MON_TH, []() { return currentEvent == EVT_TIMEOUT; });

  fsm.AddTransition(ST_MON_TH, ST_ALERTA, []() { return currentEvent == EVT_TEMP_ALTA; });
  fsm.AddTransition(ST_ALERTA, ST_MON_TH, []() { return currentEvent == EVT_TIMEOUT; });
  fsm.AddTransition(ST_ALERTA, ST_INICIO, []() { return currentEvent == EVT_BTN_A; });

  fsm.AddTransition(ST_MON_TH, ST_MON_LUZ, []() { return currentEvent == EVT_TIMEOUT; });
  fsm.AddTransition(ST_MON_LUZ, ST_MON_TH, []() { return currentEvent == EVT_TIMEOUT; });
  fsm.AddTransition(ST_MON_LUZ, ST_ALARMA, []() { return currentEvent == EVT_LUZ_ALTA; });

  fsm.AddTransition(ST_ALARMA, ST_MON_LUZ, []() { return currentEvent == EVT_TIMEOUT; });
  fsm.AddTransition(ST_ALARMA, ST_INICIO, []() { return currentEvent == EVT_BTN_B; });

  fsm.SetOnEntering(ST_INICIO, onInicio);
  fsm.SetOnEntering(ST_MON_TH, onMonTH);
  fsm.SetOnEntering(ST_ALERTA, onAlerta);
  fsm.SetOnEntering(ST_MON_LUZ, onMonLuz);
  fsm.SetOnEntering(ST_ALARMA, onAlarma);
}

// ---------------- Setup & Loop ----------------
void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(PIN_LED_AZUL, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);

  setupFSM();
  fsm.SetState(ST_INICIO, false, true);

  tTemp.Start();
  tLuz.Start();
  tBotones.Start();
}

void loop() {
  tTimeout.Update();
  tTemp.Update();
  tLuz.Update();
  tLedAzul.Update();
  tLedRojo.Update();
  tBotones.Update();

  fsm.Update();
  currentEvent = EVT_NONE; // limpiar
}