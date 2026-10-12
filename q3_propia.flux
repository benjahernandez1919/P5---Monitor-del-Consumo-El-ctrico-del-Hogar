// GT5 - Consulta Flux 3 (propia)
// Equipo E07 | Proyecto P5 | Monitor de consumo eléctrico (ESP32 + INA219)
//
// PREGUNTA: ¿Cuántas lecturas de la última hora quedaron bajo el umbral de
// voltaje de 4,0 V, es decir, en condición de CONSUMO_ALTO según la FSM del nodo?
//
// Contexto: el encargado del laboratorio necesita detectar cuándo se conectan
// cargas excesivas o equipos defectuosos, que hacen saltar las protecciones
// térmicas de la sala. En el firmware, un voltaje de bus bajo 4,0 V
// (UMBRAL_VOLTAJE_BAJO) dispara el estado CONSUMO_ALTO; esta consulta cuenta
// cuántas muestras persistidas cumplieron esa condición.
//
// Nota: solo se cuentan lecturas válidas (el nodo omite "voltaje" cuando
// sensor_ok = 0). Si no hubo ninguna lectura bajo el umbral, count() no
// devuelve filas (resultado vacío = 0 eventos).

equipo   = "E07"     // valor de MQTT_USER en config.h
proyecto = "P5"      // valor de PROYECTO en config.h
nodo     = "esp32"   // valor de NODO en config.h
campo    = "voltaje" // V, mismo nombre que publica el ESP32
umbral   = 4.0       // V, igual a UMBRAL_VOLTAJE_BAJO del firmware

from(bucket: "mediciones")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "mediciones")
  |> filter(fn: (r) => r.equipo == equipo and
                       r.proyecto == proyecto and
                       r.nodo == nodo)
  |> filter(fn: (r) => r._field == campo)
  |> filter(fn: (r) => r._value < umbral)
  |> count()
  |> yield(name: "lecturas_consumo_alto")
