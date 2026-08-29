/*
  ============================================================================
  PRUEBA COMBINADA - Funciones 0x0F, 0x19 y 0x16
  ============================================================================
  Pide las tres funciones seguidas en cada ciclo, bien etiquetadas:
    - 0x0F -> respuesta 0x8F (207 bytes): estado en tiempo real, ya decodificado
      en gran parte (voltajes, corrientes, potencias por fase, frecuencia,
      temperatura candidata en offset 98, tiempo de funcionamiento)
    - 0x19 -> respuesta 0x99 (407 bytes): segunda trama de estado, sin
      contadores de energia identificados todavia
    - 0x16 -> respuesta esperada 0x96: no probada aun, candidata a contener
      "ajustes del inversor" (documentado para el X1 Mini) - puede que no
      sea la que buscamos, pero es la siguiente pista razonable a explorar
  ============================================================================
*/

#include <HardwareSerial.h>

#define BAUDRATE 9600
#define RX_PIN 16
#define TX_PIN 17

HardwareSerial InverterSerial(2);

const char* DONGLE_SN = "ZZ00XX11YY"; // Inventado, ya confirmado que el inversor lo acepta igual

void setup() {
  Serial.begin(115200);
  delay(1000);
  InverterSerial.begin(BAUDRATE, SERIAL_8N1, RX_PIN, TX_PIN);
  Serial.println("=== PRUEBA FINAL: ESP32 solo ===");
}

// Checksum: suma de 16 bits de todos los bytes previos, LITTLE-ENDIAN
// (byte bajo primero, byte alto despues) - confirmado con captura real
void calcularChecksumLE(uint8_t* frame, size_t lenSinChecksum, uint8_t* out) {
  uint16_t suma = 0;
  for (size_t i = 0; i < lenSinChecksum; i++) suma += frame[i];
  out[0] = suma & 0xFF;        // byte bajo primero
  out[1] = (suma >> 8) & 0xFF; // byte alto despues
}

void enviarYMostrar(uint8_t* frame, size_t len, const char* etiqueta) {
  Serial.printf("\n--- %s ---\n", etiqueta);
  Serial.print("Enviado: ");
  for (size_t i = 0; i < len; i++) {
    if (frame[i] < 0x10) Serial.print("0");
    Serial.print(frame[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  while (InverterSerial.available()) InverterSerial.read();
  InverterSerial.write(frame, len);

  unsigned long inicio = millis();
  int contador = 0;
  uint8_t respuesta[220];
  Serial.print("Respuesta: ");
  while (millis() - inicio < 2000) {
    if (InverterSerial.available()) {
      uint8_t b = InverterSerial.read();
      if (contador < 220) respuesta[contador] = b;
      if (b < 0x10) Serial.print("0");
      Serial.print(b, HEX);
      Serial.print(" ");
      contador++;
      inicio = millis();
    }
  }
  if (contador == 0) Serial.println("(sin respuesta)");
  else Serial.printf("  [%d bytes]\n", contador);
}

void registrarDongle() {
  uint8_t frame[17];
  frame[0] = 0xAA;
  frame[1] = 0x55;
  frame[2] = 0x11; // 17 = tamaño total del frame
  frame[3] = 0x02; // control
  frame[4] = 0x01; // funcion
  for (int i = 0; i < 10; i++) frame[5 + i] = DONGLE_SN[i];

  calcularChecksumLE(frame, 15, &frame[15]);
  enviarYMostrar(frame, sizeof(frame), "PASO 1: Registro del dongle");
}

void pedirFuncion(uint8_t funcion, const char* etiqueta) {
  uint8_t frame[7] = {0xAA, 0x55, 0x07, 0x01, funcion, 0x00, 0x00};
  calcularChecksumLE(frame, 5, &frame[5]);
  enviarYMostrar(frame, sizeof(frame), etiqueta);
}

void loop() {
  registrarDongle();
  delay(500);

  pedirFuncion(0x0F, "FUNCION 0x0F (estado tiempo real)");
  delay(500);
  pedirFuncion(0x19, "FUNCION 0x19 (segunda trama de estado)");
  delay(500);
  pedirFuncion(0x16, "FUNCION 0x16 (candidata: ajustes/otros contadores)");

  Serial.println("\n====================================================");
  delay(10000);
}
