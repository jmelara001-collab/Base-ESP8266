#include <Arduino.h>
#include <IO7F8266.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

// Instanciar el ADC ADS1115
Adafruit_ADS1115 ads;

// Pines I2C asignados según el estándar del ESP-12E
#define I2C_SDA 4  // GPIO4
#define I2C_SCL 5  // GPIO5

// Relaciones de conversión de instrumentación
const float FACTOR_DONA = 50.0; 
const float FACTOR_TABLERO = 50.0; // <-- CAMBIADO: Ahora 5A medidos corresponden a 250A reales (250 / 5 = 50)

// Variables globales para almacenar las lecturas procesadas
float vRMS = 0.0;
float corrienteAmperimetro = 0.0;
float corrienteReal250A = 0.0;

// Variable obligatoria para la librería IO7
String user_html = "";

// Prefijo para el nombre del AP de configuración
char* ssid_pfix = (char*)"IOT_Device";

// Control de tiempo para publicación
unsigned long lastPublishMillis = -pubInterval;

// Función dedicada a capturar los 200ms de onda senoidal de forma óptima
float calcularVoltajeRMS() {
    double sumaCuadrados = 0;
    int numeroMuestras = 0;
    unsigned long tiempoInicio = millis();
    
    while (millis() - tiempoInicio < 200) {
        int16_t lecturaRaw = ads.readADC_Differential_2_3();
        float voltajeInstantaneo = lecturaRaw * 0.000125;
        sumaCuadrados += (voltajeInstantaneo * voltajeInstantaneo);
        numeroMuestras++;
    }
    
    if (numeroMuestras == 0) return 0.0;
    return sqrt((float)(sumaCuadrados / numeroMuestras));
}

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // 1. Ejecutar la medición del ADC
    vRMS = calcularVoltajeRMS();
    
    // Filtro para eliminar el ruido de fondo constante (4.5mV de offset)
    if (vRMS <= 0.0046) {
        vRMS = 0.0;
    }

    // 2. Realizar los cálculos del mapeo lineal
    corrienteAmperimetro = vRMS * FACTOR_DONA;
    corrienteReal250A = corrienteAmperimetro * FACTOR_TABLERO; // Escala corregida a un máximo de 250A
    
    // 3. --- CONVERSIÓN ESTRICTA A 3 DECIMALES ---
    String vRMS_str = String(vRMS, 3);
    String iAmp_str = String(corrienteAmperimetro, 3);
    String iReal_str = String(corrienteReal250A, 3);

    // 4. --- ENVIANDO DATOS EN FORMATO TEXTO SEGURO A IO7 ---
    data["status"] = "running";
    data["voltaje_sensor"] = vRMS_str;  
    data["i_amperimetro"] = iAmp_str;   
    data["i_real"] = iReal_str;         // Cambiado dinámicamente para el nuevo límite de 250A
    // -----------------------------------------------------

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
    // --- LÓGICA DE COMANDOS AQUÍ SI ES NECESARIO ---
}

void setup() {
    Serial.begin(115200);

    // Inicializar y acelerar el bus I2C a 400kHz para optimizar recursos del ESP8266
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    // Inicializar el ADS1115
    if (!ads.begin()) {
        Serial.println("¡Error Crítico! No se pudo encontrar el ADS1115.");
        while (1);
    }
    
    // Configurar los parámetros de alta velocidad del ADC
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_860SPS);

    // Inicialización del dispositivo IO7 y carga de configuración
    initDevice();

    // Registro de funciones Callback
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    // Aplicar configuración inicial
    handleUserMeta();

    // Cambiado explícitamente a 1000 ms (1 segundo) por defecto
    if (pubInterval <= 0) pubInterval = 1000; 

    // Conexión WiFi básica de la librería
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

    // Temporizador de publicación basado en pubInterval
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}