#include <WiFi.h>

// Reemplaza con tus credenciales de red
const char* ssid = "TU_RED_WIFI";
const char* password = "TU_CONTRASENA";

void setup() {
  // Iniciamos la comunicación serie a 115200 baudios
  Serial.begin(115200);
  delay(1000); // Pequeña pausa para que el monitor serie se inicie

  Serial.println("\n--- Iniciando ESP32 ---");
  Serial.print("Conectando a la red: ");
  Serial.println(ssid);

  // Iniciamos la conexión Wi-Fi en modo Estación (Cliente)
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // Esperamos a que se establezca la conexión
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n\n¡Conexión Wi-Fi exitosa!");
  Serial.println("-----------------------------------");
  
  // 1. Obtener e imprimir la Dirección MAC
  Serial.print("Dirección MAC física: ");
  Serial.println(WiFi.macAddress());

  // 2. Obtener e imprimir la Dirección IP Local
  Serial.print("Dirección IP asignada: ");
  Serial.println(WiFi.localIP());
  Serial.println("-----------------------------------");
}

void loop() {
  // Dejamos el loop vacío ya que solo queremos leer los datos una vez al arrancar
}