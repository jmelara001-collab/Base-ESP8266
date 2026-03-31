#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <IO7F8266.h>

// --- CONFIGURACIÓN DE HARDWARE ---
// R1 = 6.8k (Entrada), R2 = 3.3k (GND)
// Factor real = (6.8 + 3.3) / 3.3 = 3.0606
const float DIVIDER_FACTOR = 3.0606f; 
const int MUESTRAS = 20; // Número de lecturas para promediar y quitar el ruido

Adafruit_ADS1115 ads;
String user_html = "";
char* ssid_pfix = (char*)"IOT_ESP12E_Volt";
unsigned long lastPublishMillis = -pubInterval;

bool adsReady = false;

// Función para obtener una lectura estable (Filtro por promedio)
float obtenerVoltajePromedio() {
    float suma = 0;
    for (int i = 0; i < MUESTRAS; i++) {
        suma += ads.computeVolts(ads.readADC_SingleEnded(3));
        delay(2); // Pequeña pausa para que el ruido cambie entre muestras
    }
    return suma / (float)MUESTRAS;
}

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    float v_pin = 0.0;
    float v_real = 0.0;

    if (adsReady) {
        // Obtenemos el voltaje ya promediado (0 - 3.3V aprox)
        v_pin = obtenerVoltajePromedio();
        
        // Convertimos al voltaje real de la máquina (0 - 10V+)
        v_real = v_pin * DIVIDER_FACTOR;
    }

    // Datos para IO7
    data["status"] = adsReady ? "ok" : "error";
    data["v_adc"] = v_pin;   // Voltaje que entra al chip
    data["v_real"] = v_real; // Voltaje real que mide el sistema

    serializeJson(root, msgBuffer);
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.print(">>> ENVIADO: ");
        Serial.print("Pin ADC: "); Serial.print(v_pin, 3);
        Serial.print("V | Real Maquina: "); Serial.print(v_real, 2);
        Serial.println("V");
    }
}

void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {
    // Espacio para control remoto futuro
}

void setup() {
    Serial.begin(115200);
    delay(500);

    // I2C para ESP-12E (GPIO 4 y 5)
    Wire.begin(4, 5);
    Wire.setClock(100000);

    Serial.println("\n======================================");
    Serial.println("   SISTEMA DE MONITOREO DE VOLTAJE    ");
    Serial.println("======================================");

    // Inicializar ADS1115 (ADDR a GND = 0x48)
    if (!ads.begin(0x48)) {
        Serial.println("ERROR: ADS1115 no encontrado. Revisa I2C.");
        adsReady = false;
    } else {
        Serial.println("ADS1115 detectado correctamente.");
        adsReady = true;
        // GAIN_ONE: +/- 4.096V (Ideal para tu rango de 0-3.3V)
        ads.setGain(GAIN_ONE);
    }

    // Inicializar librería IO7
    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    handleUserMeta();

    if (pubInterval <= 0) pubInterval = 5000;

    // Conexión WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    Serial.print("Conectando a WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.println("\nConexión establecida.");
    set_iot_server();
    iot_connect();
}

void loop() {
    // Mantener comunicación con el servidor
    if (!client.connected()) {
        iot_connect();
    }
    client.loop();

    // Publicar según el intervalo configurado
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}