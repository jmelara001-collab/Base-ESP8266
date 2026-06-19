#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>

const int PIN_RELE = D7; 

typedef struct struct_message {
    bool activarBomba;   
    bool activarLampara; 
} struct_message;

struct_message datosRecibidos;
String miMac = "";

void OnDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
  digitalWrite(LED_BUILTIN, LOW); // Testigo de recepción de radio
  
  memcpy(&datosRecibidos, incomingData, sizeof(datosRecibidos));
  
  // El receptor identifica automáticamente su rol basándose en su dirección MAC
  if (miMac == "84:CC:A8:A6:FF:8F") {
    // --- LÓGICA DE CONTROL DE LA BOMBA ---
    if (datosRecibidos.activarBomba == true) {
      digitalWrite(PIN_RELE, LOW);   // 0 = ENCIENDE FÍSICAMENTE EL RELÉ DE LA BOMBA
      Serial.println("\n[BOMBA] -> ORDEN RECIBIDA: ENCENDER RELÉ (LOW)");
    } else {
      digitalWrite(PIN_RELE, HIGH);  // 1 = APAGA FÍSICAMENTE EL RELÉ DE LA BOMBA
      Serial.println("\n[BOMBA] -> ORDEN RECIBIDA: APAGAR RELÉ (HIGH)");
    }
  } 
  else if (miMac == "84:0D:8E:AF:50:23") {
    // --- LÓGICA DE CONTROL DE LA LÁMPARA ---
    if (datosRecibidos.activarLampara == true) {
      digitalWrite(PIN_RELE, LOW);   // 0 = ENCIENDE FÍSICAMENTE EL RELÉ DE LA LÁMPARA
      Serial.println("\n[LÁMPARA] -> ORDEN RECIBIDA: ENCENDER ALARMA (LOW)");
    } else {
      digitalWrite(PIN_RELE, HIGH);  // 1 = APAGA FÍSICAMENTE EL RELÉ DE LA LÁMPARA
      Serial.println("\n[LÁMPARA] -> ORDEN RECIBIDA: APAGAR ALARMA (HIGH)");
    }
  }

  delay(50); 
  digitalWrite(LED_BUILTIN, HIGH); 
}
 
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- RECEPTOR REASIGNADO INICIADO ---");
  
  miMac = WiFi.macAddress();
  Serial.print("MI DIRECCION MAC ES: ");
  Serial.println(miMac);
  Serial.println("----------------------------------------");

  // SEGURIDAD EN EL ARRANQUE: Al encender la caja, ambos relés inician obligatoriamente
  // en HIGH (1 = Apagado físico) para evitar cualquier activación en falso.
  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, HIGH); 
  
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); 

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != 0) return;
  
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(OnDataRecv);
}
 
void loop() {
  // Loop libre
}