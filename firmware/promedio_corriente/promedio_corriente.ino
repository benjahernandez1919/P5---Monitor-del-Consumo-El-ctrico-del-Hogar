// Sketch aparte SOLO para sacar el promedio de corriente del INA219 y compararlo con el multímetro.
// No usa WiFi ni MQTT. Muestra en el OLED y en el Monitor Serie (115200):
//   - lectura instantánea
//   - promedio de los últimos 10 s
//   - promedio total desde que se encendió (o desde que se pulsó el botón)
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

const int PIN_BOTON = 5;                 // el mismo botón de rearme: reinicia el promedio total
const uint32_t PERIODO_LECTURA_MS = 100; // 10 lecturas por segundo
const uint32_t PERIODO_OLED_MS    = 500;
const uint8_t  N_VENTANA = 100;          // 100 lecturas x 0,1 s = 10 s

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_INA219 ina219;

float ventana[N_VENTANA];
uint8_t idx = 0, nVentana = 0;
double sumaTotal = 0;
uint32_t nTotal = 0;
float instantanea = NAN, promedio10s = NAN, promedioTotal = NAN;
float minimo = NAN, maximo = NAN;
uint32_t tLectura = 0, tOled = 0;

void reiniciar() {
  idx = 0; nVentana = 0; sumaTotal = 0; nTotal = 0;
  promedio10s = promedioTotal = minimo = maximo = NAN;
  Serial.println("--- promedio reiniciado ---");
}

void leer() {
  float i = ina219.getCurrent_mA();
  if (!ina219.success() || isnan(i)) { Serial.println("[INA219] lectura invalida"); return; }
  instantanea = i;

  ventana[idx] = i;
  idx = (idx + 1) % N_VENTANA;
  if (nVentana < N_VENTANA) nVentana++;
  float s = 0;
  for (uint8_t k = 0; k < nVentana; k++) s += ventana[k];
  promedio10s = s / nVentana;

  sumaTotal += i; nTotal++;
  promedioTotal = sumaTotal / nTotal;
  if (isnan(minimo) || i < minimo) minimo = i;
  if (isnan(maximo) || i > maximo) maximo = i;
}

void mostrar() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.print("Inst:  "); display.print(instantanea, 1);   display.println(" mA");
  display.setCursor(0, 14); display.print("P 10s: "); display.print(promedio10s, 1);   display.println(" mA");
  display.setCursor(0, 28); display.print("P tot: "); display.print(promedioTotal, 1); display.println(" mA");
  display.setCursor(0, 42); display.print("Min/Max: "); display.print(minimo, 0); display.print("/"); display.println(maximo, 0);
  display.setCursor(0, 54); display.print("n = "); display.println(nTotal);
  display.display();

  Serial.printf("inst=%.1f  prom10s=%.1f  promTotal=%.1f  min=%.1f  max=%.1f  n=%lu\n",
                instantanea, promedio10s, promedioTotal, minimo, maximo, (unsigned long)nTotal);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOTON, INPUT_PULLUP);
  if (!ina219.begin()) Serial.println("[INA219] no detectado");
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("[OLED] no detectada");
  display.clearDisplay(); display.display();
}

void loop() {
  uint32_t ahora = millis();
  if (ahora - tLectura >= PERIODO_LECTURA_MS) { tLectura = ahora; leer(); }
  if (ahora - tOled >= PERIODO_OLED_MS) { tOled = ahora; mostrar(); }

  static int anterior = HIGH;
  static uint32_t tBoton = 0;
  int b = digitalRead(PIN_BOTON);
  if (b == LOW && anterior == HIGH && ahora - tBoton > 200) { tBoton = ahora; reiniciar(); }
  anterior = b;
}
