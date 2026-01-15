#include <Arduino.h>
#include <IO7F32.h> 

String user_html = "";
char* ssid_pfix = (char*)"IOT_DEVICE";
unsigned long lastPublishMillis = -pubInterval;

// --- NUEVO: Definición del pin y resolución ---
const int PIN_ADC = 34;          // GPIO 34 (ADC1_CH6)
const float RESOLUCION = 4095.0; // 12 bits para ESP32
const float VOLTAJE_REF = 3.3;   // Voltaje de referencia
// ----------------------------------------------

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // --- NUEVO: Captura y Procesamiento ---
    int rawValue = analogRead(PIN_ADC); 
    float voltaje = rawValue * (VOLTAJE_REF / RESOLUCION);
    
    data["voltaje"] = voltaje;   // Enviamos el valor convertido
    data["raw"] = rawValue;      // Enviamos el valor crudo (0-4095)
    // --------------------------------------

    data["status"] = "running";

    serializeJson(root, msgBuffer);
    
    if (client.publish(evtTopic, msgBuffer)) {
        // NUEVO: Monitor serial detallado
        Serial.printf("Lectura ADC -> Raw: %d | Voltaje: %.2fV\n", rawValue, voltaje);
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

    // NUEVO: Configuración de atenuación (Opcional pero recomendado para 0-3.3V)
    // analogSetAttenuation(ADC_11db); 

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