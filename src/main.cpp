#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <WiFi.h>
#include <IO7F32.h> // Cambiado a la versión de librería compatible con ESP32

// Instanciar el ADC ADS1115
Adafruit_ADS1115 ads;

// Pines I2C por defecto en el ESP32 DevKit V1
#define I2C_SDA 18  // GPIO21 (SDA)
#define I2C_SCL 19  // GPIO22 (SCL)

// Multiplicador directo: Convierte el voltaje leído (V_RMS) a Amperios reales (0 a 50A)
const float FACTOR_DONA_DIRECTA = 50.0; 

// Variables globales para almacenar las lecturas procesadas
float vRMS = 0.0;
float vPico = 0.0;
float corrienteReal50A = 0.0;
float corrientePico50A = 0.0;
float factorCresta = 0.0;

// Variable obligatoria para la librería IO7
String user_html = "";

// Prefijo para el nombre del AP de configuración
char* ssid_pfix = (char*)"M1_IOT_Device";

// Control de tiempo para publicación de alta resolución (100ms = 10Hz)
unsigned long lastPublishMillis = 0;
const unsigned long INTERVALO_ENVIO_MS = 100; 

/**
 * Captura EXACTAMENTE 1 ciclo completo de red a 60 Hz (16666 us).
 * Mide el RMS y registra el Pico Instantáneo para análisis de distorsión/IA.
 */
void calcularMetricasCiclo() {
    double sumaCuadrados = 0;
    int numeroMuestras = 0;
    float picoDetectado = 0.0;
    unsigned long tiempoInicio = micros();
    
    // Muestreo enfocado en 1 ciclo de 60Hz
    while (micros() - tiempoInicio < 16666) {
        int16_t lecturaRaw = ads.readADC_Differential_2_3();
        float voltajeInstantaneo = lecturaRaw * 0.000125; // GAIN_ONE -> 0.125mV por bit
        
        float valAbs = fabs(voltajeInstantaneo);
        if (valAbs > picoDetectado) {
            picoDetectado = valAbs; // Retiene el punto más alto del ciclo
        }

        sumaCuadrados += (voltajeInstantaneo * voltajeInstantaneo);
        numeroMuestras++;
    }
    
    if (numeroMuestras == 0) {
        vRMS = 0.0;
        vPico = 0.0;
        return;
    }

    vRMS = sqrt((float)(sumaCuadrados / numeroMuestras));
    vPico = picoDetectado;
}

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // 1. Muestreo del ciclo
    calcularMetricasCiclo();
    
    // Filtro para eliminar el ruido de fondo constante
    if (vRMS <= 0.01) {
        vRMS = 0.0;
        vPico = 0.0;
    }

    // 2. Cálculos directos de magnitudes físicas
    corrienteReal50A = vRMS * FACTOR_DONA_DIRECTA; 
    corrientePico50A = vPico * FACTOR_DONA_DIRECTA;

    // Cálculo del Factor de Cresta (Métrica extra para el modelo de IA)
    if (corrienteReal50A > 0.1) {
        factorCresta = corrientePico50A / corrienteReal50A;
    } else {
        factorCresta = 0.0;
    }
    
    // 3. Conversión a cadenas de texto de alta precisión
    String vRMS_str = String(vRMS, 3);
    String iReal_str = String(corrienteReal50A, 3);
    String iPico_str = String(corrientePico50A, 3);
    String fCresta_str = String(factorCresta, 2);

    // 4. Formato JSON enriquecido para monitoreo e IA
    data["status"] = "running";
    data["v_real"] = vRMS_str;  
    data["i_real"] = iReal_str;     // Corriente RMS (0 a 50A)
    data["i_pico"] = iPico_str;     // Pico de corriente instantáneo
    data["f_cresta"] = fCresta_str; // Indicador de deformación (Normal sin distorsión ~1.41)

    serializeJson(root, msgBuffer);
    if (!client.publish(evtTopic, msgBuffer)) {
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
    // Lógica de comandos
}

void setup() {
    Serial.begin(115200);

    // Inicialización del bus I2C en los pines por defecto del ESP32
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000); // 400 kHz

    if (!ads.begin()) {
        Serial.println("¡Error Crítico! No se pudo encontrar el ADS1115.");
        while (1);
    }
    
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_860SPS);

    initDevice();

    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    handleUserMeta();

    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    Serial.print("Conectando WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
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

    // Muestreo y envío a 10 Hz (100 ms)
    if (millis() - lastPublishMillis >= INTERVALO_ENVIO_MS) {
        publishData();
        lastPublishMillis = millis();
    }
}