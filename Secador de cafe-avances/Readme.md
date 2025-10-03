Archivo: Avances_secador.ino
Autores: Brayan Casanova Fernandez, Valentina Tobar Monsalve, Samuel Zuñiga Bolaños
Descripción: Sistema ESP32 integrado para secado de café con múltiples sensores y control automático

Macros y Constantes 
Macro	Valor	Descripción
TARGET_WEIGHT	104.0f	Peso objetivo del café en gramos Avances_secador.ino:31
DHTPIN	15	Pin del sensor DHT22 Avances_secador.ino:32
HX_DOUT	26	Pin de datos del HX711 Avances_secador.ino:37
HX_SCK	14	Pin de reloj del HX711 Avances_secador.ino:38
DS18B20_PIN	32	Pin del sensor de temperatura DS18B20 Avances_secador.ino:42
SLEEP_SECONDS	300	Tiempo de deep sleep en segundos (5 min) Avances_secador.ino:78
Variables Globales 
Variables Persistentes RTC
Variable	Tipo	Descripción
calibrated	RTC_DATA_ATTR bool	Estado de calibración del sistema Avances_secador.ino:89
calibration_factor	RTC_DATA_ATTR float	Factor de calibración de la báscula Avances_secador.ino:90
startWeight	RTC_DATA_ATTR float	Peso inicial del café en gramos Avances_secador.ino:91
lastLevel	RTC_DATA_ATTR uint8_t	Último nivel de control enviado Avances_secador.ino:92
savedOffset	RTC_DATA_ATTR long	Offset persistente de la báscula Avances_secador.ino:93
Objetos de Hardware
Objeto	Tipo	Descripción
dht	DHT	Sensor de temperatura y humedad ambiental Avances_secador.ino:34
scale	HX711	Módulo de báscula para peso Avances_secador.ino:39
ds18b20	DallasTemperature	Sensor de temperatura del café Avances_secador.ino:44
display	Adafruit_SSD1306	Display OLED para información Avances_secador.ino:51
rtc	RTC_DS1307	Reloj en tiempo real Avances_secador.ino:54
Funciones 
void setVectorLevel(uint8_t level)
Parámetros:

level - Nivel de control de 0 a 7
Descripción: Configura el vector de 3 bits para comunicación con Arduino Pro Mini. Los bits se mapean a los pines VEC_PIN0, VEC_PIN1, VEC_PIN2.

Implementación: Avances_secador.ino:106-112

float readWeightGrams(int samples = HX_SAMPLES)
Parámetros:

samples - Número de muestras para promedio (default: 10)
Retorna: float - Peso en gramos, NAN si el sensor no está listo

Descripción: Lee el peso de la báscula HX711 y convierte las unidades a gramos.

Implementación: Avances_secador.ino:115-122

uint8_t computeLevelFromWeightTemp(float weight_g, float tempCafeC)
Parámetros:

weight_g - Peso actual del café en gramos
tempCafeC - Temperatura del café en Celsius
Retorna: uint8_t - Nivel de control de 0 a 7

Descripción: Calcula el nivel de control basado en el peso actual y la temperatura del café. Aplica ajustes según rangos de temperatura definidos.

Implementación: Avances_secador.ino:125-139

void updateLEDsByWeight(float weight_g)
Parámetros:

weight_g - Peso actual en gramos
Descripción: Actualiza los LEDs de estado según el progreso del secado basado en el peso.

Lógica:

LED Rojo: Progreso < 30%
LED Amarillo: Progreso 30-80%
LED Verde: Progreso > 80% o peso objetivo alcanzado
Implementación: Avances_secador.ino:142-156

void showOLEDandSerial(float weight_g, float tempAmbC, float humPct, float tempCafeC, uint8_t level, DateTime now)
Parámetros:

weight_g - Peso en gramos
tempAmbC - Temperatura ambiental en Celsius
humPct - Humedad en porcentaje
tempCafeC - Temperatura del café en Celsius
level - Nivel de control actual
now - Timestamp actual del RTC
Descripción: Muestra información del sistema en display OLED y Serial Monitor.

Implementación: Avances_secador.ino:159-186

void autoTareOnce()
Descripción: Realiza calibración automática de la báscula en el primer encendido. Establece el punto cero y aplica el factor de calibración.

Efectos secundarios:

Modifica calibrated = true
Actualiza savedOffset
Implementación: Avances_secador.ino:189-200

void syncTimeToRTCIfWifi()
Descripción: Intenta conectar a WiFi y sincronizar el RTC DS1307 con servidor NTP. Timeout de 4 segundos para conexión WiFi.

Configuración WiFi: Avances_secador.ino:70-74

Implementación: Avances_secador.ino:203-233

void handleSerialCalibration()
Descripción: Maneja comandos de calibración desde Serial Monitor durante período activo.

Comandos:

+ o a: Incrementa factor de calibración
- o z: Decrementa factor de calibración
t: Realiza tare manual
Implementación: Avances_secador.ino:236-253

void setup()
Descripción: Función principal que ejecuta el ciclo completo del sistema. No hay función loop() porque el sistema usa deep sleep.

Flujo de ejecución:

Inicialización de hardware y sensores
Calibración automática si es primer encendido
Sincronización NTP opcional
Lectura de sensores y cálculo de nivel
Período activo de 5 segundos
Entrada a deep sleep por 5 minutos
Implementación: Avances_secador.ino:256-367

Configuración de Pines 
Función	Pin	Descripción
LED_RED	13	LED indicador rojo Avances_secador.ino:57
LED_YELLOW	2	LED indicador amarillo Avances_secador.ino:58
LED_GREEN	4	LED indicador verde Avances_secador.ino:59
BUTTON_PIN	33	Botón de despertar (RTC-capable) Avances_secador.ino:62
VEC_PIN0	16	Bit 0 del vector de control Avances_secador.ino:65
VEC_PIN1	17	Bit 1 del vector de control Avances_secador.ino:66
VEC_PIN2	5	Bit 2 del vector de control Avances_secador.ino:67
Dependencias 
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
Avances_secador.ino:17-28