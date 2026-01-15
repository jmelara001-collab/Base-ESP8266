#include <Arduino.h>
#include <IO7F8266.h>

String user_html = "";

// Nombre del punto de acceso para configuración inicial
char* ssid_pfix = (char*)"IOT_DEVICE";

// Forzamos el envío inmediato al iniciar usando el valor negativo del intervalo
unsigned long lastPublishMillis = -pubInterval;

// Definición de pines digitales
const int PIN_D1 = D1; 
const int PIN_D7 = D7;

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // Lectura de estados lógicos (0 o 1)
    int estadoD1 = digitalRead(PIN_D1);
    int estadoD7 = digitalRead(PIN_D7);

    // Empaquetado de datos para IO7
    data["d1_logic"] = estadoD1;
    data["d7_logic"] = estadoD7;
    data["status"] = "running";

    serializeJson(root, msgBuffer);
    
    // Publicación y confirmación en Monitor Serie
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.printf("Enviado OK -> D1: %d | D7: %d\n", estadoD1, estadoD7);
    } else {
        Serial.println("Error al publicar evento");
    }
}

void handleUserMeta() {
    // Permite que la plataforma cambie el tiempo de envío remotamente
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        Serial.printf("Nuevo intervalo recibido: %d ms\n", pubInterval);
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {
    // Aquí podrías agregar código para recibir órdenes desde la web
}

void setup() {
    Serial.begin(115200);

    // Configuración de pines como entrada con Pull-up interna 
    // (Esto evita que el valor 'baile' si no hay nada conectado)
    pinMode(PIN_D1, INPUT_PULLUP);
    pinMode(PIN_D7, INPUT_PULLUP);

    initDevice();

    // Asignación de funciones antes de conectar
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    // Ejecuta la configuración inicial de metadatos
    handleUserMeta();

    // Asegura un intervalo mínimo de 5 segundos si no hay configuración
    if (pubInterval <= 0) {
        pubInterval = 5000;
    }

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
    // Verifica conexión MQTT
    if (!client.connected()) {
        iot_connect();
    }
    
    client.loop();

    // Temporizador basado en el intervalo de la plataforma
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}