#include <Arduino.h>
#include <IO7F8266.h>

String user_html = "";

// Prefijo para el nombre del AP de configuración
char* ssid_pfix = (char*)"IOT_DEVICE";

// Control de tiempo para publicación inmediata
unsigned long lastPublishMillis = -pubInterval;

// Definición del pin analógico
const int PIN_VOLTAJE = A0;

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // 1. Leer el valor crudo (0 a 1023)
    int lecturaAnaloga = analogRead(PIN_VOLTAJE);
    
    // 2. Convertir a voltaje (Escala de 3.3V para ESP8266)
    float voltaje = lecturaAnaloga * (3.3 / 1023.0);

    // --- DATOS ENVIADOS A IO7 ---
    data["voltaje"] = voltaje;
    data["raw"] = lecturaAnaloga;
    data["status"] = "running";
    // ----------------------------

    serializeJson(root, msgBuffer);
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.printf("Voltaje: %.2fV | Raw: %d | Enviado OK\n", voltaje, lecturaAnaloga);
    } else {
        Serial.println("Error al enviar a IO7");
    }
}

void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        Serial.printf("Intervalo actualizado: %d ms\n", pubInterval);
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {}

void setup() {
    Serial.begin(115200);

    // Nota: El pin A0 es entrada por defecto, no requiere pinMode especial.
    
    initDevice();

    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    handleUserMeta();

    if (pubInterval <= 0) pubInterval = 5000;

    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    Serial.print("Conectando WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.printf("\nConectado! IP: %s\n", WiFi.localIP().toString().c_str());

    set_iot_server();
    iot_connect();
}

void loop() {
    if (!client.connected()) {
        iot_connect();
    }
    
    client.loop();

    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}