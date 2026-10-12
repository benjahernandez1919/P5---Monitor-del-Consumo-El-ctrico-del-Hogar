// GT5 - Consulta Flux 2: promedio del field principal en ventanas de 5 minutos
// Equipo E07 | Proyecto P5 | Monitor de consumo eléctrico (ESP32 + INA219)
// Solo válida para un field numérico.
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
  |> aggregateWindow(every: 5m, fn: mean, createEmpty: false)
  |> yield(name: "promedio_5m")
