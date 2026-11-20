#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include "config.h"

/* ---------------- 1. CONFIGURACION ---------------- */
const int PIN_LED_ALERTA = 4;
const int PIN_BOTON_REARME = 5;

// Servo MG996R de 360° (rotación continua): write() controla VELOCIDAD, no posición
const int PIN_SERVO = 18;                   // señal (cable naranjo)
const int SERVO_DETENIDO = 90;              // punto de parada
const int SERVO_VELOCIDAD = 180;            // 180 = máxima en un sentido, 0 = máxima en el otro
const unsigned long SERVO_RAMPA_MS = 5;     // arranque suave rápido: llega a full en ~0,5 s

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

const uint32_t PERIODO_LECTURA_MS = 500;
const uint32_t PERIODO_PUB_MS     = 30000;  // publicar cada 30s (mínimo del curso: 5s)
const uint32_t REINTENTO_WIFI_MS  = 15000;
const uint32_t ESPERA_INICIAL     = 2000;   // backoff MQTT: 2, 4, 8, 16, 30s
const uint32_t ESPERA_MAXIMA      = 30000;
const uint8_t  MAX_FALLOS         = 3;      // 3 lecturas inválidas seguidas => sensor_ok = 0

const unsigned long TIMEOUT_FALLA_MS = 5000;
const unsigned long DEBOUNCE_DELAY_MS = 50;
const unsigned long OLED_REFRESH_MS = 200;
const float UMBRAL_VOLTAJE_BAJO = 4.0;
const float UMBRAL_VOLTAJE_NORMAL = 4.2;

// Calibración (regresión lineal contra la fuente de referencia)
const float CAL_M = 0.995;
const float CAL_B = -0.229;

/* ---------------- 2. ESTADO INTERNO ---------------- */
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_INA219 ina219;
WiFiClient   red;
PubSubClient mqtt(red);
Servo servo;
int servoVelocidadActual = SERVO_DETENIDO;
unsigned long t_servo = 0;

String clientId, topicDatos, topicEstado, topicCmd;

uint32_t tLectura = 0, tPub = 0, tWiFi = 0, tReconexion = 0;
uint32_t esperaReconexion = ESPERA_INICIAL;
unsigned long t_inicioEstado = 0;
unsigned long t_ultimaActualizacionOLED = 0;

float voltajeV = NAN, corriente_mA = NAN, potenciaW = NAN;   // última lectura VÁLIDA
uint8_t fallosSeguidos = MAX_FALLOS;
bool sensorOk = false;

enum SystemState { MEDICION, CONSUMO_ALTO, ERROR_SEGURO };
SystemState estadoActual = MEDICION;
int8_t ultimoEstadoPub = -1;

const char* nombreEstado() {
  switch (estadoActual) {
    case MEDICION: return "MEDICION";
    case CONSUMO_ALTO: return "CONSUMO_ALTO";
    case ERROR_SEGURO: return "ERROR_SEGURO";
  }
  return "DESCONOCIDO";
}

const char* nombreEstadoMQTT() {
  return (estadoActual == MEDICION) ? "online" : nombreEstado();
}

/* ---------------- 3. FUNCIONES AUXILIARES ---------------- */

// 3.0 Giro continuo: acelera rápido hasta la velocidad máxima y se mantiene.
// La rampa corta evita el pico de corriente del arranque brusco, que puede resetear el ESP32.
void girarServo() {
  if (servoVelocidadActual == SERVO_VELOCIDAD) return;
  if (millis() - t_servo < SERVO_RAMPA_MS) return;
  t_servo = millis();
  servoVelocidadActual += (SERVO_VELOCIDAD > servoVelocidadActual) ? 1 : -1;
  servo.write(servoVelocidadActual);
}

// 3.1 Lectura del INA219, independiente de la red
void leerINA219() {
  float v = ina219.getBusVoltage_V();
  float i = ina219.getCurrent_mA();
  float p = ina219.getPower_mW();

  if (!ina219.success() || isnan(v) || isnan(i) || isnan(p)) {
    if (fallosSeguidos < 255) fallosSeguidos++;
    Serial.printf("[INA219] lectura inválida (%u seguidas)\n", (unsigned)fallosSeguidos);
    sensorOk = (fallosSeguidos < MAX_FALLOS);
    if (!sensorOk) ina219.begin();
    return;
  }

  voltajeV = (v - CAL_B) / CAL_M;
  corriente_mA = i;
  potenciaW = p / 1000.0;
  fallosSeguidos = 0;
  sensorOk = true;
}

void actualizarOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Voltaje: "); display.print(voltajeV, 2); display.println(" V");
  display.setCursor(0, 16);
  display.print("Corriente: "); display.print(corriente_mA, 2); display.println(" mA");
  display.setCursor(0, 32);
  display.print("Potencia: "); display.print(potenciaW, 2); display.println(" W");
  display.setCursor(0, 48);
  display.print("Estado: "); display.println(nombreEstado());
  display.display();
}

bool leerBotonRearme() {
  static int estadoEstable = HIGH;
  static int ultimoLecturaPin = HIGH;
  static unsigned long t_ultimoRebote = 0;
  bool pulsacionDetectada = false;
  int lecturaActual = digitalRead(PIN_BOTON_REARME);
  if (lecturaActual != ultimoLecturaPin) t_ultimoRebote = millis();
  if ((millis() - t_ultimoRebote) > DEBOUNCE_DELAY_MS) {
    if (lecturaActual != estadoEstable) {
      estadoEstable = lecturaActual;
      if (estadoEstable == LOW) pulsacionDetectada = true;
    }
  }
  ultimoLecturaPin = lecturaActual;
  return pulsacionDetectada;
}

// 3.2 Comandos entrantes (se usan en la Semana 11)
void recibirComando(char* topic, byte* payload, unsigned int largo) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, largo);
  if (error) {
    Serial.printf("[cmd] JSON invalido en %s: %s\n", topic, error.c_str());
    return;
  }
  Serial.printf("[cmd] recibido en %s\n", topic);
}

// 3.3 WiFi sin bloquear
void mantenerWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  uint32_t ahora = millis();
  if (ahora - tWiFi < REINTENTO_WIFI_MS) return;
  tWiFi = ahora;
  Serial.println("[wifi] sin red, reintentando...");
  WiFi.reconnect();
}

// Estado en /estado con RETAIN
bool publicarEstado() {
  if (!mqtt.connected()) return false;
  bool ok = mqtt.publish(topicEstado.c_str(), nombreEstadoMQTT(), true);
  if (ok) {
    ultimoEstadoPub = estadoActual;
    Serial.printf("[pub] %s -> %s (retained)\n", topicEstado.c_str(), nombreEstadoMQTT());
  } else {
    Serial.println("[pub] ERROR publicando estado");
  }
  return ok;
}

// 3.4 MQTT sin bloquear, con testamento y espera creciente
void mantenerMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t ahora = millis();
  if (ahora - tReconexion < esperaReconexion) return;
  tReconexion = ahora;

  Serial.printf("[mqtt] conectando como %s ... ", clientId.c_str());
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                    topicEstado.c_str(), 1, true, "offline")) {
    Serial.println("OK");
    publicarEstado();
    mqtt.subscribe(topicCmd.c_str(), 1);
    esperaReconexion = ESPERA_INICIAL;
  } else {
    Serial.printf("FALLO rc=%d, reintento en %us\n",
                  mqtt.state(), (unsigned)(esperaReconexion / 1000));
    esperaReconexion = (esperaReconexion * 2 > ESPERA_MAXIMA) ? ESPERA_MAXIMA : esperaReconexion * 2;
  }
}

// 3.5 Publicación: JSON plano con los datos calibrados
void publicarDatos() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  if (sensorOk) {
    doc["voltaje"] = roundf(voltajeV * 100.0f) / 100.0f;
    doc["corriente"] = roundf(corriente_mA * 100.0f) / 100.0f;
    doc["potencia"] = roundf(potenciaW * 100.0f) / 100.0f;
  }
  doc["sensor_ok"] = sensorOk ? 1 : 0;
  doc["rssi_dbm"] = WiFi.RSSI();

  char payload[256];
  size_t n = serializeJson(doc, payload, sizeof(payload));

  if (mqtt.publish(topicDatos.c_str(), (const uint8_t*)payload, n, true)) {
    Serial.printf("[pub] %s -> %s (retained)\n", topicDatos.c_str(), payload);
  } else {
    Serial.println("[pub] ERROR publish() (buffer o sesion)");
  }

  publicarEstado();
}

void publicarEstadoSiCambio() {
  if (ultimoEstadoPub == (int8_t)estadoActual) return;
  publicarEstado();
}

/* ---------------- 4. PROGRAMA ---------------- */
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_LED_ALERTA, OUTPUT);
  pinMode(PIN_BOTON_REARME, INPUT_PULLUP);
  digitalWrite(PIN_LED_ALERTA, LOW);
  t_inicioEstado = millis();

  ESP32PWM::allocateTimer(0);
  servo.setPeriodHertz(50);                       // servos: 50 Hz
  servo.attach(PIN_SERVO, 500, 2500);             // pulso 0,5 a 2,5 ms (rango completo del MG996R)
  servo.write(SERVO_DETENIDO);                    // parte detenido y luego acelera

  if (!ina219.begin()) {
    Serial.println("[INA219] no detectado al inicio; el nodo sigue e intentará reportarlo.");
  }
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("[OLED] no detectada.");
  }
  display.clearDisplay();
  display.display();

  clientId    = String(MQTT_USER) + "-" + NODO;
  topicDatos  = String("curso/") + MQTT_USER + "/" + PROYECTO + "/" + NODO;
  topicEstado = topicDatos + "/estado";
  topicCmd    = topicDatos + "/cmd";
  Serial.printf("[id] Client ID: %s\n[id] Datos    : %s\n", clientId.c_str(), topicDatos.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[wifi] IP = ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[wifi] sin red todavía; el nodo sigue midiendo y reintenta");
  }
  tWiFi = millis();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(recibirComando);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(3);
}

void loop() {
  mantenerWiFi();
  mantenerMQTT();
  mqtt.loop();

  uint32_t ahora = millis();
  if (ahora - tLectura >= PERIODO_LECTURA_MS) { tLectura = ahora; leerINA219(); }

  if (millis() - t_ultimaActualizacionOLED >= OLED_REFRESH_MS) {
    t_ultimaActualizacionOLED = millis();
    actualizarOLED();
  }

  if (ahora - tPub >= PERIODO_PUB_MS) { tPub = ahora; publicarDatos(); }
  publicarEstadoSiCambio();

  switch (estadoActual) {
    case MEDICION:
      digitalWrite(PIN_LED_ALERTA, LOW);
      if (sensorOk && voltajeV < UMBRAL_VOLTAJE_BAJO) {
        estadoActual = CONSUMO_ALTO;
        t_inicioEstado = millis();
      }
      break;

    case CONSUMO_ALTO:
      digitalWrite(PIN_LED_ALERTA, HIGH);
      if (voltajeV >= UMBRAL_VOLTAJE_NORMAL) {
        estadoActual = MEDICION;
        t_inicioEstado = millis();
        break;
      }
      if ((millis() - t_inicioEstado) > TIMEOUT_FALLA_MS) {
        estadoActual = ERROR_SEGURO;
        t_inicioEstado = millis();
      }
      break;

    case ERROR_SEGURO:
      digitalWrite(PIN_LED_ALERTA, HIGH);
      if (leerBotonRearme()) {
        estadoActual = MEDICION;
        t_inicioEstado = millis();
      }
      break;
  }

  girarServo();   // el MG996R gira continuo a máxima velocidad para generar carga
}