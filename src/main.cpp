#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

// Instancia del ADC1
Adafruit_ADS1115 ads;

/* FACTOR DE CALIBRACIÓN (Obtenido en LTspice):
  Entrada Real (10V) / Voltaje en A0 (1.93275V) = 5.1739
*/
const float FACTOR_VOLTAJE = 5.1739;

void setup() {
  Serial.begin(115200);
  
  // En el ESP32-S, Wire usa por defecto GPIO 21 (SDA) y 22 (SCL)
  Wire.begin(21, 22);

  Serial.println("Iniciando ADS1115...");
  
  if (!ads.begin()) {
    Serial.println("¡Error! No se encontró el ADS1115. Revisa conexiones y GND.");
    while (1);
  }

  /*
    CONFIGURACIÓN DE GANANCIA (PGA):
    GAIN_TWO: Rango +/- 2.048V (1 bit = 0.0625mV)
    Como nuestra simulación dio 1.93V, este es el rango perfecto.
  */
  ads.setGain(GAIN_TWO); 

  Serial.println("ADS1115 Configurado. Leyendo 0-10V...");
}

void loop() {
  int16_t adc0;
  float volts_ads;
  float volts_industriales;

  // Leer canal A0 (donde conectaste tu circuito de 0-10V)
  adc0 = ads.readADC_SingleEnded(0);
  
  // Voltaje que llega físicamente al pin del ADS (debería ser ~1.93V cuando hay 10V)
  volts_ads = ads.computeVolts(adc0);

  // Aplicar factor para recuperar los 0-10V originales
  volts_industriales = volts_ads * FACTOR_VOLTAJE;

  // Salida al Monitor Serial
  Serial.print("Voltaje en ADS: ");
  Serial.print(volts_ads, 4);
  Serial.print("V | Voltaje Real Sensor: ");
  Serial.print(volts_industriales, 2);
  Serial.println("V");

  delay(1000); // Leer cada segundo
}