#include <Arduino.h>
#include <IO7F32.h> // Librería específica para ESP32

// Variable obligatoria para la interfaz de la librería
String user_html = "";

// Nombre del punto de acceso para configuración
char* ssid_pfix = (char*)"IOT_DEVICE";

// Control de tiempo para publicación (envío inmediato al conectar)
unsigned long lastPublishMillis = -pubInterval;

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // --- AGREGA TUS DATOS AQUÍ ---
    data["status"] = "running";
    // Ejemplo: data["sensor"] = lectura;
    // ----------------------------

    serializeJson(root, msgBuffer);
    
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.println("Evento enviado a IO7 OK");
    } else {
        Serial.println("Error al enviar a IO7");
    }
}

void handleUserMeta() {
    // Sincroniza el intervalo de publicación desde la plataforma
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        Serial.printf("Intervalo actualizado: %d ms\n", pubInterval);
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {
    // --- AGREGA LÓGICA DE COMANDOS AQUÍ ---
    // Ejemplo: Controlar salidas desde el dashboard
}

void setup() {
    Serial.begin(115200);

    // --- CONFIGURA TUS PINES AQUÍ (pinMode) ---

    // Inicialización del dispositivo y carga de memoria interna
    initDevice();

    // Registro de funciones Callback (Meta y Comandos)
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    // Aplicar configuración inicial de la plataforma
    handleUserMeta();

    // Asegura un intervalo por defecto si no existe configuración previa
    if (pubInterval <= 0) pubInterval = 5000;

    // Conexión WiFi en modo Estación
    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    Serial.print("Conectando WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.printf("\nConectado | IP: %s\n", WiFi.localIP().toString().c_str());

    // Configuración y conexión al servidor MQTT
    set_iot_server();
    iot_connect();
}

void loop() {
    // Mantiene la conexión viva y reconecta si es necesario
    if (!client.connected()) {
        iot_connect();
    }
    
    client.loop();

    // Temporizador de publicación basado en pubInterval
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}