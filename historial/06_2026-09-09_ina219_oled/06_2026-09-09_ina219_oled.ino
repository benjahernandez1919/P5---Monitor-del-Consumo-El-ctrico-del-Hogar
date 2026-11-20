#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- PINOUT ---
const int PIN_LED_ALERTA = 4;     // LED / Indicador de Alerta
const int PIN_BOTON_REARME = 5;   // Botón de rearme (PULL-UP)
// INA219 y OLED usan I2C por defecto: SDA=21, SCL=22

// --- PANTALLA OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- SENSOR INA219 ---
Adafruit_INA219 ina219;

// --- CALIBRACIÓN PROPIA DEL VOLTAJE (regresión lineal contra la fuente de referencia) ---
const float CAL_M = 0.995;
const float CAL_B = -0.229;

// --- ENUMERACIÓN DE ESTADOS (FSM) ---
enum SystemState {
  MEDICION,
  CONSUMO_ALTO,
  ERROR_SEGURO
};

SystemState estadoActual = MEDICION;

// --- VARIABLES DE TEMPORIZACIÓN Y CONTROL ---
unsigned long t_inicioEstado = 0;
const unsigned long TIMEOUT_FALLA_MS = 5000;
const unsigned long DEBOUNCE_DELAY_MS = 50;

// Control de refresco de OLED (no bloqueante, con millis())
unsigned long t_ultimaActualizacionOLED = 0;
const unsigned long OLED_REFRESH_MS = 200; // ajustable

// --- UMBRALES DE VOLTAJE (con histéresis para evitar parpadeo por ruido) ---
const float UMBRAL_VOLTAJE_BAJO = 4.0;     // Entra en alerta si el voltaje cae debajo de esto
const float UMBRAL_VOLTAJE_NORMAL = 4.2;   // Solo vuelve a normal si sube sobre esto

// Variables de medición (vienen del INA219)
float potenciaW = 0.0;
float voltajeV = 0.0;
float corriente_mA = 0.0;

// Lectura real desde el INA219, con corrección de calibración por regresión lineal
void leerSensorINA219() {
  float voltajeCrudo = ina219.getBusVoltage_V();
  voltajeV = (voltajeCrudo - CAL_B) / CAL_M; // calibración por regresión lineal
  corriente_mA = ina219.getCurrent_mA();
  potenciaW = ina219.getPower_mW() / 1000.0; // mW -> W
}

// Actualiza la pantalla OLED con las lecturas en tiempo real
void actualizarOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("Voltaje: ");
  display.print(voltajeV, 2);
  display.println(" V");

  display.setCursor(0, 16);
  display.print("Corriente: ");
  display.print(corriente_mA, 2);
  display.println(" mA");

  display.setCursor(0, 32);
  display.print("Potencia: ");
  display.print(potenciaW, 2);
  display.println(" W");

  display.display();
}

// Lectura de botón no bloqueante por detección de flanco con debounce
bool leerBotonRearme() {
  static int estadoEstable = HIGH;
  static int ultimoLecturaPin = HIGH;
  static unsigned long t_ultimoRebote = 0;
  bool pulsacionDetectada = false;

  int lecturaActual = digitalRead(PIN_BOTON_REARME);

  if (lecturaActual != ultimoLecturaPin) {
    t_ultimoRebote = millis();
  }

  if ((millis() - t_ultimoRebote) > DEBOUNCE_DELAY_MS) {
    if (lecturaActual != estadoEstable) {
      estadoEstable = lecturaActual;
      if (estadoEstable == LOW) {
        pulsacionDetectada = true;
      }
    }
  }

  ultimoLecturaPin = lecturaActual;
  return pulsacionDetectada;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_ALERTA, OUTPUT);
  pinMode(PIN_BOTON_REARME, INPUT_PULLUP);

  digitalWrite(PIN_LED_ALERTA, LOW);
  t_inicioEstado = millis();

  if (!ina219.begin()) {
    Serial.println("No se detectó el INA219. Revisa las conexiones SDA/SCL.");
    while (1) { /* sin delay */ }
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("No se detectó la pantalla OLED. Revisa la dirección I2C.");
    while (1) { /* sin delay */ }
  }
  display.clearDisplay();
  display.display();
}

void loop() {
  // 1. Lectura continua y no bloqueante
  leerSensorINA219();

  // 2. Refresco de OLED cada OLED_REFRESH_MS (no bloqueante)
  if (millis() - t_ultimaActualizacionOLED >= OLED_REFRESH_MS) {
    t_ultimaActualizacionOLED = millis();
    actualizarOLED();
  }

  Serial.print("V: ");
  Serial.print(voltajeV, 2);
  Serial.print("  I: ");
  Serial.print(corriente_mA, 2);
  Serial.print("mA  P: ");
  Serial.print(potenciaW, 2);
  Serial.println("W");

  // 3. Máquina de Estados Finita (basada en VOLTAJE, con histéresis)
  switch (estadoActual) {

    case MEDICION:
      digitalWrite(PIN_LED_ALERTA, LOW);

      if (voltajeV < UMBRAL_VOLTAJE_BAJO) {
        Serial.println("[TRANSICION] Voltaje bajo detectado.");
        estadoActual = CONSUMO_ALTO;
        t_inicioEstado = millis();
      }
      break;

    case CONSUMO_ALTO:
      digitalWrite(PIN_LED_ALERTA, HIGH);

      if (voltajeV >= UMBRAL_VOLTAJE_NORMAL) {
        Serial.println("[TRANSICION] Retorno a Medición Normal.");
        estadoActual = MEDICION;
        t_inicioEstado = millis();
        break;
      }

      if ((millis() - t_inicioEstado) > TIMEOUT_FALLA_MS) {
        Serial.println("[TIMEOUT] Límite de tiempo excedido en Consumo Alto.");
        estadoActual = ERROR_SEGURO;
        t_inicioEstado = millis();
      }
      break;

    case ERROR_SEGURO:
      digitalWrite(PIN_LED_ALERTA, HIGH);

      if (leerBotonRearme()) {
        Serial.println("[REARME] Botón accionado. Restableciendo a MEDICION.");
        estadoActual = MEDICION;
        t_inicioEstado = millis();
      }
      break;
  }
}
