#include <Arduino.h>
#include <WiFi.h>      
#include <IO7F32.h>    
#include <Preferences.h> 

// --- PROTOTIPOS DE FUNCIONES (Para evitar errores de compilación en VS Code) ---
void core0NetworkTask(void * pvParameters);
void publishData();
void handleUserMeta();
void handleUserCommand(char* topic, JsonDocument* root);
IRAM_ATTR void onPulse();

// --- VARIABLES PARA EL CONTROL DE REINICIO AUTOMÁTICO ---
unsigned long wifiDownMillis = 0;       
const unsigned long RESTART_TIMEOUT = 300000; // 5 minutos de espera offline (WiFi o SSL) antes de reiniciar radio

String user_html = "";  
char* ssid_pfix = (char*)"IOT_DEVICE";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

float limite_rpm = 140;  
int pulsesPerRev = 4;      
const int PIN_SENSOR = 18; 
const int LED_PIN = 2;
unsigned long debounceUs; 

volatile bool newData = false;
volatile unsigned long lastPulseTime = 0;
volatile unsigned long pulsePeriodUs = 0; 

float pps = 0;
float rpm = 0;
bool maquina_running = false; 

uint32_t reconnecciones_wifi = 0;   
bool wifiWasConnected = false;      

// --- VARIABLES PARA EL ACUMULADOR DE TIEMPO EN FLASH ---
Preferences preferences;
uint32_t tiempo_running_acumulado = 0; 
uint32_t ultimo_tiempo_guardado = 0;
unsigned long last_run_calc_millis = 0;

// --- TAREA PARA EL CORE 0 (RED) ---
TaskHandle_t NetworkTaskHandle;

// ---------------------------------------------------------------------------
// INTERRUPCIÓN (ISR) - Ejecuta instantáneamente ante un pulso físico
// ---------------------------------------------------------------------------
IRAM_ATTR void onPulse() {
    unsigned long now = micros();
    unsigned long timeDifference = now - lastPulseTime;
    if (timeDifference > debounceUs) {
        pulsePeriodUs = timeDifference; 
        lastPulseTime = now;
        newData = true; 
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
// PUBLICACIÓN DE DATOS MQTT (Llamada desde la tarea de red)
// ---------------------------------------------------------------------------
void publishData() {
    StaticJsonDocument<768> root; 
    JsonObject data = root.createNestedObject("d");

    data["pps"] = round(pps * 100.0) / 100.0;
    data["rpm"] = round(rpm * 100.0) / 100.0;
    data["running"] = maquina_running ? 1 : 0;
    data["uptime"] = millis() / 1000;          
    data["reconn"] = reconnecciones_wifi;     
    data["heap"]   = ESP.getFreeHeap();       
    data["d18_logic"] = digitalRead(PIN_SENSOR);
    data["wifi_ok"]   = (WiFi.status() == WL_CONNECTED);
    data["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    data["status"] = "Online";
    data["id"] = "Afelpadora";
    data["run_time_sec"] = tiempo_running_acumulado;

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            digitalWrite(LED_PIN, HIGH);
            delay(50); 
            digitalWrite(LED_PIN, LOW);
            Serial.printf("TX OK | RPM: %.2f | Run Time: %u s\n", rpm, tiempo_running_acumulado);
        }
    }
}

// ---------------------------------------------------------------------------
// TAREA EXCLUSIVA DEL CORE 0: GESTIÓN DE WIFI Y MQTT
// ---------------------------------------------------------------------------
void core0NetworkTask(void * pvParameters) {
    Serial.printf("[CORE 0] Tarea de red iniciada en el núcleo: %d\n", xPortGetCoreID());
    
    for(;;) {
        // Evaluamos conexión completa: WiFi conectado Y cliente MQTT en línea
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
            // Si venía conectado y se acaba de caer (por WiFi o por fallo SSL)
            if (wifiWasConnected) {
                reconnecciones_wifi++; 
                wifiWasConnected = false;
                wifiDownMillis = millis(); // Inicia ventana de 5 minutos
                Serial.println("[ALERTA] Enlace MQTT o WiFi perdido. Iniciando temporizador de tolerancia...");
            }

            // Si hay WiFi local pero cayó MQTT, intentamos conectar cada 5 segundos
            if (WiFi.status() == WL_CONNECTED) {
                static uint32_t lastTry = 0;
                if (millis() - lastTry > 5000) {
                    iot_connect(); 
                    lastTry = millis();
                }
            }

            // Si pasan 5 minutos sin lograr conectar exitosamente por MQTT/SSL o WiFi
            if (wifiDownMillis != 0 && (millis() - wifiDownMillis > RESTART_TIMEOUT)) {
                Serial.println("[CRÍTICO] 5 minutos sin reportar datos (Fallo SSL o red). Reiniciando radio WiFi...");
                
                WiFi.disconnect(true); 
                vTaskDelay(pdMS_TO_TICKS(100));
                WiFi.mode(WIFI_OFF);   
                vTaskDelay(pdMS_TO_TICKS(100));
                WiFi.mode(WIFI_STA);   
                
                const char* ssid = cfg["ssid"] ? (const char*)cfg["ssid"] : nullptr;
                const char* pass = cfg["w_pw"] ? (const char*)cfg["w_pw"] : nullptr;
                WiFi.begin(ssid, pass); 

                wifiDownMillis = millis(); // Nueva ventana de tolerancia tras resetear la radio
            }
        }

        // PUBLICACIÓN PERIÓDICA MQTT
        if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
            publishData();
            lastPublishMillis = millis();
        }

        // Respiro obligatorio para alimentar al Watchdog del Core 0
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

// ---------------------------------------------------------------------------
// SETUP (Ejecutado por defecto en Core 1)
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] Iniciando sistema de monitoreo multinúcleo...");

    // --- INICIALIZAR MEMORIA FLASH Y RECUPERAR ACUMULADO ---
    preferences.begin("estado_maq", false);
    tiempo_running_acumulado = preferences.getUInt("run_time", 0);
    ultimo_tiempo_guardado = tiempo_running_acumulado;
    Serial.printf("[NVS] Tiempo running acumulado recuperado: %u segundos\n", tiempo_running_acumulado);

    pinMode(PIN_SENSOR, INPUT); 
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); 

    debounceUs = (60000000 / (limite_rpm * 1.2)) / pulsesPerRev;
    attachInterrupt(digitalPinToInterrupt(PIN_SENSOR), onPulse, RISING);

    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    handleUserMeta();

    if (pubInterval <= 0) pubInterval = defaultPubIntervalMs;
    last_run_calc_millis = millis();

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

    // --- CREACIÓN DE LA TAREA EN EL CORE 0 ---
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

// ---------------- -----------------------------------------------------------
// LOOP PRINCIPAL (Ejecutado de forma limpia en el Core 1)
// ---------------------------------------------------------------------------
void loop() {
    // 1. CÁLCULO DE VELOCIDAD
    if (newData) {
        noInterrupts();
        unsigned long periodo = pulsePeriodUs; 
        newData = false;
        interrupts();

        if (periodo > 0) {
            float pps_temp = 1000000.0f / (float)periodo; 
            float rpm_temp = (pps_temp * 60.0f / (float)pulsesPerRev);

            if (rpm_temp < limite_rpm) {
                pps = pps_temp;
                rpm = rpm_temp;
                maquina_running = true; 
            }
        }
    }

    // 2. DETECTOR DE PARADA
    unsigned long localLastPulse;
    noInterrupts();
    localLastPulse = lastPulseTime;
    interrupts();

    if (micros() - localLastPulse > 2000000) {  
        if (maquina_running) { 
            rpm = 0; 
            pps = 0;
            maquina_running = false;
            Serial.println("[INFO] Máquina detenida (Tiempo de espera de 2s agotado).");
        }
    }

    // 3. LÓGICA DEL ACUMULADOR DE TIEMPO RUNNING
    unsigned long current_millis = millis();
    if (maquina_running) {
        if (current_millis - last_run_calc_millis >= 1000) {
            tiempo_running_acumulado++;
            last_run_calc_millis = current_millis;

            if (tiempo_running_acumulado - ultimo_tiempo_guardado >= 15) {
                preferences.putUInt("run_time", tiempo_running_acumulado);
                ultimo_tiempo_guardado = tiempo_running_acumulado;
                Serial.printf("[NVS Autoguardado] Tiempo actualizado en Flash: %u s\n", tiempo_running_acumulado);
            }
        }
    } else {
        last_run_calc_millis = current_millis; 
        
        if (tiempo_running_acumulado != ultimo_tiempo_guardado) {
            preferences.putUInt("run_time", tiempo_running_acumulado);
            ultimo_tiempo_guardado = tiempo_running_acumulado;
            Serial.printf("[NVS Parada] Tiempo guardado al detenerse la máquina: %u s\n", tiempo_running_acumulado);
        }
    }
}