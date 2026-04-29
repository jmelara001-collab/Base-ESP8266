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
const unsigned long RESTART_TIMEOUT = 10000; 

String user_html = "";  
char* ssid_pfix = (char*)"IOT_DEVICE";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

float limite_rpm = 75.0;  
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

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            // INDICADOR VISUAL: Solo parpadea si se envió correctamente
            digitalWrite(LED_PIN, HIGH);
            delay(50); // 50ms es ideal para ver el parpadeo
            digitalWrite(LED_PIN, LOW);
            Serial.printf("TX OK | RPM: %.2f | Temp ESP: %.1f C\n", rpm, temp_c);
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

    // 3. DETECTOR DE PARADA
    unsigned long localLastPulse;
    noInterrupts();
    localLastPulse = lastPulseTime;
    interrupts();

    if (micros() - localLastPulse > 2000000) { 
        if (maquina_running) { 
            rpm = 0; 
            pps = 0;
            maquina_running = false;
            Serial.println("[INFO] Máquina detenida.");
        }
    }

    // 4. PUBLICACIÓN PERIÓDICA (Aquí ocurre el parpadeo)
    if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
        publishData();
        lastPublishMillis = millis();
    }
}