// GT5 - Consulta Flux 1: valor más reciente del field principal
// Equipo E07 | Proyecto P5 | Monitor de consumo eléctrico (ESP32 + INA219)
// Ajustar estas variables a los valores EXACTOS observados en el Anexo A (A3)
equipo   = "E07"
proyecto = "P5"
nodo     = "esp32"
campo    = "potencia"

from(bucket: "mediciones")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "mediciones")
  |> filter(fn: (r) => r.equipo == equipo and
                       r.proyecto == proyecto and
                       r.nodo == nodo)
  |> filter(fn: (r) => r._field == campo)
  |> last()
