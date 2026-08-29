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
char* ssid_pfix = (char*)"Kinco";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

uint32_t reconnecciones_wifi = 0;   
bool wifiWasConnected = false;      

// --- VARIABLES DE CONTEO Y ESCALA ---
const int PIN_SENSOR = 18; 
const int LED_PIN = 2;

unsigned long FILTRO_MS = 10;       // Milisegundos para evitar el rebote del sensor
uint32_t PULSOS_POR_UNIDAD = 1;   // Cuántos pulsos equivalen a 1 unidad (producto final)
int TIPO_FLANCO = HIGH;             // HIGH = Cuenta al subir, LOW = Cuenta al bajar

uint32_t contador_pulsos_total = 0; // Acumula cada pulso físico individual
uint32_t total_unidades_historico = 0; // Acumula las unidades completas calculadas

int estado_sensor_crudo = LOW;      // Lo que lee digitalRead al instante
int estado_sensor_validado = LOW;   // El estado después de pasar el filtro
unsigned long tiempo_ultimo_cambio = 0; 

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
    // Ajuste dinámico de parámetros sin reprogramar
    if (cfg["meta"].containsKey("filtro_ms")) {
        FILTRO_MS = cfg["meta"]["filtro_ms"].as<unsigned long>();
    }
    if (cfg["meta"].containsKey("pulsos_unidad")) {
        PULSOS_POR_UNIDAD = cfg["meta"]["pulsos_unidad"].as<uint32_t>();
    }
    if (cfg["meta"].containsKey("flanco")) {
        TIPO_FLANCO = cfg["meta"]["flanco"].as<int>();
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {}

// ---------------------------------------------------------------------------
// PUBLICACIÓN DE DATOS MQTT (Llamada desde la tarea de red)
// ---------------------------------------------------------------------------
void publishData() {
    StaticJsonDocument<768> root; 
    JsonObject data = root.createNestedObject("d");

    // Enviamos el conteo de forma genérica
    data["unidades_totales"] = total_unidades_historico;
    data["pulsos_totales"] = contador_pulsos_total;
    
    // Variables de estado original
    data["uptime"] = millis() / 1000;          
    data["reconn"] = reconnecciones_wifi;     
    data["heap"]   = ESP.getFreeHeap();       
    data["d18_logic"] = digitalRead(PIN_SENSOR);
    data["wifi_ok"]   = (WiFi.status() == WL_CONNECTED);
    data["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    data["status"] = "Online";

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            digitalWrite(LED_PIN, HIGH);
            delay(50); 
            digitalWrite(LED_PIN, LOW);
            Serial.printf("TX OK | Unidades: %u | Pulsos: %u\n", total_unidades_historico, contador_pulsos_total);
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
    Serial.println("\n[BOOT] Iniciando sistema de conteo por pulsos (Modo Acumulador Ciego)...");

    pinMode(PIN_SENSOR, INPUT); 
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
// LOOP PRINCIPAL (Lógica de conteo con antirrebote, SIN PARADA POR TIMEOUT)
// ---------------------------------------------------------------------------
void loop() {
    int lectura_actual = digitalRead(PIN_SENSOR);

    // 1. Detectar cualquier cambio instantáneo y reiniciar el reloj del filtro
    if (lectura_actual != estado_sensor_crudo) {
        tiempo_ultimo_cambio = millis();
        estado_sensor_crudo = lectura_actual;
    }

    // 2. Si el sensor se quedó estable por el tiempo dictado en FILTRO_MS
    if ((millis() - tiempo_ultimo_cambio) > FILTRO_MS) {
        
        // 3. Revisar si este estado estable es un estado "nuevo" validado
        if (estado_sensor_crudo != estado_sensor_validado) {
            estado_sensor_validado = estado_sensor_crudo;

            // 4. ¿El cambio coincide con el flanco que queremos leer?
            if (estado_sensor_validado == TIPO_FLANCO) {
                contador_pulsos_total++; // ¡Pasó un pulso válido!
                
                // 5. Cálculo automático de unidades completas
                total_unidades_historico = contador_pulsos_total / PULSOS_POR_UNIDAD;
            }
        }
    }
}