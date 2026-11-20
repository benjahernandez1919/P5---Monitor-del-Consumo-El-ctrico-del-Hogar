#include <Arduino.h>

// --- PINOUT ---
const int PIN_ACTUADOR = 2;       // Relé / Actuador principal
const int PIN_LED_ALERTA = 4;     // LED / Indicador de Alerta
const int PIN_BOTON_REARME = 5;   // Botón de rearme (PULL-UP)
const int PIN_SENSOR_POTENCIA = A0; // Entrada analógica del sensor

// --- PARÁMETROS DE CALIBRACIÓN FÍSICA ---
const float M_FISICO = 0.98;
const float B_FISICO = 1.25;

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

// Variables de medición
float potenciaW = 0.0;
float voltajeV = 220.0; // En un sistema real se mediría mediante ADC

// Lectura y calibración del sensor físico
void leerSensorFisico() {
  int rawADC = analogRead(PIN_SENSOR_POTENCIA);
  
  // Conversión de ADC a potencia base (escalado según hardware, ej. 0-1023 -> 0-500W)
  float potenciaBase = (rawADC / 1023.0) * 500.0; 

  // Aplicación de la recta de calibración física
  potenciaW = (M_FISICO * potenciaBase) + B_FISICO;
}

// Lectura de botón no bloqueante por detección de flanco con debounce
bool leerBotonRearme() {
  static int estadoEstable = HIGH;
  static int ultimoLecturaPin = HIGH;
  static unsigned long t_ultimoRebote = 0;
  bool pulsacionDetectada = false;

  int lecturaActual = digitalRead(PIN_BOTON_REARME);

  // Si cambia la lectura del pin, reinicia el temporizador
  if (lecturaActual != ultimoLecturaPin) {
    t_ultimoRebote = millis();
  }

  // Si la lectura se mantiene estable más allá de la ventana de debounce
  if ((millis() - t_ultimoRebote) > DEBOUNCE_DELAY_MS) {
    if (lecturaActual != estadoEstable) {
      estadoEstable = lecturaActual;

      // Detección de flanco de bajada (HIGH -> LOW al presionar)
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
  
  pinMode(PIN_ACTUADOR, OUTPUT);
  pinMode(PIN_LED_ALERTA, OUTPUT);
  pinMode(PIN_BOTON_REARME, INPUT_PULLUP);

  // Estado inicial seguro
  digitalWrite(PIN_ACTUADOR, HIGH); // Carga conectada
  digitalWrite(PIN_LED_ALERTA, LOW);
  t_inicioEstado = millis();
}

void loop() {
  // 1. Lectura continua y no bloqueante
  leerSensorFisico();

  // 2. Máquina de Estados Finita
  switch (estadoActual) {
    
    // ----------------------------------------------------
    case MEDICION:
      digitalWrite(PIN_ACTUADOR, HIGH);
      digitalWrite(PIN_LED_ALERTA, LOW);

      // Condición de error lógico/físico
      if (isnan(potenciaW) || (voltajeV == 0.0 && potenciaW > 0.0)) {
        Serial.println("[ERROR] Condición física inválida detectada.");
        estadoActual = ERROR_SEGURO;
        t_inicioEstado = millis();
        break;
      }

      // Transición superior por histéresis (>= 230 W)
      if (potenciaW >= 230.0) {
        Serial.println("[TRANSICION] Consumo Alto detectado (>= 230W).");
        estadoActual = CONSUMO_ALTO;
        t_inicioEstado = millis();
      }
      break;

    // ----------------------------------------------------
    case CONSUMO_ALTO:
      digitalWrite(PIN_ACTUADOR, HIGH);
      digitalWrite(PIN_LED_ALERTA, HIGH); // Alerta activa

      // Transición inferior por histéresis (<= 200 W)
      if (potenciaW <= 200.0) {
        Serial.println("[TRANSICION] Retorno a Medición Normal (<= 200W).");
        estadoActual = MEDICION;
        t_inicioEstado = millis();
        break;
      }

      // Disparo de falla por tiempo prolongado en consumo alto
      if ((millis() - t_inicioEstado) > TIMEOUT_FALLA_MS) {
        Serial.println("[TIMEOUT] Límite de tiempo excedido en Consumo Alto.");
        estadoActual = ERROR_SEGURO;
        t_inicioEstado = millis();
      }
      break;

    // ----------------------------------------------------
    case ERROR_SEGURO:
      // FAIL-SAFE: Desconexión de carga y alerta permanente
      digitalWrite(PIN_ACTUADOR, LOW);
      digitalWrite(PIN_LED_ALERTA, HIGH);

      // Atención al rearme manual sin bloqueo
      if (leerBotonRearme()) {
        Serial.println("[REARME] Botón accionado. Restableciendo a MEDICION.");
        estadoActual = MEDICION;
        t_inicioEstado = millis();
      }
      break;
  }
}
