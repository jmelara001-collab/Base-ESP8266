#include <Arduino.h>
#include <IO7F8266.h>
#include <Adafruit_MAX31865.h> 

// Variable obligatoria para la librería IO7
String user_html = "";

// Prefijo para el nombre del AP de configuración
char* ssid_pfix = (char*)"IOT_Device";

// Control de tiempo para publicación
unsigned long lastPublishMillis = -pubInterval;

// --- CONFIGURACIÓN DEL SENSOR MAX31865 (PT100) ---
// Pines SPI para ESP8266: CS(D2/GPIO4), DI/MOSI(D7/GPIO13), DO/MISO(D6/GPIO12), CLK(D5/GPIO14)
Adafruit_MAX31865 thermo = Adafruit_MAX31865(4, 13, 12, 14);

// Valores específicos para PT100
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
        Serial.printf("Error detectado: 0x%02X\n", fault);
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
    // Lógica para recibir comandos desde la plataforma si fuera necesario
}

void setup() {
    Serial.begin(115200);

    // Inicializar el MAX31865 configurado para 3 hilos (común en PT100 industriales)
    // Si tu sensor es de 2 o 4 hilos, cambia a MAX31865_2WIRE o MAX31865_4WIRE
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

    // Publicación basada en el intervalo configurado
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}