#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>

// DIRECCIONES MAC REALES DE TUS DOS RECEPTORES
uint8_t macBomba[]   = {0x84, 0xCC, 0xA8, 0xA6, 0xFF, 0x8F};
uint8_t macLampara[] = {0x84, 0x0D, 0x8E, 0xAF, 0x50, 0x23};

// =========================================================================
// ⚠️ PARÁMETROS CONFIGURADOS:
// =========================================================================
const float LIMITE_VOLTAJE = 0.90;           // Límite fijado en 0.9V
const unsigned long TIEMPO_ESPERA = 300000;   // 5 minuto en milisegundos (300,000 ms)
// =========================================================================

typedef struct struct_message {
    bool activarBomba;   // true = Orden de activar relé, false = Orden de apagar relé
    bool activarLampara; // true = Orden de activar relé, false = Orden de apagar relé
} struct_message;

struct_message miData;

unsigned long tiempoInicioBajo = 0;
bool bajoLimiteAnterior = false;
bool disparoRealizado = false; // Nueva bandera para asegurar un ÚNICO pulso de corte

void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  Serial.print(" -> Enviado a [");
  for (int i = 0; i < 6; i++) {
    Serial.print(mac_addr[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println(sendStatus == 0 ? "]: ÉXITO" : "]: FALLO");
}
 
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- EMISOR DE PROTECCIÓN POR PULSO INICIADO ---");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (esp_now_init() != 0) {
    Serial.println("Error inicializando ESP-NOW");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(OnDataSent);
  
  esp_now_add_peer(macBomba, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
  esp_now_add_peer(macLampara, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
}
 
void loop() {
  int lecturaRaw = analogRead(A0);
  float voltaje = ((float)lecturaRaw * 3.3) / 1023.0;

  Serial.print("\nVoltaje A0: ");
  Serial.print(voltaje);
  Serial.print(" V");

  // EVALUAMOS SI EL VOLTAJE CAE POR DEBAJO DE 0.9V
  if (voltaje < LIMITE_VOLTAJE) {
    
    // ETAPA 1: La lámpara se enciende INMEDIATAMENTE y se mantiene encendida
    miData.activarLampara = true; 
    Serial.print(" -> [ALARMA: LÁMPARA ENCENDIDA]");

    // ETAPA 2: Control del temporizador de falla para la bomba
    if (!bajoLimiteAnterior) {
      tiempoInicioBajo = millis();
      bajoLimiteAnterior = true;
      disparoRealizado = false;    // Reseteamos el disparo para este nuevo ciclo de falla
      miData.activarBomba = false; // Relé de bomba apagado al inicio de la falla
      Serial.print(" Iniciando conteo de 1 min para el corte...");
    } else {
      unsigned long tiempoTranscurrido = millis() - tiempoInicioBajo;
      Serial.print(" Tiempo Falla: ");
      Serial.print(tiempoTranscurrido / 1000);
      Serial.print("s / 60s");
      
      if (tiempoTranscurrido >= TIEMPO_ESPERA) {
        if (!disparoRealizado) {
          // --- ¡MOMENTO DEL DISPARO DEL PULSO! ---
          miData.activarBomba = true;  // Envía el pulso de corte (Relé receptor va a LOW)
          disparoRealizado = true;     // Marcamos que ya se ejecutó el pulso
          Serial.print(" -> [¡PULSO DE CORTE ENVIADO!]");
        } else {
          // Si ya pasó el tiempo y ya se envió el pulso, dejamos de mandar la orden (vuelve a false)
          miData.activarBomba = false; 
          Serial.print(" -> [Corte completado. Esperando recuperación de flujo]");
        }
      } else {
        miData.activarBomba = false; // Sigue esperando a cumplir el minuto
      }
    }
  } 
  else {
    // ESTADO DE REPOSO O RECUPERACIÓN: Si el voltaje regresa a >= 0.9V, todo se normaliza
    bajoLimiteAnterior = false;
    disparoRealizado = false;
    miData.activarBomba = false;   
    miData.activarLampara = false; 
    Serial.print(" -> Estado normal estable (Todo en reposo).");
  }

  // Testigo visual de envío y transmisión por radio
  digitalWrite(LED_BUILTIN, LOW); 
  esp_now_send(macBomba, (uint8_t *) &miData, sizeof(miData));
  delay(10); 
  esp_now_send(macLampara, (uint8_t *) &miData, sizeof(miData));
  
  delay(100);                      
  digitalWrite(LED_BUILTIN, HIGH); 
  
  delay(1390); // Muestreo cada 1.5s aprox
}