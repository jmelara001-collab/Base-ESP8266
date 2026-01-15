#include <Arduino.h>
#include <IO7F8266.h>

// La librería necesita esta variable aunque esté vacía para evitar el error de compilación
String user_html = "";

char* ssid_pfix = (char*)"IOT_Device";
unsigned long lastPublishMillis = -pubInterval;

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    data["status"] = "running";

    serializeJson(root, msgBuffer);
    client.publish(evtTopic, msgBuffer);
}

void handleUserMeta() {
    // Si necesitas capturar variables de la web, se hace aquí
}

void handleUserCommand(char* topic, JsonDocument* root) {
    // Aquí recibes órdenes del servidor
}

void setup() {
    Serial.begin(115200);

    initDevice();

    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.printf("\nConectado a: %s\n", (const char*)cfg["ssid"]);

    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    set_iot_server();
    iot_connect();
}

void loop() {
    if (!client.connected()) {
        iot_connect();
    }
    
    client.loop();

    if ((pubInterval != 0) && (millis() - lastPublishMillis > pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}