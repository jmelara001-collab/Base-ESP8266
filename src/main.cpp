#include <Arduino.h>
#include <IO7F8266.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

// Instanciar el ADC ADS1115
Adafruit_ADS1115 ads;

// Pines I2C asignados según el estándar del ESP-12E
#define I2C_SDA 4  // GPIO4
#define I2C_SCL 5  // GPIO5

// Multiplicador directo: Convierte el voltaje leído (V_RMS) a Amperios reales (0 a 50A)
const float FACTOR_DONA_DIRECTA = 50.0; 

// Variables globales para almacenar las lecturas procesadas
float vRMS = 0.0;
float corrienteReal50A = 0.0;

// Variable obligatoria para la librería IO7
String user_html = "";

// Prefijo para el nombre del AP de configuración
char* ssid_pfix = (char*)"L1M1_IOT_Device";

// Control de tiempo para publicación de alta resolución (100ms = 10Hz)
unsigned long lastPublishMillis = 0;
const unsigned long INTERVALO_ENVIO_MS = 100; 

/**
 * Captura EXACTAMENTE 1 ciclo completo de red a 60 Hz (16666 microsegundos).
 * Esto elimina la inercia del promedio de 200ms y entrega el comportamiento real de la corriente.
 */
float calcularVoltajeRMS() {
    double sumaCuadrados = 0;
    int numeroMuestras = 0;
    unsigned long tiempoInicio = micros();
    
    // Muestreo enfocado en 1 ciclo de 60Hz
    while (micros() - tiempoInicio < 16666) {
        int16_t lecturaRaw = ads.readADC_Differential_2_3();
        float voltajeInstantaneo = lecturaRaw * 0.000125; // GAIN_ONE -> 0.125mV por bit
        sumaCuadrados += (voltajeInstantaneo * voltajeInstantaneo);
        numeroMuestras++;
    }
    
    if (numeroMuestras == 0) return 0.0;
    return sqrt((float)(sumaCuadrados / numeroMuestras));
}

void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // 1. Muestreo ultrarrápido de 1 ciclo (16.6ms)
    vRMS = calcularVoltajeRMS();
    
    // Filtro para eliminar el ruido de fondo constante (4.5mV de offset)
    if (vRMS <= 0.0046) {
        vRMS = 0.0;
    }

    // 2. Cálculo directo (Voltaje RMS * Factor de la Dona)
    corrienteReal50A = vRMS * FACTOR_DONA_DIRECTA; 
    
    // 3. Conversión a 3 decimales
    String vRMS_str = String(vRMS, 3);
    String iReal_str = String(corrienteReal50A, 3);

    // 4. Formato de JSON idéntico al tuyo
    data["status"] = "running";
    data["v_real"] = vRMS_str;  
    data["i_real"] = iReal_str; // Envía la corriente directa de 0 a 50A

    serializeJson(root, msgBuffer);
    if (client.publish(evtTopic, msgBuffer)) {
        // Publicación silenciosa o mínima para no retrasar el bucle
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

void handleUserCommand(char* topic, JsonDocument* root) {
    // Lógica de comandos si es necesaria
}

void setup() {
    Serial.begin(115200);

    // Bus I2C a 400kHz para máxima velocidad de lectura del ADS1115
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

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
        delay(500);
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

    // Muestrea y publica en tiempo real cada 100 ms (10 lecturas por segundo)
    if (millis() - lastPublishMillis >= INTERVALO_ENVIO_MS) {
        publishData();
        lastPublishMillis = millis();
    }
}