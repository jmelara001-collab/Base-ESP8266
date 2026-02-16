#include <Arduino.h>
#include <IO7F8266.h>

// --- CONFIGURACIÓN DE PINES ---
const uint8_t pinPulso = 13;    // D7 - Contacto NO (Sensor de giro)
const uint8_t pinEstado = 5;     // D1 - Contacto NC (Estado auxiliar)
const uint8_t ledPin = LED_BUILTIN; // LED interno para feedback visual

// --- VARIABLES DE CONTROL ---
volatile unsigned long contadorPulsos = 0;
unsigned long pulsosAnteriores = 0;
volatile unsigned long ultimoTiempoInterrupcion = 0;
const uint8_t tiempoDebounce = 20; // 20ms para filtrar ruido eléctrico

// Variables obligatorias para la librería IO7
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
    // Feedback visual: Enciende LED (LOW en ESP8266 es encendido)
    digitalWrite(ledPin, LOW);

    StaticJsonDocument<384> root; 
    JsonObject data = root.createNestedObject("d");
    
    // Captura segura del contador
    noInterrupts();
    unsigned long copiaPulsos = contadorPulsos;
    interrupts();

    unsigned long deltaPulsos = copiaPulsos - pulsosAnteriores;
    pulsosAnteriores = copiaPulsos;

    // Diagnóstico del sistema
    long rssi = WiFi.RSSI(); 
    uint32_t uptime = millis() / 1000;
    uint32_t freeMem = ESP.getFreeHeap();

    // Estructura del JSON para Node-RED
    data["v"] = copiaPulsos;     // Vueltas totales (Acumulado)
    data["d"] = deltaPulsos;     // Vueltas en el último periodo
    data["s1"] = digitalRead(pinPulso);
    data["s2"] = digitalRead(pinEstado);
    data["rssi"] = rssi;         // Intensidad de señal WiFi
    data["up"] = uptime;         // Tiempo encendido (Segundos)
    data["mem"] = freeMem;       // RAM libre (Salud del chip)
    data["status"] = "online";
    
    if (client.connected()) {
        serializeJson(root, msgBuffer);
        client.publish(evtTopic, msgBuffer); 
        Serial.printf("Enviado -> V:%lu | RSSI:%ld | Up:%lu s\n", copiaPulsos, rssi, uptime);
    }

    // Apagar LED de feedback
    digitalWrite(ledPin, HIGH);
}

// --- MANEJO DE CONFIGURACIÓN DESDE PLATAFORMA ---
void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        Serial.printf("Intervalo actualizado a: %d ms\n", pubInterval);
    }
}

// --- MANEJO DE COMANDOS DESDE NODE-RED / IO7 ---
void handleUserCommand(char* topic, JsonDocument* root) {
    JsonObject d = (*root)["d"];
    
    // Comando para resetear vueltas: {"reset": 1}
    if (d.containsKey("reset")) {
        contadorPulsos = 0;
        pulsosAnteriores = 0;
        Serial.println("Contador reseteado por comando.");
    }

    // Comando para reiniciar hardware: {"reboot": 1}
    if (d.containsKey("reboot")) {
        Serial.println("Reiniciando ESP8266...");
        delay(500);
        ESP.restart();
    }
}

void setup() {
    Serial.begin(115200);
    
    // Configuración de Hardware
    pinMode(pinPulso, INPUT); 
    pinMode(pinEstado, INPUT);
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, HIGH); // Inicia apagado

    // Activar interrupción en D7
    attachInterrupt(digitalPinToInterrupt(pinPulso), conteoGiro, RISING);

    // Inicializar IO7 y Callbacks
    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    handleUserMeta();

    // Intervalo de seguridad (5 segundos por defecto)
    if (pubInterval <= 0) pubInterval = 5000;

    // Conexión WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    Serial.print("Conectando WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Conectado OK");
    
    set_iot_server();
    iot_connect();
}

void loop() {
    // Mantener conexión MQTT
    if (!client.connected()) {
        iot_connect();
    }
    client.loop();

    // Temporizador de publicación
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}