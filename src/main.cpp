#include <Arduino.h>
#include <IO7F8266.h>

// --- CONFIGURACIÓN DE PINES ---
const uint8_t pinPulso = 13;    // D7 - Sensor reflectivo (Disco 8 orificios)
const uint8_t pinEstado = 5;     // D1 - Contacto Auxiliar
const uint8_t ledPin = LED_BUILTIN; 

// --- VARIABLES DE CONTROL ---
volatile unsigned long contadorPulsos = 0;
unsigned long pulsosAnterioresCheck = 0; 
const uint8_t tiempoDebounce = 15;      // ms para evitar ruido eléctrico
volatile unsigned long ultimoTiempoInterrupcion = 0;

const unsigned long umbralParada = 500; 

String user_html = "";
char* ssid_pfix = (char*)"IOT_Device";
unsigned long lastPublishMillis = 0;

// --- FUNCIÓN DE INTERRUPCIÓN (D7) ---
void IRAM_ATTR conteoGiro() {
    unsigned long tiempoActual = millis();
    if (tiempoActual - ultimoTiempoInterrupcion > tiempoDebounce) {
        contadorPulsos++;
        ultimoTiempoInterrupcion = tiempoActual;
    }
}

// --- FUNCIÓN DE ENVÍO DE DATOS ---
void publishData() {
    digitalWrite(ledPin, LOW);

    noInterrupts();
    unsigned long copiaPulsosActual = contadorPulsos;
    unsigned long ultimoPulsoRastreado = ultimoTiempoInterrupcion;
    interrupts();

    int estadoMaquina = 0;
    if (copiaPulsosActual > pulsosAnterioresCheck || (millis() - ultimoPulsoRastreado < umbralParada)) {
        estadoMaquina = 1;
    } else {
        estadoMaquina = 0;
    }

    pulsosAnterioresCheck = copiaPulsosActual;

    StaticJsonDocument<384> root; 
    JsonObject data = root.createNestedObject("d");
    
    long rssi = WiFi.RSSI(); 
    uint32_t uptime = millis() / 1000;
    uint32_t freeMem = ESP.getFreeHeap();

    data["m_act"] = estadoMaquina;   
    data["v_tot"] = copiaPulsosActual;
    data["s1"] = digitalRead(pinPulso);
    data["s2"] = digitalRead(pinEstado);
    data["rssi"] = rssi;
    data["up"] = uptime;
    data["mem"] = freeMem;
    data["status"] = "online";
    
    if (client.connected()) {
        serializeJson(root, msgBuffer);
        client.publish(evtTopic, msgBuffer); 
        Serial.printf("Status: %d | Pulsos: %lu\n", estadoMaquina, copiaPulsosActual);
    }

    digitalWrite(ledPin, HIGH);
}

void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {
    JsonObject d = (*root)["d"];
    if (d.containsKey("reset")) {
        noInterrupts();
        contadorPulsos = 0;
        pulsosAnterioresCheck = 0;
        interrupts();
    }
    if (d.containsKey("reboot")) {
        ESP.restart();
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(pinPulso, INPUT); 
    pinMode(pinEstado, INPUT);
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, HIGH);

    attachInterrupt(digitalPinToInterrupt(pinPulso), conteoGiro, RISING);

    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    
    if (pubInterval <= 0) pubInterval = 5000;

    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    Serial.print("Conectando a WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    // --- NUEVA SECCIÓN: Información de Red ---
    Serial.println("\n------------------------------------");
    Serial.println("¡WiFi Conectado!");
    Serial.print("Dirección IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Dirección MAC: ");
    Serial.println(WiFi.macAddress());
    Serial.println("------------------------------------");
    // ------------------------------------------
    
    set_iot_server();
    iot_connect();
}

void loop() {
    if (!client.connected()) iot_connect();
    client.loop();

    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}