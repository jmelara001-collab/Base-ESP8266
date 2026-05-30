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

float limite_rpm = 60;  
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

// --- VARIABLES PARA EL ACUMULADOR DE TIEMPO EN FLASH CORREGIDO ---
Preferences preferences;
uint32_t tiempo_acumulado_historial = 0;  // Total acumulado en encendidos anteriores (NVS)
uint32_t tiempo_running_ciclo_actual = 0; // Segundos trabajados DESDE este arranque
uint32_t tiempo_running_total = 0;        // La SUMA de ambos (lo que se envía y guarda)

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
    
    // --- Publicar el tiempo acumulado total real ---
    data["run_time_sec"] = tiempo_running_total;

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            // INDICADOR VISUAL: Solo parpadea si se envió correctamente
            digitalWrite(LED_PIN, HIGH);
            delay(50); // 50ms es ideal para ver el parpadeo
            digitalWrite(LED_PIN, LOW);
            Serial.printf("TX OK | RPM: %.2f | Temp ESP: %.1f C | Run Time Total: %u s\n", rpm, temp_c, tiempo_running_total);
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

    // --- INICIALIZAR MEMORIA FLASH Y RECUPERAR HISTORIAL INAMOVIBLE ---
    preferences.begin("estado_maq", false);
    tiempo_acumulado_historial = preferences.getUInt("run_time", 0);
    
    // Al arrancar, el total es igual al historial recuperado
    tiempo_running_total = tiempo_acumulado_historial;
    ultimo_tiempo_guardado = tiempo_acumulado_historial;
    Serial.printf("[NVS] Historial acumulado recuperado: %u segundos\n", tiempo_acumulado_historial);

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
            
            // --- RESPALDO DE EMERGENCIA ANTES DE REINICIAR (USANDO VARIABLE TOTAL) ---
            if (tiempo_running_total != ultimo_tiempo_guardado) {
                preferences.putUInt("run_time", tiempo_running_total);
                ultimo_tiempo_guardado = tiempo_running_total;
                Serial.println("[NVS] Guardado de emergencia completado.");
            }
            // -------------------------------------------------------------------------

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

// 3. DETECTOR DE PARADA (Timeout de 20 segundos para bajas RPM)
    unsigned long localLastPulse;
    noInterrupts();
    localLastPulse = lastPulseTime;
    interrupts();

    // 20,000,000 microsegundos = 20 segundos. 
    // Ideal para soportar el ritmo de velocidades ultra bajas con un margen de seguridad.
    if (micros() - localLastPulse > 20000000) {  
        if (maquina_running) { 
            noInterrupts(); // Protegemos la actualización de variables compartidas
            rpm = 0; 
            pps = 0;
            interrupts();
            
            maquina_running = false;
            Serial.println("[INFO] Máquina detenida (Tiempo de espera de 20s agotado).");
        }
    }

    // --- LÓGICA DEL ACUMULADOR DE TIEMPO RUNNING TOTAL PROTEGIDO ---
    unsigned long current_millis = millis();
    if (maquina_running) {
        // Incrementar cada 1 segundo (1000 ms) el contador del ciclo actual
        if (current_millis - last_run_calc_millis >= 1000) {
            tiempo_running_ciclo_actual++;
            last_run_calc_millis = current_millis;

            // El TOTAL es la suma del historial inamovible + lo que va de este ciclo
            tiempo_running_total = tiempo_acumulado_historial + tiempo_running_ciclo_actual;

            // Guardar en flash cada 15 segundos de trabajo neto acumulado
            if (tiempo_running_total - ultimo_tiempo_guardado >= 15) {
                preferences.putUInt("run_time", tiempo_running_total);
                ultimo_tiempo_guardado = tiempo_running_total;
                Serial.printf("[NVS] Guardado periódico completado. Total: %u s\n", tiempo_running_total);
            }
        }
    } else {
        last_run_calc_millis = current_millis; // Mantener sincronizado mientras está apagada
        
        // Si la máquina se detiene y hay segundos que no se han guardado, los asegura
        if (tiempo_running_total != ultimo_tiempo_guardado) {
            preferences.putUInt("run_time", tiempo_running_total);
            ultimo_tiempo_guardado = tiempo_running_total;
            Serial.printf("[NVS] Guardado por parada de máquina. Total: %u s\n", tiempo_running_total);
        }
    }

    // 4. PUBLICACIÓN PERIÓDICA
    if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
        publishData();
        lastPublishMillis = millis();
    }
}