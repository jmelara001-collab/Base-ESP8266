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
const float FACTOR_TABLERO = 160.0; // Relación 800/5

// Variables globales para almacenar las lecturas procesadas
float vRMS = 0.0;
float corrienteAmperimetro = 0.0;
float corrienteReal = 0.0;
float potenciaKW = 0.0;

// Variable obligatoria para la librería IO7
String user_html = "";

// Prefijo para el nombre del AP de configuración
char* ssid_pfix = (char*)"IOT_Device";

// Control de tiempo para publicación
unsigned long lastPublishMillis = -pubInterval;

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
    
    if (vRMS <= 0.0046) {
        vRMS = 0.0;
    }

    // 2. Realizar los cálculos
    corrienteAmperimetro = vRMS * FACTOR_DONA;
    corrienteReal = corrienteAmperimetro * FACTOR_TABLERO;
    
    // Cálculo de potencia: P = (V * I * PF * sqrt(3)) / 1000
    potenciaKW = ((440.0 * corrienteReal * 0.94 * 1.732) / 1000.0) * 1.427;

    // 3. Formateo a string
    String vRMS_str = String(vRMS, 3);
    String iAmp_str = String(corrienteAmperimetro, 3);
    String iReal_str = String(corrienteReal, 3);
    String pKW_str = String(potenciaKW, 3);

    // 4. Enviando todos los datos originales + la nueva potencia
    data["status"] = "running";
    data["voltaje_sensor"] = vRMS_str;  
    data["i_amperimetro"] = iAmp_str;   
    data["i_real"] = iReal_str;
    data["potencia_kw"] = pKW_str; // Dato agregado

    serializeJson(root, msgBuffer);
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.println("Evento enviado a IO7 OK");
    } else {
        Serial.println("Error al enviar a IO7");
    }
}

void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {}

void setup() {
    Serial.begin(115200);
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    if (!ads.begin()) {
        while (1);
    }
    
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_860SPS);

    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    handleUserMeta();
    if (pubInterval <= 0) pubInterval = 1000; 

    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }

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