#include <Arduino.h>
#include <WiFi.h>      
#include <IO7F32.h>    

// ---------------------------------------------------------------------------
// CONFIGURACIÓN BÁSICA IO7
// ---------------------------------------------------------------------------
String user_html = "";  
char* ssid_pfix = (char*)"IOT_DEVICE";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

// ---------------------------------------------------------------------------
// PINES DEL SENSOR (ESP32)
// ---------------------------------------------------------------------------
const int PIN_SENSOR = 18; 
const int LED_PIN = 2;

const unsigned long LED_PULSE_MS = 30;
unsigned long ledOffAtMs = 0;
volatile bool pulseBlinkFlag = false;

// ---------------------------------------------------------------------------
// VARIABLES DE MEDICIÓN Y DIAGNÓSTICO
// ---------------------------------------------------------------------------
volatile unsigned long lastPulseTime = 0;
volatile unsigned long pulsePeriodUs = 0; 
volatile bool newData = false;

const unsigned long debounceUs = 400; 

float pps = 0;
float rpm = 0;
int pulsesPerRev = 1; 
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
// PUBLICACIÓN DE DATOS MQTT (CON REDONDEO A 2 DECIMALES)
// ---------------------------------------------------------------------------
void publishData() {
    StaticJsonDocument<768> root; 
    JsonObject data = root.createNestedObject("d");

    // Redondeo a 2 decimales: Multiplicamos por 100, redondeamos y dividimos por 100.0
    data["pps"] = round(pps * 100.0) / 100.0;
    data["rpm"] = round(rpm * 100.0) / 100.0;
    
    data["running"] = maquina_running ? 1 : 0;

    // Diagnóstico
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
        Serial.printf("TX OK | RPM: %.2f | PPS: %.2f\n", rpm, pps);
    }
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] Iniciando sistema...");

    pinMode(PIN_SENSOR, INPUT);          
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); 

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
    
    wifiWasConnected = (WiFi.status() == WL_CONNECTED);
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL
// ---------------------------------------------------------------------------
void loop() {
    // 1. Gestión de conexión
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiWasConnected) {
            wifiWasConnected = true;
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
        }
    }

    // 2. Control visual LED
    if (pulseBlinkFlag) {
        digitalWrite(LED_PIN, HIGH); 
        ledOffAtMs = millis() + LED_PULSE_MS;
        pulseBlinkFlag = false;
    }
    if (ledOffAtMs != 0 && millis() >= ledOffAtMs) {
        digitalWrite(LED_PIN, LOW);  
        ledOffAtMs = 0;
    }

    // 3. Cálculo de velocidad
    if (newData) {
        noInterrupts();
        unsigned long periodo = pulsePeriodUs; 
        newData = false;
        interrupts();

        if (periodo > 0) {
            pps = 1000000.0f / periodo; 
            rpm = (pps * 60.0f / pulsesPerRev);
            maquina_running = true; 
        }
    }

    // 4. Detector de parada
    unsigned long localLastPulse;
    noInterrupts();
    localLastPulse = lastPulseTime;
    interrupts();

    if (micros() - localLastPulse > 2000000) { 
        if (maquina_running) { 
            rpm = 0; 
            pps = 0;
            maquina_running = false;
        }
    }

    // 5. Publicación periódica
    if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
        publishData();
        lastPublishMillis = millis();
    }
}