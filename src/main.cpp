#include <Arduino.h>
#include <IO7F8266.h>

// Variable obligatoria para la librería
String user_html = "";

// Prefijo para el nombre del AP de configuración
char* ssid_pfix = (char*)"IOT_Device";

// Control de tiempo para publicación
unsigned long lastPublishMillis = -pubInterval;

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // --- AGREGA TUS DATOS AQUÍ ---
    data["status"] = "running";
    // Ejemplo: data["temperatura"] = lectura;
    // ----------------------------

    serializeJson(root, msgBuffer);
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.println("Evento enviado a IO7 OK");
    } else {
        Serial.println("Error al enviar a IO7");
    }
}

void handleUserMeta() {
    // Sincroniza el intervalo de publicación con la plataforma
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        Serial.printf("Intervalo actualizado: %d ms\n", pubInterval);
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {
    // --- AGREGA LÓGICA DE COMANDOS AQUÍ ---
    // Ejemplo: Si recibes "on", encender un relevador
}

void setup() {
    Serial.begin(115200);

    // --- CONFIGURA TUS PINES AQUÍ (pinMode) ---

    // Inicialización del dispositivo y carga de configuración
    initDevice();

    // Registro de funciones Callback
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    // Aplicar configuración inicial
    handleUserMeta();

    // Intervalo de seguridad por defecto (5 segundos)
    if (pubInterval <= 0) pubInterval = 5000;

    // Conexión WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    Serial.print("Conectando WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.printf("\nConectado a: %s | IP: %s\n", (const char*)cfg["ssid"], WiFi.localIP().toString().c_str());

    // Conexión al servidor IO7
    set_iot_server();
    iot_connect();
}

void loop() {
    // Mantener conexión MQTT activa
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