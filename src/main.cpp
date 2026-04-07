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

// --- NUEVAS VARIABLES DE DIAGNÓSTICO ---
uint32_t reconnecciones_wifi = 0;   // Cuántas veces se ha caído el WiFi
uint32_t uptime_segundos = 0;      // Tiempo total desde el último reinicio

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
    StaticJsonDocument<768> root; // Aumentamos un poco el tamaño por las nuevas variables
    JsonObject data = root.createNestedObject("d");

    // Datos de operación
    data["pps"] = pps;
    data["rpm"] = rpm;
    data["running"] = maquina_running ? 1 : 0;

    // Datos de diagnóstico (El respaldo que necesitas)
    data["uptime"] = millis() / 1000;         // Segundos desde que encendió
    data["reconn"] = reconnecciones_wifi;     // Veces que ha fallado el WiFi
    data["heap"]   = ESP.getFreeHeap();       // Memoria libre (para ver si se traba por falta de memoria)

    data["d18_logic"] = digitalRead(PIN_SENSOR);
    data["wifi_ok"]  = (WiFi.status() == WL_CONNECTED);
    data["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    data["status"] = "Online";

    serializeJson(root, msgBuffer);

    bool ok = false;
    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        ok = client.publish(evtTopic, msgBuffer);
    }

    if (ok) {
    // Parpadeo al publicar con éxito
    digitalWrite(LED_PIN, HIGH);
    delay(50); // Un parpadeo rápido
    digitalWrite(LED_PIN, LOW);

    Serial.printf("TX OK | RPM: %.2f...\n", rpm);
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
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL
// ---------------------------------------------------------------------------
void loop() {
    // Manejo de conexión WiFi y reconexión
    if (WiFi.status() != WL_CONNECTED) {
        static bool wasConnected = true;
        if (wasConnected) {
            reconnecciones_wifi++; // Sumamos una caída
            wasConnected = false;
            Serial.println("[WiFi] Conexión perdida...");
        }
    } else {
        // Si estamos conectados pero el cliente MQTT no
        if (!client.connected()) {
            static uint32_t lastTry = 0;
            if (millis() - lastTry > 5000) {
                iot_connect();
                lastTry = millis();
            }
        }
        client.loop();
    }

    // Control visual del LED
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
            rpm = (pulsesPerRev > 0) ? (pps * 60.0f / pulsesPerRev) : 0;
            maquina_running = true; 
        }
    }

// --- 4. Detector de parada (MODIFICADO) ---
unsigned long localLastPulse;

// Protegemos la lectura de la variable volátil
noInterrupts();
localLastPulse = lastPulseTime;
interrupts();

if (micros() - localLastPulse > 2000000) { // Si han pasado más de 2 seg
    if (maquina_running) { 
        rpm = 0; 
        pps = 0;
        maquina_running = false;
        Serial.println("[INFO] Máquina detenida (Timeout)");
    }
}

    // 5. Publicación periódica
    if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
        publishData();
        lastPublishMillis = millis();
    }
}