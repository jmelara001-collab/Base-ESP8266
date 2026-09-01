#include <Arduino.h>
#include <WiFi.h>      
#include <IO7F32.h>    

// --- PROTOTIPOS DE FUNCIONES ---
void core0NetworkTask(void * pvParameters);
void publishData();
void handleUserMeta();
void handleUserCommand(char* topic, JsonDocument* root);

// --- VARIABLES PARA EL CONTROL DE RED ---
unsigned long wifiDownMillis = 0;       
const unsigned long RESTART_TIMEOUT = 300000; // 5 minutos de espera offline (WiFi o SSL) antes de reiniciar radio

String user_html = "";  
char* ssid_pfix = (char*)"IOT_Device";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

uint32_t reconnecciones_wifi = 0;   
bool wifiWasConnected = false;      

// --- VARIABLES DE TU PROCESO ---
// (Agrega aquí las variables de tu propio proceso)
const int LED_PIN = 2; // Se mantiene para indicar el envío de datos MQTT

// --- TAREA PARA EL CORE 0 (RED) ---
TaskHandle_t NetworkTaskHandle;

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
// PUBLICACIÓN DE DATOS MQTT (Llamada desde la tarea de red)
// ---------------------------------------------------------------------------
void publishData() {
    StaticJsonDocument<768> root; 
    JsonObject data = root.createNestedObject("d");

    // --- VARIABLES DE TU PROCESO ---
    // Ejemplo: data["mi_sensor"] = valor_sensor;
    
    // --- VARIABLES DE ESTADO ORIGINAL (ESQUELETO) ---
    data["uptime"] = millis() / 1000;          
    data["reconn"] = reconnecciones_wifi;     
    data["heap"]   = ESP.getFreeHeap();        
    data["wifi_ok"]   = (WiFi.status() == WL_CONNECTED);
    data["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    data["status"] = "Online";

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            digitalWrite(LED_PIN, HIGH);
            delay(50); 
            digitalWrite(LED_PIN, LOW);
            Serial.printf("TX OK | Uptime: %lu\n", millis() / 1000);
        }
    }
}

// ---------------------------------------------------------------------------
// TAREA EXCLUSIVA DEL CORE 0: GESTIÓN DE WIFI Y MQTT
// ---------------------------------------------------------------------------
void core0NetworkTask(void * pvParameters) {
    Serial.printf("[CORE 0] Tarea de red iniciada en el núcleo: %d\n", xPortGetCoreID());
    
    for(;;) {
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
                Serial.println("[ALERTA] Enlace MQTT o WiFi perdido. Iniciando temporizador de tolerancia...");
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
                vTaskDelay(pdMS_TO_TICKS(100));
                WiFi.mode(WIFI_OFF);   
                vTaskDelay(pdMS_TO_TICKS(100));
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

        vTaskDelay(pdMS_TO_TICKS(10)); 
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
    digitalWrite(LED_PIN, LOW); 

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

    xTaskCreatePinnedToCore(
        core0NetworkTask,     
        "NetworkTask",        
        8192,                 
        NULL,                 
        1,                    
        &NetworkTaskHandle,   
        0                     
    );
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL 
// ---------------------------------------------------------------------------
void loop() {
    // --- LÓGICA DE TU PROCESO AQUÍ ---
    // Este espacio queda libre para que programes tu aplicación
    
    
    vTaskDelay(pdMS_TO_TICKS(10)); // Pequeño retardo recomendado si el loop está vacío o corre muy rápido
}