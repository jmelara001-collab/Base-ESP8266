#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <IO7F8266.h>     

// --- PROTOTIPOS DE FUNCIONES ---
void handleNetworkTask();
void publishData();
void handleUserMeta();
void handleUserCommand(char* topic, JsonDocument* root);

// --- VARIABLES PARA EL CONTROL DE RED ---
unsigned long wifiDownMillis = 0;       
const unsigned long RESTART_TIMEOUT = 300000; // 5 minutos de espera offline antes de reiniciar radio

String user_html = "";  
char* ssid_pfix = (char*)"IOT_Device";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

uint32_t reconnecciones_wifi = 0;   
bool wifiWasConnected = false;      

// --- VARIABLES DE TU PROCESO ---
// (Agrega aquí las variables de tu propio proceso)
const int LED_PIN = 2;  // Se mantiene para indicar el envío de datos MQTT

// ---------------------------------------------------------------------------
// HANDLERS IO7
// ---------------------------------------------------------------------------
void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        if (pubInterval < 200) pubInterval = 200;
    }
    
    // (Agrega aquí el ajuste dinámico de parámetros de tu proceso)
}

void handleUserCommand(char* topic, JsonDocument* root) {
    // (Manejo de comandos entrantes de tu proceso)
}

// ---------------------------------------------------------------------------
// PUBLICACIÓN DE DATOS MQTT 
// ---------------------------------------------------------------------------
void publishData() {
    StaticJsonDocument<768> root; 
    JsonObject data = root.createNestedObject("d");

    // --- VARIABLES DE TU PROCESO ---
    // Ejemplo: data["mi_sensor"] = valor_sensor;
    
    // --- VARIABLES DE ESTADO ORIGINAL ---
    data["uptime"] = millis() / 1000;          
    data["reconn"] = reconnecciones_wifi;     
    data["heap"]   = ESP.getFreeHeap();        
    data["wifi_ok"]   = (WiFi.status() == WL_CONNECTED);
    data["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    data["status"] = "Online";

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            digitalWrite(LED_PIN, LOW); // Adaptado a lógica inversa de ESP8266 si es el LED integrado
            delay(50); 
            digitalWrite(LED_PIN, HIGH);
            Serial.printf("TX OK | Uptime: %lu\n", millis() / 1000);
        }
    }
}

// ---------------------------------------------------------------------------
// FUNCIÓN COOPERATIVA DE GESTIÓN DE WIFI Y MQTT
// ---------------------------------------------------------------------------
void handleNetworkTask() {
    bool mqtt_ok = (WiFi.status() == WL_CONNECTED) && client.connected();

    if (mqtt_ok) {
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            wifiDownMillis = 0; 
            Serial.println("[WIFI/MQTT] Conexión estable y operativa.");
        }
        client.loop(); // Mantiene viva la conexión MQTT
    } 
    else {
        if (wifiWasConnected) {
            reconnecciones_wifi++; 
            wifiWasConnected = false;
            wifiDownMillis = millis(); 
            Serial.println("[ALERTA] Enlace MQTT o WiFi perdido. Iniciando temporizador de tolerancia...");
        }

        if (WiFi.status() == WL_CONNECTED) {
            static uint32_t lastTry = 0;
            if (millis() - lastTry > 5000) {
                iot_connect(); // Función provista asumo por tu librería base
                lastTry = millis();
            }
        }

        if (wifiDownMillis != 0 && (millis() - wifiDownMillis > RESTART_TIMEOUT)) {
            Serial.println("[CRÍTICO] 5 minutos sin reportar datos. Reiniciando radio WiFi...");
            
            WiFi.disconnect(true); 
            delay(100); // En ESP8266 delay() alimenta el Watchdog automáticamente
            WiFi.mode(WIFI_OFF);   
            delay(100);
            WiFi.mode(WIFI_STA);   
            
            const char* ssid = cfg["ssid"] ? (const char*)cfg["ssid"] : nullptr;
            const char* pass = cfg["w_pw"] ? (const char*)cfg["w_pw"] : nullptr;
            WiFi.begin(ssid, pass); 

            wifiDownMillis = millis(); 
        }
    }

    if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
        publishData();
        lastPublishMillis = millis();
    }
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] Iniciando sistema");

    // --- CONFIGURACIÓN DE PINES DE TU PROCESO ---
    // (Tus pinMode van aquí)

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    handleUserMeta();

    if (pubInterval <= 0) pubInterval = defaultPubIntervalMs;

    const char* ssid = cfg["ssid"] ? (const char*)cfg["ssid"] : nullptr;
    const char* pass = cfg["w_pw"] ? (const char*)cfg["w_pw"] : nullptr;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    
    Serial.print("Conectando a WiFi inicial...");
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        Serial.print(".");
        timeout++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WIFI] ¡Conectado con éxito al inicio!");
        wifiWasConnected = true;
    } else {
        Serial.println("\n[WIFI] No se pudo conectar al inicio. Iniciando modo offline temporal.");
        wifiWasConnected = false;
        wifiDownMillis = millis(); 
    }

    // ELIMINADO: xTaskCreatePinnedToCore() ya no aplica aquí
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL 
// ---------------------------------------------------------------------------
void loop() {
    // 1. Llamada a la gestión de red cooperativa
    handleNetworkTask(); 

    // --- LÓGICA DE TU PROCESO AQUÍ ---
    // Este espacio queda libre para que programes tu aplicación
    
    
    yield();
}