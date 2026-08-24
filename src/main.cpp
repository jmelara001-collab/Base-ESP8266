#include <Arduino.h>
#include <WiFi.h>      
#include <IO7F32.h>    

// ===========================================================================
// ⚙️ CONFIGURACIÓN DEL USUARIO (Variables físicas de la máquina)
// ===========================================================================

// 1. LÍMITE FÍSICO MÁXIMO (Para calcular el filtro anti-ruido)
float limite_ppm_max = 5000.0 / 60.0; 

// 2. PULSOS POR PRODUCTO
int pulsesPerRev = 1;        

// 3. MODO DE DETECCIÓN DEL SENSOR 
int modo_deteccion = RISING; 
// ===========================================================================

const int PIN_SENSOR = 18; 
const int LED_PIN = 2;     

void core0NetworkTask(void * pvParameters);
void publishData();
void handleUserMeta();
void handleUserCommand(char* topic, JsonDocument* root);
IRAM_ATTR void onPulse();

unsigned long wifiDownMillis = 0;       
const unsigned long RESTART_TIMEOUT = 300000; 

String user_html = "";  
char* ssid_pfix = (char*)"IOT_DEVICE";
unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000; 

// --- VARIABLES INTERNAS DE CONTEO ---
unsigned long debounceUs;    
volatile uint32_t pulsos_crudos = 0; 
volatile uint32_t contador_producto = 0; 
volatile unsigned long lastPulseTime = 0;

uint32_t reconnecciones_wifi = 0;   
bool wifiWasConnected = false;      
TaskHandle_t NetworkTaskHandle;

// ---------------------------------------------------------------------------
// INTERRUPCIÓN (ISR) - ACUMULADOR ABSOLUTO
// ---------------------------------------------------------------------------
IRAM_ATTR void onPulse() {
    unsigned long now = micros();
    
    // Filtro anti-ruido: ignora rebotes que ocurran más rápido que la velocidad máxima
    if (now - lastPulseTime > debounceUs) {
        pulsos_crudos++; 
        contador_producto = pulsos_crudos / pulsesPerRev; 
        lastPulseTime = now;
    }
}

// ---------------------------------------------------------------------------
// HANDLERS IO7
// ---------------------------------------------------------------------------
void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        if (pubInterval < 200) pubInterval = 200;
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {}

// ---------------------------------------------------------------------------
// PUBLICACIÓN DE DATOS MQTT 
// ---------------------------------------------------------------------------
void publishData() {
    // Redujimos a 512 porque el paquete ahora es más limpio y ligero
    StaticJsonDocument<512> root; 
    JsonObject data = root.createNestedObject("d");

    // Datos de producción puros
    data["conteo"] = contador_producto;
    data["pulsos_raw"] = pulsos_crudos; 
    
    // Datos útiles de diagnóstico para supervisión
    data["uptime"] = millis() / 1000;          
    data["reconn"] = reconnecciones_wifi;     
    data["heap"]   = ESP.getFreeHeap();       
    data["d18_logic"] = digitalRead(PIN_SENSOR);
    data["wifi_rssi"] = WiFi.RSSI(); 

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            digitalWrite(LED_PIN, HIGH);
            delay(50); 
            digitalWrite(LED_PIN, LOW);
            Serial.printf("TX OK | Total Acumulado: %u cajas\n", contador_producto);
        }
    }
}

// ---------------------------------------------------------------------------
// TAREA CORE 0: RED (Intacta)
// ---------------------------------------------------------------------------
void core0NetworkTask(void * pvParameters) {
    Serial.printf("[CORE 0] Tarea de red iniciada en el núcleo: %d\n", xPortGetCoreID());
    
    for(;;) {
        bool mqtt_ok = (WiFi.status() == WL_CONNECTED) && client.connected();

        if (mqtt_ok) {
            if (!wifiWasConnected) {
                wifiWasConnected = true;
                wifiDownMillis = 0; 
            }
            client.loop();
        } 
        else {
            if (wifiWasConnected) {
                reconnecciones_wifi++; 
                wifiWasConnected = false;
                wifiDownMillis = millis(); 
            }

            if (WiFi.status() == WL_CONNECTED) {
                static uint32_t lastTry = 0;
                if (millis() - lastTry > 5000) {
                    iot_connect(); 
                    lastTry = millis();
                }
            }

            if (wifiDownMillis != 0 && (millis() - wifiDownMillis > RESTART_TIMEOUT)) {
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

    pinMode(PIN_SENSOR, INPUT); 
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); 

    // Cálculo del filtro anti-ruido (Debounce)
    debounceUs = (60000000 / (limite_ppm_max * 1.2)) / pulsesPerRev;
    
    attachInterrupt(digitalPinToInterrupt(PIN_SENSOR), onPulse, modo_deteccion);

    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    handleUserMeta();

    if (pubInterval <= 0) pubInterval = defaultPubIntervalMs;

    const char* ssid = cfg["ssid"] ? (const char*)cfg["ssid"] : nullptr;
    const char* pass = cfg["w_pw"] ? (const char*)cfg["w_pw"] : nullptr;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        timeout++;
    }
    
    if (WiFi.status() == WL_CONNECTED) wifiWasConnected = true;
    else { wifiWasConnected = false; wifiDownMillis = millis(); }

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
    // Al actuar como acumulador puro manejado por interrupciones y FreeRTOS, 
    // el loop de Arduino no necesita procesar nada, liberando el 100% del procesador.
    vTaskDelay(pdMS_TO_TICKS(100));
}