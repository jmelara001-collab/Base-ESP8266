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
unsigned long wifiDownMillis = 0;       // Momento en que se perdió el WiFi
const unsigned long RESTART_TIMEOUT = 60000; // 1 minuto en milisegundos

String user_html = "";  
char* ssid_pfix = (char*)"RAJADORA_3";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

float limite_rpm = 200.0;  
int pulsesPerRev = 8;      
const int PIN_SENSOR = 18; 
const int LED_PIN = 2;
unsigned long debounceUs; 

const unsigned long LED_PULSE_MS = 30;
unsigned long ledOffAtMs = 0;
volatile bool pulseBlinkFlag = false;
volatile unsigned long lastPulseTime = 0;
volatile unsigned long pulsePeriodUs = 0; 
volatile bool newData = false;

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
        pulseBlinkFlag = true;
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

    bool ok = false;
    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        ok = client.publish(evtTopic, msgBuffer);
    }

    if (ok) {
        digitalWrite(LED_PIN, HIGH);
        delay(10); 
        digitalWrite(LED_PIN, LOW);
        Serial.printf("TX OK | RPM: %.2f | Temp ESP: %.1f C\n", rpm, temp_c);
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
        Serial.print("[WIFI] Dirección IP: ");
        Serial.println(WiFi.localIP());
        Serial.print("[WIFI] Dirección MAC: ");
        Serial.println(WiFi.macAddress());
        wifiWasConnected = true;
    } else {
        Serial.println("\n[WIFI] No se pudo conectar al inicio.");
        wifiWasConnected = false;
        wifiDownMillis = millis(); // Empezamos a contar desde el boot si no conectó
    }
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL
// ---------------------------------------------------------------------------
void loop() {
    // 1. GESTIÓN DE CONEXIÓN Y REINICIO DE SEGURIDAD
    if (WiFi.status() == WL_CONNECTED) {
        // Si recuperamos la conexión, reseteamos el cronómetro de falla
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            wifiDownMillis = 0; 
            Serial.print("[WIFI] Reconectado. IP: ");
            Serial.println(WiFi.localIP());
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
        // Si se pierde la conexión
        if (wifiWasConnected) {
            reconnecciones_wifi++; 
            wifiWasConnected = false;
            wifiDownMillis = millis(); // Marcamos el tiempo exacto de la caída
            Serial.println("[WIFI] Conexión perdida...");
        }

        // CONTROL DE REINICIO CRÍTICO:
        // Si han pasado más de 3 minutos (180000 ms) sin WiFi, reiniciamos
        if (wifiDownMillis != 0 && (millis() - wifiDownMillis > RESTART_TIMEOUT)) {
            Serial.println("[ALERTA] Sistema bloqueado por falta de red. Reiniciando...");
            delay(1000);
            ESP.restart(); // Reinicio total por software
        }
    }

    // 2. CONTROL VISUAL LED
    if (pulseBlinkFlag) {
        digitalWrite(LED_PIN, HIGH); 
        ledOffAtMs = millis() + LED_PULSE_MS;
        pulseBlinkFlag = false;
    }
    if (ledOffAtMs != 0 && millis() >= ledOffAtMs) {
        digitalWrite(LED_PIN, LOW);  
        ledOffAtMs = 0;
    }

    // 3. CÁLCULO DE VELOCIDAD
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
            } else {
                Serial.print("[FILTRO] Ruido: ");
                Serial.println(rpm_temp);
            }
        }
    }

    // 4. DETECTOR DE PARADA
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

    // 5. PUBLICACIÓN PERIÓDICA
    if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
        publishData();
        lastPublishMillis = millis();
    }
}