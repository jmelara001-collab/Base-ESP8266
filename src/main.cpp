#include <Arduino.h>
#include <WiFi.h>      
#include <IO7F32.h>    
#include <esp_timer.h>

// --- PROTOTIPOS DE FUNCIONES ---
void core0NetworkTask(void * pvParameters);
void publishData();
void handleUserMeta();
void handleUserCommand(char* topic, JsonDocument* root);
void initSensor();
float calcularRPM();

// --- VARIABLES PARA EL CONTROL DE RED ---
unsigned long wifiDownMillis = 0;        
const unsigned long RESTART_TIMEOUT = 300000; // 5 minutos de espera offline (WiFi o SSL) antes de reiniciar radio

String user_html = "";  
char* ssid_pfix = (char*)"V_R";

unsigned long lastPublishMillis = 0;
int defaultPubIntervalMs = 5000;

uint32_t reconnecciones_wifi = 0;    
bool wifiWasConnected = false;      

// --- PARÁMETROS DE LA MÁQUINA (configurables desde meta) ---
const int PIN_SENSOR = 18; 
const int LED_PIN = 2;

uint32_t PULSOS_POR_REV = 360;        // Cuántos pulsos da el sensor por cada revolución
int TIPO_FLANCO = HIGH;             // HIGH = Cuenta al subir, LOW = Cuenta al bajar
float RPM_MAX = 50000.0f;            // Velocidad máxima de la máquina (+20-30% de margen). Define el filtro anti-ruido
float RPM_MIN = 10.0f;               // Velocidad más lenta a la que trabaja. Más lento que esto = detenido

// --- CONSTANTES INTERNAS (no necesitan ajuste) ---
const float VUELTAS_SIN_PULSO_PARO = 3.0f; // Si pasan 3 periodos sin pulso -> detenido
const float FRACCION_FILTRO = 0.5f;        // Ignora pulsos que lleguen antes de la mitad del periodo a RPM_MAX

volatile int64_t periodoMaxUs = 12000000;  // Periodo a RPM_MIN (calculado en initSensor)
volatile int64_t filtroUs = 10000;         // Periodo mínimo aceptado (calculado en initSensor)

// --- VARIABLES COMPARTIDAS CON LA INTERRUPCIÓN (ISR) ---
portMUX_TYPE pulsoMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t isr_pulsos = 0;     // Pulsos válidos totales
volatile int64_t  isr_t_ultimo = 0;   // Marca de tiempo (us) del último pulso
volatile uint32_t isr_periodo = 0;    // Último periodo medido entre pulsos (us), 0 = aún no hay
volatile uint32_t isr_n_tramo = 0;    // Índice del pulso con que arrancó el giro actual
volatile int64_t  isr_t_tramo = 0;    // Tiempo del pulso con que arrancó el giro actual

// --- ESTADO DE LA ÚLTIMA PUBLICACIÓN (para el promedio exacto entre publicaciones) ---
uint32_t pub_n = 0;
int64_t  pub_t = 0;

// --- TAREA PARA EL CORE 0 (RED) ---
TaskHandle_t NetworkTaskHandle;

// ---------------------------------------------------------------------------
// INTERRUPCIÓN: GUARDA EL INSTANTE EXACTO DE CADA PULSO
// ---------------------------------------------------------------------------
void IRAM_ATTR isrPulso() {
    int64_t t = esp_timer_get_time();   // Microsegundos desde el arranque (64 bits, no se desborda)

    portENTER_CRITICAL_ISR(&pulsoMux);
    int64_t dt = t - isr_t_ultimo;

    if (isr_pulsos == 0 || dt > periodoMaxUs) {
        // Primer pulso o arranque después de estar detenido: inicia un nuevo tramo
        isr_pulsos++;
        isr_periodo = 0;
        isr_n_tramo = isr_pulsos;
        isr_t_tramo = t;
        isr_t_ultimo = t;
    }
    else if (dt >= filtroUs) {
        // Pulso válido
        isr_pulsos++;
        isr_periodo = (uint32_t)dt;
        isr_t_ultimo = t;
    }
    // Si llegó antes de lo físicamente posible es rebote/ruido y se ignora
    portEXIT_CRITICAL_ISR(&pulsoMux);
}

// ---------------------------------------------------------------------------
// CONFIGURACIÓN DEL SENSOR (calcula filtro y tiempo de paro a partir de las RPM)
// ---------------------------------------------------------------------------
void initSensor() {
    if (PULSOS_POR_REV == 0) PULSOS_POR_REV = 1;
    if (RPM_MIN < 0.1f) RPM_MIN = 0.1f;
    if (RPM_MAX <= RPM_MIN) RPM_MAX = RPM_MIN * 10.0f;

    periodoMaxUs = (int64_t)(60000000.0f / (RPM_MIN * (float)PULSOS_POR_REV));
    filtroUs     = (int64_t)(FRACCION_FILTRO * 60000000.0f / (RPM_MAX * (float)PULSOS_POR_REV));

    detachInterrupt(digitalPinToInterrupt(PIN_SENSOR));
    pinMode(PIN_SENSOR, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_SENSOR), isrPulso, (TIPO_FLANCO == HIGH) ? RISING : FALLING);

    Serial.printf("[SENSOR] Pulsos/rev: %u | RPM max: %.1f | RPM min: %.1f | Filtro: %lld us\n",
                  PULSOS_POR_REV, RPM_MAX, RPM_MIN, filtroUs);
}

// ---------------------------------------------------------------------------
// CÁLCULO DE RPM (se llama justo antes de publicar)
// Devuelve la velocidad promedio exacta desde la publicación anterior, o 0 si está detenido
// ---------------------------------------------------------------------------
float calcularRPM() {
    // Copia atómica de los datos de la ISR
    portENTER_CRITICAL(&pulsoMux);
    uint32_t n       = isr_pulsos;
    int64_t  t_ult   = isr_t_ultimo;
    uint32_t periodo = isr_periodo;
    uint32_t n_tramo = isr_n_tramo;
    int64_t  t_tramo = isr_t_tramo;
    portEXIT_CRITICAL(&pulsoMux);

    int64_t ahora = esp_timer_get_time();
    const float K = 60000000.0f / (float)PULSOS_POR_REV;   // us -> RPM
    float rpm = 0.0f;

    // 1) Promedio exacto: vueltas completas entre el último pulso de la publicación
    //    anterior y el último pulso de ahora, dividido entre su tiempo real.
    uint32_t base_n = pub_n;
    int64_t  base_t = pub_t;
    if (base_n < n_tramo) {          // Hubo un arranque: no promediar el tiempo detenido
        base_n = n_tramo;
        base_t = t_tramo;
    }
    uint32_t intervalos = n - base_n;
    if (intervalos > 0 && t_ult > base_t) {
        rpm = K * (float)intervalos / (float)(t_ult - base_t);
    } else if (periodo > 0) {
        rpm = K / (float)periodo;    // No llegó ningún pulso nuevo: usa el último periodo
    }

    pub_n = n;
    pub_t = t_ult;

    // 2) Detección de paro según el propio ritmo del eje
    int64_t transcurrido = ahora - t_ult;

    if (n == 0 || periodo == 0) return 0.0f;                                             // Nunca giró o aún no hay 2 pulsos
    if (transcurrido > periodoMaxUs) return 0.0f;                                        // Más lento que RPM_MIN
    if (transcurrido > (int64_t)(VUELTAS_SIN_PULSO_PARO * (float)periodo)) return 0.0f;  // Varias vueltas sin pulso: detenido

    // 3) Si el pulso se está atrasando, la velocidad real no puede ser mayor que esta cota
    if (transcurrido > (int64_t)periodo) {
        float cota = K / (float)transcurrido;
        if (rpm > cota) rpm = cota;
    }

    return rpm;
}

// ---------------------------------------------------------------------------
// HANDLERS IO7
// ---------------------------------------------------------------------------
void handleUserMeta() {
    if (cfg["meta"].containsKey("pubInterval")) {
        pubInterval = cfg["meta"]["pubInterval"].as<int>();
        if (pubInterval < 200) pubInterval = 200;
    }
    if (cfg["meta"].containsKey("pulsos_rev")) {
        PULSOS_POR_REV = cfg["meta"]["pulsos_rev"].as<uint32_t>();
    }
    if (cfg["meta"].containsKey("flanco")) {
        TIPO_FLANCO = cfg["meta"]["flanco"].as<int>();
    }
    if (cfg["meta"].containsKey("rpm_max")) {
        RPM_MAX = cfg["meta"]["rpm_max"].as<float>();
    }
    if (cfg["meta"].containsKey("rpm_min")) {
        RPM_MIN = cfg["meta"]["rpm_min"].as<float>();
    }
    initSensor();
}

void handleUserCommand(char* topic, JsonDocument* root) {}

// ---------------------------------------------------------------------------
// PUBLICACIÓN DE DATOS MQTT (Llamada desde la tarea de red)
// ---------------------------------------------------------------------------
void publishData() {
    float rpm = calcularRPM();

    StaticJsonDocument<768> root; 
    JsonObject data = root.createNestedObject("d");

    data["rpm"] = roundf(rpm * 10.0f) / 10.0f;   // Velocidad con 1 decimal, 0 = detenido
    
    data["uptime"] = millis() / 1000;           
    data["reconn"] = reconnecciones_wifi;     
    data["heap"]   = ESP.getFreeHeap();       
    data["d18_logic"] = digitalRead(PIN_SENSOR);
    data["wifi_ok"]   = (WiFi.status() == WL_CONNECTED);
    data["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
    data["status"] = "Online";

    serializeJson(root, msgBuffer);

    if (WiFi.status() == WL_CONNECTED && client.connected()) {
        if (client.publish(evtTopic, msgBuffer)) {
            digitalWrite(LED_PIN, HIGH);
            delay(50); 
            digitalWrite(LED_PIN, LOW);
            Serial.printf("TX OK | RPM: %.1f\n", rpm);
        }
    }
}

// ---------------------------------------------------------------------------
// TAREA EXCLUSIVA DEL CORE 0: GESTIÓN DE WIFI Y MQTT (INALTERADA)
// ---------------------------------------------------------------------------
void core0NetworkTask(void * pvParameters) {
    Serial.printf("[CORE 0] Tarea de red iniciada en el núcleo: %d\n", xPortGetCoreID());
    
    for(;;) {
        bool mqtt_ok = (WiFi.status() == WL_CONNECTED) && client.connected();

        if (mqtt_ok) {
            if (!wifiWasConnected) {
                wifiWasConnected = true;
                wifiDownMillis = 0; 
                Serial.println("[WIFI/MQTT] Conexión estable y operativa.");
            }
            client.loop();
        } 
        else {
            if (wifiWasConnected) {
                reconnecciones_wifi++; 
                wifiWasConnected = false;
                wifiDownMillis = millis(); 
                Serial.println("[ALERTA] Enlace MQTT o WiFi perdido. Iniciando temporizador de tolerancia...");
            }

            if (WiFi.status() == WL_CONNECTED) {
                static uint32_t lastTry = 0;
                if (millis() - lastTry > 5000) {
                    iot_connect(); 
                    lastTry = millis();
                }
            }

            if (wifiDownMillis != 0 && (millis() - wifiDownMillis > RESTART_TIMEOUT)) {
                Serial.println("[CRÍTICO] 5 minutos sin reportar datos. Reiniciando radio WiFi...");
                
                WiFi.disconnect(true); 
                vTaskDelay(pdMS_TO_TICKS(100));
                WiFi.mode(WIFI_OFF);   
                vTaskDelay(pdMS_TO_TICKS(100));
                WiFi.mode(WIFI_STA);   
                
                const char* ssid = cfg["ssid"] ? (const char*)cfg["ssid"] : nullptr;
                const char* pass = cfg["w_pw"] ? (const char*)cfg["w_pw"] : nullptr;
                WiFi.begin(ssid, pass); 

                wifiDownMillis = millis(); 
            }
        }

        if (pubInterval > 0 && millis() - lastPublishMillis > (unsigned long)pubInterval) {
            publishData();
            lastPublishMillis = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] Iniciando sistema de medición de RPM por periodo...");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); 

    initDevice();
    userMeta = handleUserMeta;
    userCommand = handleUserCommand;
    handleUserMeta();

    initSensor();

    if (pubInterval <= 0) pubInterval = defaultPubIntervalMs;

    const char* ssid = cfg["ssid"] ? (const char*)cfg["ssid"] : nullptr;
    const char* pass = cfg["w_pw"] ? (const char*)cfg["w_pw"] : nullptr;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    
    Serial.print("Conectando a WiFi inicial...");
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        Serial.print(".");
        timeout++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WIFI] ¡Conectado con éxito al inicio!");
        wifiWasConnected = true;
    } else {
        Serial.println("\n[WIFI] No se pudo conectar al inicio. Iniciando modo offline temporal.");
        wifiWasConnected = false;
        wifiDownMillis = millis(); 
    }

    xTaskCreatePinnedToCore(
        core0NetworkTask,     
        "NetworkTask",        
        8192,                 
        NULL,                 
        1,                    
        &NetworkTaskHandle,   
        0                     
    );
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL (La medición la hace la interrupción; el loop queda libre)
// ---------------------------------------------------------------------------
void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}