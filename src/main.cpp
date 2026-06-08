#include <Arduino.h>
#include <WiFi.h>      
#include <IO7F32.h>    

// --- Declaración para el sensor de temperatura interno ---
#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif

// --- VARIABLES PARA EL CONTROL DE REINICIO AUTOMÁTICO ---
unsigned long wifiDownMillis = 0;       
const unsigned long RESTART_TIMEOUT = 300000; // 5 minutos de espera offline antes de reiniciar

// --- VARIABLES DE CONFIGURACIÓN DE USUARIO ---
String user_html = "<p><input type='text' name='meta.yourVar' placeholder='Your Custom Config'>";
int customVar1;
char* ssid_pfix = (char*)"IOT_DEVICE"; // Nombre del WiFi mantenido

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

// --- VARIABLES DEL RELÉ ---
const int RELAY = 18;
const int LED_PIN = 2;
bool relay_estado = false; // Indica si el relé está activado

uint32_t reconnecciones_wifi = 0;   
bool wifiWasConnected = false;      

// ---------------------------------------------------------------------------
// HANDLERS IO7
// ---------------------------------------------------------------------------
void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        if (pubInterval < 200) pubInterval = 200;
    }
    if (cfg["meta"].containsKey("yourVar")) {
        customVar1 = cfg["meta"]["yourVar"];
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {
    JsonObject d = (*root)["d"];

    // Si entra un comando para la válvula/relé
    if (d.containsKey("valve")) {
        if (strstr(d["valve"], "on")) {
            digitalWrite(RELAY, HIGH);
            relay_estado = true;
        } else {
            digitalWrite(RELAY, LOW);
            relay_estado = false;
        }
        // Forzar publicación inmediata al cambiar de estado para mayor respuesta
        lastPublishMillis = millis() - pubInterval; 
    }
}

// ---------------------------------------------------------------------------
// PUBLICACIÓN DE DATOS MQTT
// ---------------------------------------------------------------------------
void publishData() {
    StaticJsonDocument<512> root; 
    JsonObject data = root.createNestedObject("d");

    float temp_c = (temprature_sens_read() - 32) / 1.8;

    data["valve"] = relay_estado ? "on" : "off";
    data["temp"] = round(temp_c * 10.0) / 10.0; 
    data["uptime"] = millis() / 1000; // Tiempo que lleva encendido el ESP         
    data["reconn"] = reconnecciones_wifi;     
    data["heap"]   = ESP.getFreeHeap();       
    data["wifi_ok"]   = (WiFi.status() == WL_CONNECTED);
    data["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    data["status"] = "Online";

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            // INDICADOR VISUAL: Parpadea si se envió correctamente
            digitalWrite(LED_PIN, HIGH);
            delay(50); 
            digitalWrite(LED_PIN, LOW);
            Serial.printf("TX OK | Relé: %s | Temp ESP: %.1f C | Uptime: %u s\n", 
                          relay_estado ? "ON" : "OFF", temp_c, millis() / 1000);
        }
    }
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] Iniciando sistema de control de relé...");

    pinMode(RELAY, OUTPUT);
    digitalWrite(RELAY, LOW);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); 

    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    handleUserMeta();

    if (pubInterval <= 0) pubInterval = defaultPubIntervalMs;
    lastPublishMillis = millis() - pubInterval;

    const char* ssid = cfg["ssid"] ? (const char*)cfg["ssid"] : nullptr;
    const char* pass = cfg["w_pw"] ? (const char*)cfg["w_pw"] : nullptr;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    
    Serial.print("Conectando a WiFi...");
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        Serial.print(".");
        timeout++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WIFI] ¡Conectado con éxito!");
        wifiWasConnected = true;
    } else {
        Serial.println("\n[WIFI] No se pudo conectar al inicio.");
        wifiWasConnected = false;
        wifiDownMillis = millis(); 
    }
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL
// ---------------------------------------------------------------------------
void loop() {
    // 1. GESTIÓN DE CONEXIÓN
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            wifiDownMillis = 0; 
            Serial.println("[WIFI] Reconectado.");
        }

        if (!client.connected()) {
            static uint32_t lastTry = 0;
            if (millis() - lastTry > 5000) {
                iot_connect();
                lastTry = millis();
            }
        }
        client.loop();
    } 
    else {
        if (wifiWasConnected) {
            reconnecciones_wifi++; 
            wifiWasConnected = false;
            wifiDownMillis = millis();
            Serial.println("[WIFI] Conexión perdida...");
        }

        if (wifiDownMillis != 0 && (millis() - wifiDownMillis > RESTART_TIMEOUT)) {
            Serial.println("[ALERTA] Reiniciando por falta de red...");
            delay(1000);
            ESP.restart();
        }
    }

    // 2. PUBLICACIÓN PERIÓDICA
    if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
        publishData();
        lastPublishMillis = millis();
    }
}