// Plantilla de configuración. Copiar este archivo como "config.h" en esta misma carpeta
// y completar con los datos reales. config.h está en .gitignore y NO se sube al repositorio.
#ifndef CONFIG_H
#define CONFIG_H

const char* WIFI_SSID = "NOMBRE_DE_LA_RED";
const char* WIFI_PASS = "CLAVE_DE_LA_RED";

const char* MQTT_HOST = "IP_DEL_BROKER";
const int   MQTT_PORT = 1883;

const char* MQTT_USER = "E07";             // usuario MQTT del equipo (también forma el tópico)
const char* MQTT_PASS = "CLAVE_MQTT";

const char* PROYECTO  = "P5";
const char* NODO      = "esp32";

#endif
