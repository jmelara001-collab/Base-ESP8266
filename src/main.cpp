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
const unsigned long RESTART_TIMEOUT = 300000; 

String user_html = "";  
char* ssid_pfix = (char*)"VMB-33";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

uint32_t reconnecciones_wifi = 0;   
bool wifiWasConnected = false;      

// --- VARIABLES DE TU PROCESO ---
const float VOLTAJE_MIN = 0.0; 
const float VOLTAJE_MAX = 3.3;

const float VALOR_MIN = 8.0;       
const float VALOR_MAX = 152.0;     

const int LED_PIN = 2;

// --- VARIABLES INTERNAS ---
int adc_raw = 0;
float voltaje = 0.0;
float medicion_final = 0.0;
unsigned long lastSensorReadMillis = 0;

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
    data["adc_raw"] = adc_raw;
    data["voltaje"] = voltaje;
    data["unidades"]  = medicion_final; 
    
    // --- VARIABLES DE ESTADO ORIGINAL ---
    data["uptime"]    = millis() / 1000;           
    data["reconn"]    = reconnecciones_wifi;     
    data["heap"]      = ESP.getFreeHeap();         
    data["wifi_ok"]   = (WiFi.status() == WL_CONNECTED);
    data["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    data["status"]    = "Online";

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            digitalWrite(LED_PIN, LOW); 
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
        client.loop(); 
    } 
    else {
        if (wifiWasConnected) {
            reconnecciones_wifi++; 
            wifiWasConnected = false;
            wifiDownMillis = millis(); 
            Serial.println("[ALERTA] Enlace MQTT o WiFi perdido...");
        }

        if (WiFi.status() == WL_CONNECTED) {
            static uint32_t lastTry = 0;
            if (millis() - lastTry > 5000) {
                iot_connect(); 
                lastTry = millis();
            }
        }

        if (wifiDownMillis != 0 && (millis() - wifiDownMillis > RESTART_TIMEOUT)) {
            Serial.println("[CRÍTICO] 5 minutos sin reportar datos. Reiniciando radio WiFi...");
            WiFi.disconnect(true); 
            delay(100); 
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
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL 
// ---------------------------------------------------------------------------
void loop() {
    handleNetworkTask(); 

    // --- LÓGICA DE TU PROCESO ---
    if (millis() - lastSensorReadMillis > 100) {
        
        adc_raw = analogRead(A0);      
        voltaje = adc_raw * (3.3 / 1023.0);
        
        medicion_final = ((voltaje - VOLTAJE_MIN) * ((VALOR_MAX - VALOR_MIN) / (VOLTAJE_MAX - VOLTAJE_MIN))) + VALOR_MIN;
        
        lastSensorReadMillis = millis();
    }
    
    yield();
}