#include <Arduino.h>
#include <IO7F8266.h>
#include <SoftwareSerial.h>
#include <ModbusMaster.h>

// ==========================================
// CONFIGURACIÓN PINOUT RS-485 / MODBUS
// ==========================================
#define RX_PIN 14    // D5 (GPIO14) -> RO del MAX485
#define TX_PIN 12    // D6 (GPIO12) -> DI del MAX485
#define DE_RE_PIN 4  // D2 (GPIO4)  -> DE y RE unidos

// Parámetros confirmados del Circutor Computer Smart III
#define SMART3_SLAVE_ID 1
#define SMART3_BAUDRATE 19200

SoftwareSerial rs485(RX_PIN, TX_PIN);
ModbusMaster smart3Node;

// Variables obligatorias para IO7F8266
String user_html = "";
char* ssid_pfix = (char*)"IOT_Device";

// Control de tiempo para publicación
unsigned long lastPublishMillis = -pubInterval;

// Control de dirección para el transceptor MAX485
void preTransmission() {
    digitalWrite(DE_RE_PIN, HIGH);
    delayMicroseconds(200);
}

void postTransmission() {
    delayMicroseconds(200);
    digitalWrite(DE_RE_PIN, LOW);
}

// Unir dos registros de 16 bits (High y Low) a 32 bits según el estándar del Smart III
uint32_t combineRegisters(uint16_t high, uint16_t low) {
    return ((uint32_t)high << 16) | low;
}

// ==========================================
// FUNCIÓN DE PUBLICACIÓN A PLATAFORMA IO7
// ==========================================
void publishData() {
    StaticJsonDocument<512> root;
    JsonObject data = root.createNestedObject("d");
    
    // Variables temporales para almacenar lecturas
    float kw_inst = 0;
    float pf = 0;
    float mwh = 0;
    bool read_ok = true;
    uint8_t err_code = 0;

    // --- LECTURA 1: Potencia Instantánea y Cos Phi (Pantalla Principal) ---
    // Leemos 18 registros empezando en 0x0052 hasta 0x0063
    uint8_t result1 = smart3Node.readInputRegisters(0x0052, 18);

    if (result1 == smart3Node.ku8MBSuccess) {
        // Potencia Activa Instantánea (0x0052) -> Offset 0 y 1
        uint32_t rawW = combineRegisters(smart3Node.getResponseBuffer(0), smart3Node.getResponseBuffer(1));
        
        // Cos Phi trifásico (0x0062) -> Offset 16 y 17 (0x62 - 0x52 = 0x10 = 16 decimal)
        // Este es el valor que el Circutor muestra en su pantalla principal para la compensación
        uint32_t rawCosPhi = combineRegisters(smart3Node.getResponseBuffer(16), smart3Node.getResponseBuffer(17));
        
        kw_inst = rawW / 1000.0;    // Convertir Vatios a Kilovatios
        
        // Autodetección de escala para el Cos Phi
        if (rawCosPhi > 100) {
            pf = rawCosPhi / 1000.0;    // Escala x1000 (Ej. 937 -> 0.94)
        } else {
            pf = rawCosPhi / 100.0;     // Escala x100 (Ej. 94 -> 0.94)
        }
        
    } else {
        read_ok = false;
        err_code = result1;
    }

    // Pequeña pausa para no saturar el bus RS-485
    delay(50);

    // --- LECTURA 2: Energía Activa Consumida ---
    // Leemos 2 registros desde 0x0088 (Energía activa consumida en kWh)
    uint8_t result2 = smart3Node.readInputRegisters(0x0088, 2);

    if (result2 == smart3Node.ku8MBSuccess) {
        // Energía Acumulada (0x0088) -> Offset 0 y 1
        uint32_t rawKWh = combineRegisters(smart3Node.getResponseBuffer(0), smart3Node.getResponseBuffer(1));
        
        // Conversión a Megavatios-hora (MWh) dividiendo entre 1000
        mwh = rawKWh / 1000.0; 
    } else {
        read_ok = false;
        err_code = result2; // Sobrescribe el error si la lectura 2 falla
    }

    // --- CONSTRUCCIÓN DEL JSON ---
    if (read_ok) {
        data["status"] = "ok";
        
        // Convertimos a texto con 2 decimales limpios
        data["kw_inst"] = serialized(String(kw_inst, 2));
        data["mwh"]     = serialized(String(mwh, 2));
        data["pf"]      = serialized(String(pf, 2)); // Mandamos el Cos Phi bajo la etiqueta pf

        Serial.printf("Smart III OK | kW Inst: %.2f kW | MWh Acumulado: %.2f MWh | Cos phi (Pantalla): %.2f\n", kw_inst, mwh, pf);
    } else {
        // Reportar error de bus Modbus a IO7
        data["status"] = "modbus_error";
        data["modbus_code"] = err_code;
        data["kw_inst"] = 0;
        data["mwh"]     = 0;
        data["pf"]      = 0;

        Serial.printf("Error Modbus Smart III: 0x%02X\n", err_code);
    }

    // Envío del paquete JSON a la plataforma
    serializeJson(root, msgBuffer);
    if (client.publish(evtTopic, msgBuffer)) {
        Serial.println("Evento enviado a IO7 OK");
        
        // --- DESTELLO DEL LED INTERNO ---
        digitalWrite(LED_BUILTIN, LOW);  // Enciende el LED (lógica inversa)
        delay(30);                       // Pausa muy rápida de 30 ms (solo un flash)
        digitalWrite(LED_BUILTIN, HIGH); // Apaga el LED
        
    } else {
        Serial.println("Error al enviar a IO7 (Red/MQTT)");
    }
}

void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        Serial.printf("Intervalo actualizado: %d ms\n", pubInterval);
    }
}

void handleUserCommand(char* topic, JsonDocument* root) {
    // Reservado para comandos de control
}

void setup() {
    Serial.begin(115200);

    // Configuración del LED interno del ESP8266
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH); // Aseguramos que inicie apagado (HIGH)

    // Pines de control para RS-485
    pinMode(DE_RE_PIN, OUTPUT);
    digitalWrite(DE_RE_PIN, LOW);

    // Inicialización del bus RS-485 a 19200 baudios
    rs485.begin(SMART3_BAUDRATE);
    
    smart3Node.begin(SMART3_SLAVE_ID, rs485);
    smart3Node.preTransmission(preTransmission);
    smart3Node.postTransmission(postTransmission);

    initDevice();

    userMeta = handleUserMeta;
    userCommand = handleUserCommand;

    handleUserMeta();

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

    // Servidor IO7
    set_iot_server();
    iot_connect();
}

void loop() {
    // Mantener la conexión MQTT activa independientemente del Modbus
    if (!client.connected()) {
        iot_connect();
    }
    
    client.loop();

    // Temporizador no bloqueante para publicación de datos
    if ((pubInterval != 0) && (millis() - lastPublishMillis > (unsigned long)pubInterval)) {
        publishData();
        lastPublishMillis = millis();
    }
}