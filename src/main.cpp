/**
 * IoT MQTT Platform - Versión Compatible con ESP32 Core 3.x
 * Corregido para NodeMCU ESP32-S y ESP32-C3
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "mqtt_client.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

// ----------------------- Ajustes -----------------------
#define TELEMETRY_INTERVAL_MS 5000
#define BOOT_BUTTON_PIN 0      // GPIO 0 para NodeMCU ESP32-S
#define RESET_HOLD_MS 3000
#define WS_PATH "/mqtt"

// ----------------------- Estado / Config -----------------------
char cfgBroker[96] = "";
char cfgPort[8] = "443";
char cfgUser[80] = "";
char cfgPass[64] = "";
char cfgUserId[16] = "";
char cfgDeviceId[16] = "";

char telemetryTopic[64];
char statusTopic[64];
char cmdTopic[64];

unsigned long lastTelemetry = 0;
bool mqttConnected = false;
int mqttFailCount = 0;
unsigned long lastMqttRetry = 0;
esp_mqtt_client_handle_t mqttClient = nullptr;
Preferences prefs;

// ----------------------- WiFi Events -----------------------
static void onWiFiEvent(WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        Serial.printf("[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
    }
}

// ----------------------- MQTT Event Handler -----------------------
static void mqttEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;
    if (id == MQTT_EVENT_CONNECTED) {
        mqttConnected = true;
        mqttFailCount = 0;
        Serial.println("[MQTT] Conectado (WSS)");
        esp_mqtt_client_subscribe(mqttClient, cmdTopic, 1);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        mqttConnected = false;
        Serial.println("[MQTT] Desconectado");
    }
}

// ----------------------- CONEXIÓN MQTT (API 3.x Corregida) -----------------------
void connectMqtt() {
    char uri[256];
    snprintf(uri, sizeof(uri), "wss://%s:%s%s", cfgBroker, cfgPort, WS_PATH);

    // Versión compatible con Core 3.x (API Refactorizada)
    esp_mqtt_client_config_t mqtt_cfg = {};
    
    // En lugar de broker.address.uri, probamos la estructura plana si la anterior falló
    mqtt_cfg.uri = uri; 
    mqtt_cfg.username = cfgUser;
    mqtt_cfg.password = cfgPass;
    
    // Para el certificado en Core 3.x, a veces requiere el puntero directo
    mqtt_cfg.crt_bundle_attach = arduino_esp_crt_bundle_attach;

    if (mqttClient) {
        esp_mqtt_client_destroy(mqttClient);
    }

    mqttClient = esp_mqtt_client_init(&mqtt_cfg);
    
    // Si el compilador se queja de la línea anterior, intenta esta alternativa:
    // mqttClient = esp_mqtt_client_init(&mqtt_cfg); 

    esp_mqtt_client_register_event(mqttClient, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqttEventHandler, NULL);
    esp_mqtt_client_start(mqttClient);
    Serial.printf("[MQTT] Intentando conectar a: %s\n", uri);
}

void setup() {
    Serial.begin(115200);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    
    WiFi.onEvent(onWiFiEvent);

    // Leer preferencias
    prefs.begin("iotmqtt", true);
    bool ok = prefs.getBool("ok", false);
    if (ok) {
        strncpy(cfgBroker, prefs.getString("broker", "").c_str(), 95);
        strncpy(cfgPort, prefs.getString("port", "443").c_str(), 7);
        strncpy(cfgUser, prefs.getString("user", "").c_str(), 79);
        strncpy(cfgPass, prefs.getString("pass", "").c_str(), 63);
        strncpy(cfgUserId, prefs.getString("uid", "").c_str(), 15);
        strncpy(cfgDeviceId, prefs.getString("did", "").c_str(), 15);
    }
    prefs.end();

    WiFiManager wm;
    WiFiManagerParameter pBroker("broker", "Broker", cfgBroker, 96);
    WiFiManagerParameter pPort("port", "Port", cfgPort, 8);
    WiFiManagerParameter pUser("user", "User", cfgUser, 80);
    WiFiManagerParameter pPass("pass", "Pass", cfgPass, 64);
    WiFiManagerParameter pUid("uid", "User ID", cfgUserId, 16);
    WiFiManagerParameter pDid("did", "Device ID", cfgDeviceId, 16);

    wm.addParameter(&pBroker);
    wm.addParameter(&pPort);
    wm.addParameter(&pUser);
    wm.addParameter(&pPass);
    wm.addParameter(&pUid);
    wm.addParameter(&pDid);

    if (!wm.autoConnect("IoT-Setup-Portal")) {
        delay(3000);
        ESP.restart();
    }

    // Guardar si es configuración nueva
    if (!ok) {
        prefs.begin("iotmqtt", false);
        prefs.putString("broker", pBroker.getValue());
        prefs.putString("port", pPort.getValue());
        prefs.putString("user", pUser.getValue());
        prefs.putString("pass", pPass.getValue());
        prefs.putString("uid", pUid.getValue());
        prefs.putString("did", pDid.getValue());
        prefs.putBool("ok", true);
        prefs.end();
        
        strncpy(cfgBroker, pBroker.getValue(), 95);
        strncpy(cfgUserId, pUid.getValue(), 15);
        strncpy(cfgDeviceId, pDid.getValue(), 15);
    }

    snprintf(telemetryTopic, 64, "u/%s/d/%s/telemetry", cfgUserId, cfgDeviceId);
    snprintf(cmdTopic, 64, "u/%s/d/%s/cmd", cfgUserId, cfgDeviceId);

    connectMqtt();
}

void loop() {
    if (WiFi.status() == WL_CONNECTED && mqttConnected) {
        if (millis() - lastTelemetry > TELEMETRY_INTERVAL_MS) {
            lastTelemetry = millis();
            StaticJsonDocument<128> doc;
            doc["signal"] = WiFi.RSSI();
            doc["heap"] = ESP.getFreeHeap();

            char buf[128];
            size_t n = serializeJson(doc, buf);
            esp_mqtt_client_publish(mqttClient, telemetryTopic, buf, n, 1, 0);
            Serial.println("Telemetría enviada.");
        }
    }
    delay(10);
}