#include <Arduino.h>
#include <WiFi.h>      
#include <IO7F32.h>    
#include <Preferences.h> // --- Librería para guardar en memoria flash ---

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

String user_html = "";  
char* ssid_pfix = (char*)"IOT_DEVICE";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

float limite_rpm = 75;  
int pulsesPerRev = 1;      
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
uint32_t tiempo_running_acumulado = 0; // Tiempo total en segundos
uint32_t ultimo_tiempo_guardado = 0;
unsigned long last_run_calc_millis = 0;

// ---------------------------------------------------------------------------
// INTERRUPCIÓN (ISR)
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
// PUBLICACIÓN DE DATOS MQTT
// ---------------------------------------------------------------------------
void publishData() {
    StaticJsonDocument<768> root; 
    JsonObject data = root.createNestedObject("d");

    float temp_c = (temprature_sens_read() - 32) / 1.8;

    data["pps"] = round(pps * 100.0) / 100.0;
    data["rpm"] = round(rpm * 100.0) / 100.0;
    data["running"] = maquina_running ? 1 : 0;
    data["temp"] = round(temp_c * 10.0) / 10.0; 
    data["uptime"] = millis() / 1000;          
    data["reconn"] = reconnecciones_wifi;     
    data["heap"]   = ESP.getFreeHeap();       
    data["d18_logic"] = digitalRead(PIN_SENSOR);
    data["wifi_ok"]   = (WiFi.status() == WL_CONNECTED);
    data["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    data["status"] = "Online";
    
    // --- Publicar el tiempo acumulado de running (en segundos) ---
    data["run_time_sec"] = tiempo_running_acumulado;

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            // INDICADOR VISUAL: Solo parpadea si se envió correctamente
            digitalWrite(LED_PIN, HIGH);
            delay(50); // 50ms es ideal para ver el parpadeo
            digitalWrite(LED_PIN, LOW);
            Serial.printf("TX OK | RPM: %.2f | Temp ESP: %.1f C | Run Time: %u s\n", rpm, temp_c, tiempo_running_acumulado);
        }
    }
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] Iniciando sistema de monitoreo...");

    // --- INICIALIZAR MEMORIA FLASH Y RECUPERAR ACUMULADO ---
    preferences.begin("estado_maq", false);
    tiempo_running_acumulado = preferences.getUInt("run_time", 0);
    ultimo_tiempo_guardado = tiempo_running_acumulado;
    Serial.printf("[NVS] Tiempo running acumulado recuperado: %u segundos\n", tiempo_running_acumulado);

    pinMode(PIN_SENSOR, INPUT); // <-- REGRESADO A INPUT PORQUE YA HAY PULLDOWN FÍSICO
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); 

    debounceUs = (60000000 / (limite_rpm * 1.2)) / pulsesPerRev;
    attachInterrupt(digitalPinToInterrupt(PIN_SENSOR), onPulse, RISING);

    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    handleUserMeta();

    if (pubInterval <= 0) pubInterval = defaultPubIntervalMs;
    lastPublishMillis = millis() - pubInterval;
    last_run_calc_millis = millis();

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
            
            // --- NUEVO: RESPALDO DE EMERGENCIA ANTES DE REINICIAR ---
            if (tiempo_running_acumulado != ultimo_tiempo_guardado) {
                preferences.putUInt("run_time", tiempo_running_acumulado);
                ultimo_tiempo_guardado = tiempo_running_acumulado;
                Serial.println("[NVS] Guardado de emergencia completado.");
            }
            // --------------------------------------------------------

            delay(1000);
            ESP.restart();
        }
    }

    // 2. CÁLCULO DE VELOCIDAD
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

    // 3. DETECTOR DE PARADA (Ajustado para máquinas lentas)
    unsigned long localLastPulse;
    noInterrupts();
    localLastPulse = lastPulseTime;
    interrupts();

    // 10000000 us = 10 segundos. 
    // Como tu pulso llega cada 5s, esto da un margen de espera razonable.
    if (micros() - localLastPulse > 10000000) {  
        if (maquina_running) { 
            rpm = 0; 
            pps = 0;
            maquina_running = false;
            Serial.println("[INFO] Máquina detenida (Tiempo de espera de 10s agotado).");
        }
    }

    // --- NUEVO: LÓGICA DEL ACUMULADOR DE TIEMPO RUNNING ---
    unsigned long current_millis = millis();
    if (maquina_running) {
        // Incrementar cada 1 segundo (1000 ms)
        if (current_millis - last_run_calc_millis >= 1000) {
            tiempo_running_acumulado++;
            last_run_calc_millis = current_millis;

            // Guardar en flash cada 15 segundos para proteger la vida útil de la memoria
            if (tiempo_running_acumulado - ultimo_tiempo_guardado >= 15) {
                preferences.putUInt("run_time", tiempo_running_acumulado);
                ultimo_tiempo_guardado = tiempo_running_acumulado;
            }
        }
    } else {
        last_run_calc_millis = current_millis; // Mantener sincronizado mientras está apagada
        
        // Si la máquina se detiene y hay segundos que no se han guardado en flash, guárdalos ahora
        if (tiempo_running_acumulado != ultimo_tiempo_guardado) {
            preferences.putUInt("run_time", tiempo_running_acumulado);
            ultimo_tiempo_guardado = tiempo_running_acumulado;
            Serial.println("[NVS] Tiempo guardado al detenerse la máquina.");
        }
    }

    // 4. PUBLICACIÓN PERIÓDICA (Aquí ocurre el parpadeo)
    if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
        publishData();
        lastPublishMillis = millis();
    }
}