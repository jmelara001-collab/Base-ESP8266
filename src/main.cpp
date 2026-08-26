#include <Wire.h>
#include <Adafruit_ADS1X15.h>

Adafruit_ADS1115 adsU2; 
Adafruit_ADS1115 adsU4; 

const float FACTOR_CORRIENTE = 50.0; 

// =================================================================
// CAMBIA ESTE NÚMERO ANTES DE SUBIR EL CÓDIGO PARA CADA PRUEBA
// 0 = Normal, 1 = Sobrecarga, 2 = Desbalance, 3 = Falla de Fase
int ETIQUETA_ACTUAL = 3; 
// =================================================================

void setup() {
  Serial.begin(115200);
  Wire.begin(18, 19);

  adsU2.begin(0x48);
  adsU4.begin(0x49);

  adsU2.setGain(GAIN_TWO);
  adsU4.setGain(GAIN_TWO);
  
  adsU2.setDataRate(RATE_ADS1115_860SPS);
  adsU4.setDataRate(RATE_ADS1115_860SPS);
  
  // Imprimir encabezados de las columnas una sola vez
  Serial.println("L1,L2,L3,Desbalance_Pct,Etiqueta");
}

float leerCorrienteRMS(Adafruit_ADS1115 &ads, uint8_t canalDiferencial, uint32_t tiempoMuestreo) {
  uint32_t inicio = millis();
  int contador = 0;
  float sumaCuadrados = 0.0;

  while (millis() - inicio < tiempoMuestreo) {
    int16_t adc = (canalDiferencial == 0) ? ads.readADC_Differential_0_1() : ads.readADC_Differential_2_3();
    float voltios = ads.computeVolts(adc);
    sumaCuadrados += (voltios * voltios);
    contador++;
  }

  if (contador == 0) return 0;
  return sqrt(sumaCuadrados / contador) * FACTOR_CORRIENTE;
}

void loop() {
  // Muestreo de las 3 fases
  float L1 = leerCorrienteRMS(adsU2, 1, 50); 
  float L2 = leerCorrienteRMS(adsU2, 0, 50); 
  float L3 = leerCorrienteRMS(adsU4, 0, 50); 

  // Cálculo del porcentaje de desbalance
  float promedio = (L1 + L2 + L3) / 3.0;
  float maxDesviacion = 0.0;
  
  if (promedio > 0) {
    float devL1 = abs(L1 - promedio);
    float devL2 = abs(L2 - promedio);
    float devL3 = abs(L3 - promedio);
    
    maxDesviacion = devL1;
    if (devL2 > maxDesviacion) maxDesviacion = devL2;
    if (devL3 > maxDesviacion) maxDesviacion = devL3;
  }
  
  float desbalance = (promedio > 0) ? (maxDesviacion / promedio) * 100.0 : 0.0;

  // Imprimir formato CSV
  Serial.print(L1, 2); Serial.print(",");
  Serial.print(L2, 2); Serial.print(",");
  Serial.print(L3, 2); Serial.print(",");
  Serial.print(desbalance, 2); Serial.print(",");
  Serial.println(ETIQUETA_ACTUAL);

  delay(200); // 5 lecturas por segundo para generar datos rápido
}