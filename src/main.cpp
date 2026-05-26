#include <Arduino.h>
#include <IO7F32.h> 

String user_html = "";
char* ssid_pfix = (char*)"IOT_DEVICE";
unsigned long lastPublishMillis = -pubInterval;

// 1. Declaramos la variable que quieres enviar
const char* mensaje_codigo = "codigo 1";

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // --- DATOS AGREGADOS ---
    data["status"] = "running";
    data["mensaje"] = mensaje_codigo; // Enviamos "codigo 1" a la plataforma
    
    // Imprimimos en el monitor serial antes de enviar
    Serial.print("Enviando a plataforma: ");
    Serial.println(mensaje_codigo);
    // ----------------------------

    serializeJson(root, msgBuffer);
    
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.println("Evento enviado a IO7 OK");
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

void handleUserCommand(char* topic, JsonDocument* root) {
    // Lógica de comandos
}

void setup() {
    Serial.begin(115200);

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
    
    Serial.printf("\nConectado | IP: %s\n", WiFi.localIP().toString().c_str());

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