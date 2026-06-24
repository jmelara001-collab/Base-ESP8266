#include <Arduino.h>
#include <IO7F8266.h>

// =============================================================================
// CONFIGURACIÓN DE LECTURA ANALÓGICA (A0)
// =============================================================================
#define ANALOG_PIN A0

// Función para mapear valores con decimales (float)
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// =============================================================================
// VARIABLES OBLIGATORIAS PARA LA LIBRERÍA IO7
// =============================================================================
String user_html = "";

// Prefijo para el nombre del AP de configuración
char* ssid_pfix = (char*)"IOT_Device";

// Control de tiempo para publicación
unsigned long lastPublishMillis = -pubInterval;

// =============================================================================
// FUNCIONES Y CALLBACKS
// =============================================================================

void publishData() {
    // 1. Enciende el LED integrado (Lógica inversa: LOW = Encendido)
    digitalWrite(LED_BUILTIN, LOW);

    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // Lectura del pin analógico A0 (0 a 1023)
    int analogValue = analogRead(ANALOG_PIN);
    
    // Conversión a voltaje (El ADC del ESP8266 NodeMCU mide de 0V a 3.3V internamente)
    float voltaje = (analogValue * 3.3) / 1023.0;
    
    // Mapeo: 0.0V -> 0.0 m/min  |  3.3V -> 200.0 m/min
    float velocidad = mapFloat(voltaje, 0.0, 3.3, 0.0, 200.0);
    
    // Control de seguridad por si hay un leve ruido en la lectura analógica
    if (velocidad < 0.0) velocidad = 0.0;
    if (velocidad > 200.0) velocidad = 200.0;

    // Redondeo a 2 decimales para la trama JSON
    velocidad = round(velocidad * 100.0) / 100.0;
    voltaje = round(voltaje * 100.0) / 100.0;

    // --- ESTRUCTURA DE DATOS ENVIADA ---
    data["status"] = "running";
    data["voltaje"] = voltaje;
    data["velocidad"] = velocidad; // Metros por minuto
    // ----------------------------

    serializeJson(root, msgBuffer);
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.printf("Evento enviado a IO7 OK | Voltaje: %.2fV | Vel: %.2f m/min\n", voltaje, velocidad);
    } else {
        Serial.println("Error al enviar a IO7");
    }

    // 2. Apaga el LED integrado tras el envío (Lógica inversa: HIGH = Apagado)
    digitalWrite(LED_BUILTIN, HIGH);
}

void handleUserMeta() {
    // Sincroniza el intervalo de publicación con la plataforma
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        Serial.printf("Intervalo actualizado: %d ms\n", pubInterval);
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {
    // --- LÓGICA DE COMANDOS AQUÍ ---
}

// =============================================================================
// ARDUINO SETUP & LOOP
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n--- Iniciando Nodo de Monitoreo Velocidad (A0) + IO7 ---");

    // Configurar el pin del LED integrado como salida y asegurar que inicie apagado (HIGH)
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    // Inicialización del dispositivo IO7 y carga de configuración
    initDevice();

    // Registro de funciones Callback de IO7
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    // Aplicar configuración inicial de metadatos
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
    // Mantener conexión MQTT activa con el Broker de IO7
    if (!client.connected()) {
        iot_connect();
    }
    
    client.loop();

    // Temporizador cíclico de publicación basado en el intervalo
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}   