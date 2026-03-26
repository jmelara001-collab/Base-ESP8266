#include <Arduino.h>
#include <IO7F8266.h>
#include <Adafruit_MAX31865.h> 

// Variable obligatoria para la librería
String user_html = "";

// Prefijo para el nombre del AP de configuración
char* ssid_pfix = (char*)"IOT_Device";

// Control de tiempo para publicación
unsigned long lastPublishMillis = -pubInterval;

// --- CONFIGURACIÓN DEL SENSOR MAX31865 ---
// Pines SPI para el ESP8266: CS(4 -> D2), DI/MOSI(13), DO/MISO(12), CLK(14)
Adafruit_MAX31865 thermo = Adafruit_MAX31865(4, 13, 12, 14);

#define RREF      430.0
#define RNOMINAL  100.0

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    float temperaturaActual = thermo.temperature(RNOMINAL, RREF);
    
    data["status"] = "running";
    data["temperatura"] = temperaturaActual;
    
    uint8_t fault = thermo.readFault();
    if (fault) {
        data["sensor_error"] = fault;
        thermo.clearFault();
    }

    serializeJson(root, msgBuffer);
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.print("Evento enviado a IO7 OK | Temp: ");
        Serial.println(temperaturaActual);
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

    // Inicializar el MAX31865 (Configurado para 3 hilos por defecto)
    thermo.begin(MAX31865_3WIRE); 

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
    
    Serial.printf("\nConectado a: %s | IP: %s\n", (const char*)cfg["ssid"], WiFi.localIP().toString().c_str());

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