#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <IO7F8266.h>

Adafruit_ADS1115 ads;
String user_html = "";
char* ssid_pfix = (char*)"IOT_ESP12E_Volt";
unsigned long lastPublishMillis = -pubInterval;

// Bandera de estado del sensor
bool adsReady = false;

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    float v_pin = 0.0;
    float v_10v = 0.0;

    if (adsReady) {
        // Leer el canal 3 (A3)
        int16_t adc_raw = ads.readADC_SingleEnded(3);
        
        // Voltaje real en el pin (debería ser 2.0V cuando hay 10V en la entrada)
        v_pin = ads.computeVolts(adc_raw);
        
        // Conversión a escala 10V (Factor 5:1 según tu calibración)
        v_10v = v_pin * 5.0;
    }

    // Datos para la plataforma
    data["status"] = adsReady ? "ok" : "error";
    data["v_adc"] = v_pin;   // El voltaje que lee el chip (0-2V)
    data["v_real"] = v_10v;  // El voltaje de tu máquina (0-10V)

    serializeJson(root, msgBuffer);
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.printf("Lectura Enviada -> ADC: %.3fV | Real: %.2fV\n", v_pin, v_10v);
    }
}

void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {
    // Espacio para lógica de control futura
}

void setup() {
    Serial.begin(115200);
    delay(500);

    // Configuración I2C para ESP-12E (D2=SDA, D1=SCL)
    Wire.begin(4, 5);
    Wire.setClock(100000);

    Serial.println("\n--- Inicializando Monitor de Voltaje ---");

    // Inicializar ADS1115 en dirección 0x48 (ADDR -> GND)
    if (!ads.begin(0x48)) {
        Serial.println("Error: ADS1115 no encontrado. Revisa el cableado.");
        adsReady = false;
    } else {
        Serial.println("ADS1115 conectado correctamente.");
        adsReady = true;
        // GAIN_ONE: Rango de +/- 4.096V (Perfecto para leer tus 2V con precisión)
        ads.setGain(GAIN_ONE);
    }

    // Configuración de librería IO7
    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    handleUserMeta();

    if (pubInterval <= 0) pubInterval = 5000;

    // Conexión WiFi con los datos de configuración
    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.println("\nWiFi Conectado exitosamente.");
    set_iot_server();
    iot_connect();
}

void loop() {
    // Mantener conexión con el servidor de IO7
    if (!client.connected()) {
        iot_connect();
    }
    client.loop();

    // Publicación temporizada
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}