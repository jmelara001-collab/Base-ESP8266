#include <Arduino.h>
#include <IO7F32.h>      // Librería específica para ESP32
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

// Instancia del ADC externo
Adafruit_ADS1115 ads;

// ====== VARIABLES DE CALIBRACIÓN Y FILTRADO ======
// Ajustada matemáticamente para corregir la sutil saturación del núcleo de ferrita.
// Relación validada en pruebas: (15.4A del Tester / 14.73A del Monitor anterior) * 50 = 52.27
const float FACTOR_CALIBRADO = 52.27; 

// Variables globales para el manejo dinámico de la escala (Autorango por Software)
adsGain_t gananciaActual = GAIN_TWO;
float rangoVoltajeActual = 2.048;
float corrienteRMS = 0.0;
float voltajeRMS = 0.0;

// Variable obligatoria para la interfaz de la librería IO7F32
String user_html = "";

// Nombre del punto de acceso para configuración local si falla el WiFi
char* ssid_pfix = (char*)"IOT_DEVICE";

// Control de tiempo para publicación (envío inmediato al conectar)
unsigned long lastPublishMillis = -pubInterval;

// Función para cambiar la escala del PGA interno del ADS1115 por comandos
void configurarEscala(int opcion) {
    switch(opcion) {
        case 1: // Escala Alta (Hasta 50A) -> Rango +-2.048V
            gananciaActual = GAIN_TWO;
            rangoVoltajeActual = 2.048;
            break;
        case 2: // Escala Media (Hasta ~25A) -> Rango +-1.024V
            gananciaActual = GAIN_FOUR;
            rangoVoltajeActual = 1.024;
            break;
        case 3: // Escala Baja (Hasta ~12A) -> Rango +-0.512V
            gananciaActual = GAIN_EIGHT;
            rangoVoltajeActual = 0.512;
            break;
        case 4: // Escala Milivoltios (Señales críticas) -> Rango +-0.256V
            gananciaActual = GAIN_SIXTEEN;
            rangoVoltajeActual = 0.256;
            break;
        default:
            return;
    }
    ads.setGain(gananciaActual);
    Serial.printf("[ADS1115] Escala cambiada por comando a opción: %d (Rango: +-%fV)\n", opcion, rangoVoltajeActual);
}

void publishData() {
    float sumaCuadrados = 0.0; 
    int muestras = 400; // 400 muestras consecutivas rápidas cubren de sobra los ciclos a 60Hz
    int contador = 0;

    // 1. Muestreo de hardware de alta velocidad del canal diferencial (A0 - A1)
    while(contador < muestras) {
        int16_t resultadoRaw = ads.readADC_Differential_0_1(); 
        
        // Convertir el valor binario a Voltaje real según la escala activa
        float voltaje = resultadoRaw * (rangoVoltajeActual / 32768.0);
        
        // Acumular los cuadrados de los voltajes en una variable flotante
        sumaCuadrados += (voltaje * voltaje);
        contador++;
    }

    // 2. Procesamiento matemático del Voltaje RMS
    voltajeRMS = sqrt(sumaCuadrados / (float)muestras);
    
    // 3. Conversión a Corriente Real usando tu factor calibrado
    corrienteRMS = voltajeRMS * FACTOR_CALIBRADO;

    // 4. UMBRAL DE CERO (Noise Gate):
    // Si la lectura está por debajo de 0.25 Amperios, forzamos un cero limpio en la plataforma
    if (corrienteRMS < 0.25) {
        corrienteRMS = 0.00;
    }

    // Mostrar medición en local antes de transmitir por red
    Serial.print("[MEDICIÓN] Voltaje: ");
    Serial.print(voltajeRMS, 4);
    Serial.print(" V RMS | Corriente: ");
    Serial.print(corrienteRMS, 2);
    Serial.println(" A");

    // 5. Empaquetar y enviar datos estructurados JSON por MQTT
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // --- PAYLOAD ENVIADO AL DASHBOARD ---
    data["status"] = "running";
    data["corriente"] = serialized(String(corrienteRMS, 2)); // Forzar transmisión redondeada a 2 decimales
    data["voltaje_sensor"] = serialized(String(voltajeRMS, 4));
    data["escala_v"] = rangoVoltajeActual;
    // ------------------------------------

    serializeJson(root, msgBuffer);
    
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.println("[MQTT] Evento de telemetría enviado a IO7 OK");
    } else {
        Serial.println("[MQTT] Error al enviar paquete a IO7");
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
    // --- LÓGICA DE COMANDOS: CAMBIO DE ESCALA DESDE EL DASHBOARD ---
    // Estructura del JSON esperada desde el widget/botón web: {"d": {"escala": X}} (Donde X va de 1 a 4)
    JsonObject d = (*root)["d"];
    if (d.containsKey("escala")) {
        int nuevaEscala = d["escala"].as<int>();
        configurarEscala(nuevaEscala);
    }
}

void setup() {
    Serial.begin(115200);

    // Inicializar I2C en los pines nativos del ESP32 DevKit V1 (SDA=21, SCL=22)
    Wire.begin(18, 19); 

    // Inicializar el módulo ADC externo
    if (!ads.begin()) {
        Serial.println("¡ALERTA: No se detectó el módulo ADS1115 en los pines D21 y D22!");
        // No bloqueamos con un while(1) aquí para permitir que la librería IO7 levante su Access Point de respaldo si es necesario
    } else {
        Serial.println("[ADS1115] Inicializado correctamente en pines de hardware.");
        ads.setDataRate(RATE_ADS1115_860SPS); // Forzar máxima velocidad de muestreo
        configurarEscala(1);                  // Arrancar siempre en la escala más protegida (+-2.048V)
    }

    // Inicialización del dispositivo y carga del sistema de archivos interno (LittleFS)
    initDevice();

    // Registro de funciones Callback (Meta y Comandos)
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    // Aplicar configuración inicial de la plataforma
    handleUserMeta();

    // Asegura un intervalo por defecto si no existe configuración previa
    if (pubInterval <= 0) pubInterval = 5000;

    // Conexión WiFi en modo Estación utilizando las credenciales cargadas de memoria
    WiFi.mode(WIFI_STA);
    WiFi.begin((const char*)cfg["ssid"], (const char*)cfg["w_pw"]);
    
    Serial.print("Conectando WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.printf("\nConectado de forma exitosa | IP Asignada: %s\n", WiFi.localIP().toString().c_str());

    // Configuración de credenciales de red y conexión inicial al broker MQTT
    set_iot_server();
    iot_connect();
}

void loop() {
    // Mantiene la conexión viva con el broker MQTT y reconecta automáticamente si cae el enlace
    if (!client.connected()) {
        iot_connect();
    }
    
    client.loop();

    // Temporizador de publicación asíncrono basado en pubInterval
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}