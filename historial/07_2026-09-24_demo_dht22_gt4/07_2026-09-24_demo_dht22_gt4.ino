#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

/* ---------------- 1. CONFIGURACION ---------------- */
const int PIN_LED_ALERTA = 4;
const int PIN_BOTON_REARME = 5;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

const uint32_t PERIODO_LECTURA_MS = 500;    // el INA219 lee bastante más rápido que el DHT22
const uint32_t PERIODO_PUB_MS     = 10000;  // publicar cada 10s (mínimo del curso: 5s)
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

String clientId, topicDatos, topicEstado, topicCmd;

uint32_t tLectura = 0, tPub = 0, tWiFi = 0, tReconexion = 0;
uint32_t esperaReconexion = ESPERA_INICIAL;
unsigned long t_inicioEstado = 0;
unsigned long t_ultimaActualizacionOLED = 0;

float voltajeV = NAN, corriente_mA = NAN, potenciaW = NAN;   // última lectura VÁLIDA
uint8_t fallosSeguidos = MAX_FALLOS;   // hasta la primera lectura buena, el sensor no está OK
bool sensorOk = false;

enum SystemState { MEDICION, CONSUMO_ALTO, ERROR_SEGURO };
SystemState estadoActual = MEDICION;

const char* nombreEstado() {
  switch (estadoActual) {
    case MEDICION: return "MEDICION";
    case CONSUMO_ALTO: return "CONSUMO_ALTO";
    case ERROR_SEGURO: return "ERROR_SEGURO";
  }
  return "DESCONOCIDO";
}

/* ---------------- 3. FUNCIONES AUXILIARES ---------------- */

// 3.1 Lectura del INA219, independiente de la red
void leerINA219() {
  if (!ina219.success()) {              // si el I2C viene fallando, reintenta el begin()
    if (fallosSeguidos < 255) fallosSeguidos++;
    Serial.printf("[INA219] lectura inválida (%u seguidas)\n", (unsigned)fallosSeguidos);
    sensorOk = (fallosSeguidos < MAX_FALLOS);
    return;
  }
  float voltajeCrudo = ina219.getBusVoltage_V();
  voltajeV = (voltajeCrudo - CAL_B) / CAL_M;
  corriente_mA = ina219.getCurrent_mA();
  potenciaW = ina219.getPower_mW() / 1000.0;
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

// 3.3 WiFi sin bloquear y sin cortar una asociación en curso
void mantenerWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  uint32_t ahora = millis();
  if (ahora - tWiFi < REINTENTO_WIFI_MS) return;
  tWiFi = ahora;
  Serial.println("[wifi] sin red, reintentando...");
  WiFi.reconnect();
}

// 3.4 MQTT sin bloquear el lazo, con testamento y espera creciente
void mantenerMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t ahora = millis();
  if (ahora - tReconexion < esperaReconexion) return;
  tReconexion = ahora;

  Serial.printf("[mqtt] conectando como %s ... ", clientId.c_str());
  // Testamento: si el nodo desaparece, el BROKER publica "offline" (QoS 1, retenido)
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                    topicEstado.c_str(), 1, true, "offline")) {
    Serial.println("OK");
    mqtt.publish(topicEstado.c_str(), nombreEstado(), true);  // estado inicial: retenido
    mqtt.subscribe(topicCmd.c_str(), 1);                      // resuscribir en CADA reconexión
    esperaReconexion = ESPERA_INICIAL;
  } else {
    Serial.printf("FALLO rc=%d, reintento en %us\n",
                  mqtt.state(), (unsigned)(esperaReconexion / 1000));
    esperaReconexion = (esperaReconexion * 2 > ESPERA_MAXIMA) ? ESPERA_MAXIMA : esperaReconexion * 2;
  }
}

// 3.5 Publicación: JSON plano, datos calibrados, sensor_ok si el INA219 falla
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

  if (mqtt.publish(topicDatos.c_str(), (const uint8_t*)payload, n, true)) { // retained=true
    Serial.printf("[pub] %s -> %s\n", topicDatos.c_str(), payload);
  } else {
    Serial.println("[pub] ERROR publish() (buffer o sesion)");
  }

  // Actualiza el estado retenido de la FSM en cada publicación
  mqtt.publish(topicEstado.c_str(), nombreEstado(), true);
}

/* ---------------- 4. PROGRAMA ---------------- */
void setup() {
  Serial.begin(115200);
  delay(500); // solo en setup

  pinMode(PIN_LED_ALERTA, OUTPUT);
  pinMode(PIN_BOTON_REARME, INPUT_PULLUP);
  digitalWrite(PIN_LED_ALERTA, LOW);
  t_inicioEstado = millis();

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
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) { // espera acotada
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
}