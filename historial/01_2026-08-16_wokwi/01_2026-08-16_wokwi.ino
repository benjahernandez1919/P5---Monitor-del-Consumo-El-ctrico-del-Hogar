/* ==========================================================================
   FUNDAMENTOS DE IoT - 2do SEMESTRE 2026
   GT1 (Semana 3) - Proyecto base: cadena de adquisicion en simulacion
   Universidad Autonoma de Chile - Ingenieria Civil Informatica
   v1.0 - 2026-2 - R. Rodriguez

   QUE HACE ESTE PROGRAMA
   Lee una senal analogica, la convierte a magnitud fisica, le aplica una
   correccion de dos puntos y la filtra. Imprime por el monitor serie, en
   formato CSV, el tiempo, las cuentas crudas, el valor calibrado y el valor
   filtrado, para poder comparar las tres columnas.

   COMO SE USA EN LA GT1
   1. Copien este proyecto en su propia cuenta de Wokwi y renombrenlo E NN-GT1.
   2. Ejecuten con M_CAL = 1.0 y B_CAL = 0.0 (sin corregir) y anoten la lectura
      en los DOS puntos de referencia que entrega el docente de la seccion.
   3. Despejen m y b (ver la funcion de ayuda al final del archivo), escriban
      los valores en M_CAL y B_CAL, y verifiquen en un TERCER punto.
   4. Registren m, b, la tolerancia declarada y el criterio de eleccion de N
      en el README del repositorio del equipo.

   REGLAS DE LA ASIGNATURA QUE ESTE CODIGO RESPETA
   - Sin delay() en el lazo principal: el muestreo se agenda con millis().
   - Solo canales del ADC1 (GPIO 32 a 39): el ADC2 queda inutilizable cuando
     el WiFi esta activo, y desde la Semana 8 el WiFi estara activo.
   - Toda constante fisica va comentada con su unidad y su origen.
   ========================================================================== */

// ---------------------------------------------------------------------------
// 1. Configuracion del equipo  (EDITEN ESTA SECCION)
// ---------------------------------------------------------------------------
const uint8_t  PIN_SENSOR   = 34;      // ADC1 canal 6. No usar GPIO 0-27 (ADC2).
const uint32_t PERIODO_MS   = 1000;    // [ms] Periodo de muestreo del taller.
                                       // El periodo del nodo real es el que
                                       // declararon en su plan de datos (ED).
const uint8_t  N_FILTRO     = 5;       // Ventana del filtro. Justifiquenla.
const bool     USAR_MEDIANA = false;   // false: media movil | true: mediana

// Constantes de conversion cuentas -> magnitud fisica.
// ORIGEN: hoja de datos del sensor. SUSTITUYAN por las suyas y documenten.
const float V_REF        = 3.3f;       // [V] tension de referencia del ADC
const uint16_t CUENTAS_MAX = 4095;     // ADC de 12 bits: 0 a 4095
const float ESCALA_SENSOR = 100.0f;    // [grados C / V] sensibilidad nominal
const float OFFSET_SENSOR = 0.0f;      // [grados C] a 0 V, segun hoja de datos



// ---------------------------------------------------------------------------
// ERROR SISTEMATICO SIMULADO
// ---------------------------------------------------------------------------
//
// Wokwi utiliza componentes practicamente ideales.
// Para reproducir el comportamiento de un sensor fisico introducimos
// deliberadamente un error sistematico.
//
// El "sensor imperfecto" simulado cumple:
//
//      M = GANANCIA_ERROR_SIM * valor_nominal + OFFSET_ERROR_SIM
//
// Ejemplo:
//
//      valor nominal = 20 grados C
//
//      M = 1.05 * 20 + 2
//      M = 23 grados C
//
// Por tanto, el sensor simulado medira sistematicamente diferente
// de la referencia.
//
// ESTOS VALORES SON PARTE DE LA SIMULACION.
// NO SON LOS VALORES DE CALIBRACION.
// ---------------------------------------------------------------------------

const bool SIMULAR_ERROR_SISTEMATICO = true;

const float GANANCIA_ERROR_SIM = 1.05f;  // +5 % de error de ganancia
const float OFFSET_ERROR_SIM = 2.0f;     // [grados C] error de offset


// Constantes de la CALIBRACION DE DOS PUNTOS (item 4 de la pauta).
// Se obtienen midiendo, no se copian de otro equipo.
const float M_CAL = 9.585f;            // ganancia  (adimensional)
const float B_CAL = -12.729f;            // offset    [grados C]




// Simulacion explicita del ruido que el ADC real si tiene y el simulador no.
// Se declara aqui porque en la Semana 4, con el sensor fisico, esta linea
// se elimina: el ruido deja de ser simulado y pasa a ser un problema real.
const bool  SIMULAR_RUIDO = true;
const float RUIDO_CUENTAS = 25.0f;     // [cuentas] amplitud del ruido simulado

// ---------------------------------------------------------------------------
// 2. Estado interno (no requiere edicion)
// ---------------------------------------------------------------------------
uint32_t t_ultima_muestra = 0;         // marca de tiempo de la ultima muestra
float    ventana[16];                  // buffer circular del filtro
uint8_t  idx_ventana = 0;
uint8_t  muestras_validas = 0;

// ---------------------------------------------------------------------------
// 3. Funciones auxiliares
// ---------------------------------------------------------------------------

// Convierte cuentas del ADC a magnitud fisica segun la hoja de datos.
// Entrada: cuentas [0..4095].  Salida: magnitud [grados C].
float cuentas_a_fisica(int cuentas) {
  float tension = (V_REF * cuentas) / CUENTAS_MAX;         // [V]
  return tension * ESCALA_SENSOR + OFFSET_SENSOR;          // [grados C]
}

// ---------------------------------------------------------------------------
// Simulacion del error sistematico
// ---------------------------------------------------------------------------
//
// Esta funcion "estropea" deliberadamente la medicion ideal para representar
// un sensor fisico imperfecto.
//
// Si la simulacion esta desactivada, devuelve el mismo valor recibido.
// ---------------------------------------------------------------------------

float simular_error_sistematico(float valor_nominal) {

  if (!SIMULAR_ERROR_SISTEMATICO) {
    return valor_nominal;
  }

  return
      GANANCIA_ERROR_SIM * valor_nominal
      + OFFSET_ERROR_SIM;
}


// Aplica la correccion de dos puntos:  y_corregido = m * y_medido + b
float aplicar_calibracion(float valor_medido) {
  return M_CAL * valor_medido + B_CAL;
}

// Media movil sobre la ventana.
float media_movil() {
  float suma = 0.0f;
  for (uint8_t i = 0; i < muestras_validas; i++) suma += ventana[i];
  return (muestras_validas > 0) ? (suma / muestras_validas) : 0.0f;
}

// Mediana sobre la ventana. Elimina picos aislados sin arrastrar el valor.
float mediana() {
  if (muestras_validas == 0) return 0.0f;
  float copia[16];
  for (uint8_t i = 0; i < muestras_validas; i++) copia[i] = ventana[i];
  for (uint8_t i = 1; i < muestras_validas; i++) {      // insercion simple
    float clave = copia[i];
    int8_t j = i - 1;
    while (j >= 0 && copia[j] > clave) { copia[j + 1] = copia[j]; j--; }
    copia[j + 1] = clave;
  }
  return copia[muestras_validas / 2];
}

// ---------------------------------------------------------------------------
// 4. Programa
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);            // 12 bits: 0 a 4095
  Serial.println("t_ms,cuentas,nominal_C,medido_C,calibrado_C,filtrado_C");  // cabecera CSV
}

void loop() {
  uint32_t ahora = millis();

  // Muestreo agendado: el lazo nunca se bloquea, de modo que mas adelante
  // puede convivir con la maquina de estados (Semana 4) y con la publicacion
  // MQTT (Semana 8) sin reescribir esta estructura.
  if (ahora - t_ultima_muestra >= PERIODO_MS) {
    t_ultima_muestra = ahora;

    int cuentas = analogRead(PIN_SENSOR);

    if (SIMULAR_RUIDO) {
      cuentas += (int)(random(-100, 101) / 100.0f * RUIDO_CUENTAS);
      cuentas = constrain(cuentas, 0, CUENTAS_MAX);
    }

    float valor_nominal    = cuentas_a_fisica(cuentas);        // conversion (item 3)

    float valor_medido = simular_error_sistematico (valor_nominal); 
    float valor_calibrado = aplicar_calibracion(valor_medido);      // correccion  (item 4)

    ventana[idx_ventana] = valor_calibrado;                  // filtrado    (item 5)
    idx_ventana = (idx_ventana + 1) % N_FILTRO;
    if (muestras_validas < N_FILTRO) muestras_validas++;

    float valor_filtrado = USAR_MEDIANA ? mediana() : media_movil();

    //Serial.printf("%lu,%d,%.2f,%.2f\n", ahora, cuentas, calibrado, valor_filtrado);
    Serial.printf(
    "%lu,%d,%.2f,%.2f,%.2f,%.2f\n",
    ahora,
    cuentas,
    valor_nominal,
    valor_medido,
    valor_calibrado,
    valor_filtrado
    );
  }
  // Aqui NO va delay(). El resto del lazo queda libre para las semanas
  // siguientes: maquina de estados, lectura de botones, publicacion MQTT.
}

/* ==========================================================================
   AYUDA PARA EL ITEM 4 - CALIBRACION DE DOS PUNTOS

   Con M_CAL = 1.0 y B_CAL = 0.0, midan en los dos puntos de referencia que
   entrega el docente:

       Referencia 1 = R1     Lectura obtenida = M1
       Referencia 2 = R2     Lectura obtenida = M2

   Entonces:

       m = (R2 - R1) / (M2 - M1)
       b =  R1 - m * M1

   Escriban m en M_CAL y b en B_CAL, vuelvan a simular y verifiquen en un
   TERCER punto de referencia. El error en ese tercer punto debe caer dentro
   de la tolerancia que su equipo declaro para el proyecto.

   ADVERTENCIA: si R1 y R2 estan muy proximos, (M2 - M1) es pequeno y la
   ganancia se dispara. Los dos puntos deben estar separados dentro del rango
   de trabajo del sensor.

   ADVERTENCIA 2: ajustar m y b hasta que el tercer punto "de bien" no es
   calibrar. Los dos puntos determinan la recta; el tercero solo la verifica.
   ========================================================================== */
