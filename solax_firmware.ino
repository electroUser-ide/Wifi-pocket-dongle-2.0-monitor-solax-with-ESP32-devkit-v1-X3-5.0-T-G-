/*
  ============================================================================
  MONITOR SOLAR FINAL - Solax X3-5.0-TG (ESP32 DevKit V1)
  ============================================================================
  Protocolo verificado con captura real del inversor (no el documentado
  para el X1 Mini):
    - Peticion datos: control 0x01, funcion 0x0F
    - Respuesta: control 0x01, funcion 0x8F, 207 bytes (200 payload)
    - Checksum: suma de 16 bits de los bytes previos, LITTLE-ENDIAN
    - Registro previo obligatorio: control 0x02, funcion 0x01, payload=10
      bytes de "numero de serie" (puede ser inventado, no se valida)

  Offsets de payload confirmados por busqueda exacta sobre captura real:
    0,2,4   -> Voltaje red R/S/T   (uint16 LE, 0.1V)
    6,8,10  -> Corriente red R/S/T (uint16 LE, 0.1A)
    12,14,16-> Potencia red R/S/T  (uint16 LE, 1W)
    30,32,34-> Frecuencia R/S/T    (uint16 LE, 0.01Hz)
    38      -> Energia total generada (uint32 LE, 0.1kWh)
    94-95   -> Potencia nominal del inversor (constante, 5000W)
    98      -> Temperatura inversor   (1 byte, C)
    144     -> Temperatura secundaria (1 byte, C, segunda sonda)
    152     -> Energia exportada total (uint32 LE, 0.01kWh)

  Nota: se localizaron tambien offset 148 (potencia neta de red) y offset
  156 (energia importada total), pero se dejaron de usar a peticion del
  usuario (no se quieren esos campos en la app ni en el firmware).

  Campo derivado (no viene directo del inversor, se calcula aqui):
    - Potencia inversor/PV = potenciaR + potenciaS + potenciaT (confirmado
      contra pantalla: 1528W calculado vs "1500-1600W" real mostrado)

  ----------------------------------------------------------------------------
  RESILIENCIA A CORTES DE LUZ
  ----------------------------------------------------------------------------
  El inversor es on-grid: sin tension de red no funciona, y como el ESP32 se
  alimenta desde su puerto USB, un corte apaga a los dos a la vez. No hay
  forma de "aguantar" el corte con este hardware - la estrategia es que, al
  volver la luz y reiniciar el ESP32, NO se pierda el progreso del dia:

    - Los contadores de partida del dia (energia generada y exportada a las
      00:00) se guardan en la memoria NVS del ESP32 (flash), no en RAM.
      Sobreviven a reinicios sin perderse.
    - Se guarda tambien la fecha a la que corresponden esos contadores. Si al
      arrancar la fecha guardada ya es "hoy", NO se vuelven a fijar (evita
      que un reinicio a media tarde reinicie por error el contador del dia).
    - Las estadisticas de temperatura (max/media/min) tambien se van
      persistiendo en NVS con cada muestra, no solo en RAM, con el mismo
      mecanismo de fecha para saber si son "de hoy" o hay que reiniciarlas.
    - El envio del correo diario tambien se controla por fecha guardada en
      NVS (no solo una variable en RAM), evitando duplicados si el ESP32 se
      reinicia el mismo dia despues de enviar, y permitiendo un "envio de
      recuperacion": si el ESP32 arranca y ya son las 22:00 o mas tarde y
      el informe de hoy no se ha mandado, lo manda enseguida en vez de
      esperar a que el reloj marque exactamente las 22:00.

  Limitacion asumida: si el corte de luz coincide justo con la medianoche,
  el valor de "inicio de dia" que se toma es el primero disponible al
  arrancar, no el de las 00:00 exactas. Como no hay generacion solar de
  madrugada, el error introducido es minimo o nulo en la practica.
  ============================================================================
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <Preferences.h>
#include <ESP_Mail_Client.h>
#include <HardwareSerial.h>

// ============================================================================
// CONFIGURACION DEL USUARIO - rellenar antes de cargar
// ============================================================================
const char* WIFI_SSID     = "TU_SSID";
const char* WIFI_PASSWORD = "TU_PASSWORD";
const char* NOMBRE_PLANTA = "Mi Instalacion Solar";

#define SMTP_HOST         "smtp.gmail.com"
#define SMTP_PORT         465
#define AUTHOR_EMAIL      "tu_correo@gmail.com"
#define AUTHOR_PASSWORD   "tu_password_de_aplicacion"
#define RECIPIENT_EMAIL   "destino@correo.com"

const char* NTP_SERVER          = "pool.ntp.org";
const long  GMT_OFFSET_SEC      = 3600;
const int   DAYLIGHT_OFFSET_SEC = 3600;

// --- Firebase Realtime Database (para ver los datos fuera de casa) ---
// URL de tu proyecto, tipo: https://tu-proyecto-default-rtdb.europe-west1.firebasedatabase.app
const char* FIREBASE_HOST = "https://TU-PROYECTO-default-rtdb.REGION.firebasedatabase.app";
// Secreto/token de la base de datos (ver instrucciones al final de este archivo)
const char* FIREBASE_AUTH = "TU_DATABASE_SECRET";
const unsigned long INTERVALO_SUBIDA_MS = 5UL * 1000UL; // cada 5s

#define INVERTER_RX_PIN 16
#define INVERTER_TX_PIN 17
#define INVERTER_BAUD   9600
HardwareSerial InverterSerial(2);

const char* DONGLE_SN = "ESP32SOLAX"; // Inventado, 10 caracteres, confirmado que vale

const int HORA_ENVIO   = 22;
const unsigned long INTERVALO_LECTURA_MS = 5UL * 1000UL;        // cada 5s, para datos en vivo
const unsigned long INTERVALO_MUESTREO_TEMP_MS = 10UL * 60UL * 1000UL; // cada 10 min, solo para estadisticas de temp (evita desgastar la NVS)

// ============================================================================
// ESTADO PERSISTENTE (NVS)
// ============================================================================
Preferences prefs;

float g_eInicioDia = 0, g_expInicioDia = 0;
int   g_diaGuardado = 0;     // formato yyyymmdd

float g_eInicioMes = 0, g_expInicioMes = 0;
int   g_mesGuardado = 0;     // formato yyyymm

int   g_ultimoEnvioFecha = 0; // yyyymmdd

float g_tempMax = -1000, g_tempMin = 1000, g_tempSum = 0;
int   g_tempSamples = 0;
int   g_tempDiaGuardado = 0;

// ============================================================================
// LECTURAS EN VIVO (RAM, se refrescan cada muestreo)
// ============================================================================
float g_voltR=0, g_voltS=0, g_voltT=0;
float g_corrR=0, g_corrS=0, g_corrT=0;
float g_potR=0, g_potS=0, g_potT=0;
float g_frecR=0, g_frecS=0, g_frecT=0;
float g_eTotal=0;      // kWh
float g_expTotal=0;    // kWh
float g_tempActual=0;  // C
float g_tempSecundaria=0; // C, offset 144 - segunda sonda, menos verificado que g_tempActual
float g_potenciaInversor=0; // W, = suma potenciaR+S+T. Confirmado contra pantalla (1528W calculado vs "1500-1600W" real)
const float POTENCIA_NOMINAL_INVERSOR = 5000.0f; // W, constante confirmada en offset 94-95

unsigned long g_ultimoMuestreo = 0;
unsigned long g_ultimaMuestraTemp = 0;
unsigned long g_ultimaSubidaFirebase = 0;

// ============================================================================
// UTILIDADES DE FECHA
// ============================================================================
int fechaYYYYMMDD(struct tm &t) {
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}
int mesYYYYMM(struct tm &t) {
  return (t.tm_year + 1900) * 100 + (t.tm_mon + 1);
}

// ============================================================================
// CORRECCION UNICA DE ARRANQUE - inicio de mes
// ============================================================================
// El firmware anterior (version de prueba) fijo el "inicio de mes" el
// 27/08/2026 usando el eTotal de esa misma tarde en vez del real de
// principios de agosto, dejando la produccion mensual del informe por
// debajo de la real durante el resto del mes.
//
// Correccion conocida: el 26/08/2026, con eTotal=11096.1 kWh, la pantalla
// del inversor marcaba "Generacion mensual: 203.5 kWh". Eso da un inicio
// de mes real de 11096.1 - 203.5 = 10892.6 kWh.
//
// IMPORTANTE: esta correccion solo se aplica al contador de GENERACION.
// Para exportacion mensual no tenemos un dato de referencia equivalente,
// asi que su contador de inicio de mes se deja que lo fije la logica
// normal de "nuevo mes" (con la misma pequeña imprecision que tuvo la
// generacion diaria el primer dia: se autocorrige sola el 1 de
// septiembre). Por eso esta funcion NO toca g_mesGuardado hasta DESPUES
// de que la logica normal ya haya podido fijar exportacion, y solo pisa
// el valor de g_eInicioMes al final.
const float CORRECCION_E_INICIO_MES = 10892.6f;

void aplicarCorreccionDeGeneracionMensualSiToca() {
  if (prefs.getBool("correccion1", false)) return; // ya aplicada antes, no repetir
  if (g_mesGuardado == 0) return; // esperar a que la logica normal fije el mes primero

  g_eInicioMes = CORRECCION_E_INICIO_MES;
  prefs.putFloat("eInicioMes", g_eInicioMes);
  prefs.putBool("correccion1", true);
  Serial.println("Correccion de inicio de mes (generacion) aplicada (una sola vez).");
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  InverterSerial.begin(INVERTER_BAUD, SERIAL_8N1, INVERTER_RX_PIN, INVERTER_TX_PIN);

  prefs.begin("solax_mon", false);
  g_eInicioDia      = prefs.getFloat("eInicioDia", 0);
  g_expInicioDia    = prefs.getFloat("expInicioDia", 0);
  g_diaGuardado     = prefs.getInt("diaGuardado", 0);
  g_eInicioMes      = prefs.getFloat("eInicioMes", 0);
  g_expInicioMes    = prefs.getFloat("expInicioMes", 0);
  g_mesGuardado     = prefs.getInt("mesGuardado", 0);
  g_ultimoEnvioFecha= prefs.getInt("ultimoEnvio", 0);
  g_tempMax         = prefs.getFloat("tempMax", -1000);
  g_tempMin         = prefs.getFloat("tempMin", 1000);
  g_tempSum         = prefs.getFloat("tempSum", 0);
  g_tempSamples     = prefs.getInt("tempSamples", 0);
  g_tempDiaGuardado = prefs.getInt("tempDia", 0);

  conectarWiFi();
  configurarHora();

  Serial.println("Setup completo.");
}

// ============================================================================
// LOOP PRINCIPAL
// ============================================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    conectarWiFi();
  }

  if (millis() - g_ultimoMuestreo >= INTERVALO_LECTURA_MS || g_ultimoMuestreo == 0) {
    g_ultimoMuestreo = millis();
    actualizarDatosInversor();

    if (millis() - g_ultimaMuestraTemp >= INTERVALO_MUESTREO_TEMP_MS || g_ultimaMuestraTemp == 0) {
      g_ultimaMuestraTemp = millis();
      actualizarEstadisticasTemperatura();
    }
  }

  chequearTareasDiariasYMensuales();

  if (millis() - g_ultimaSubidaFirebase >= INTERVALO_SUBIDA_MS || g_ultimaSubidaFirebase == 0) {
    g_ultimaSubidaFirebase = millis();
    subirDatosFirebase();
  }

  delay(1000);
}

// ============================================================================
// WIFI Y HORA
// ============================================================================
void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Conectando a WiFi %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nNo se pudo conectar al WiFi, se reintentara en el loop.");
  }
}

void configurarHora() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  int intentos = 0;
  while (!getLocalTime(&timeinfo) && intentos < 10) {
    Serial.println("Esperando hora NTP...");
    delay(1000);
    intentos++;
  }
}

// ============================================================================
// PROTOCOLO SOLAX (registro + peticion de datos + parseo)
// ============================================================================
void calcularChecksumLE(uint8_t* frame, size_t lenSinChecksum, uint8_t* out) {
  uint16_t suma = 0;
  for (size_t i = 0; i < lenSinChecksum; i++) suma += frame[i];
  out[0] = suma & 0xFF;
  out[1] = (suma >> 8) & 0xFF;
}

bool enviarYLeer(uint8_t* frame, size_t len, uint8_t* respuesta, size_t maxLen, size_t &recibidos) {
  while (InverterSerial.available()) InverterSerial.read();
  InverterSerial.write(frame, len);

  unsigned long inicio = millis();
  recibidos = 0;
  while (millis() - inicio < 2000) {
    if (InverterSerial.available()) {
      uint8_t b = InverterSerial.read();
      if (recibidos < maxLen) respuesta[recibidos] = b;
      recibidos++;
      inicio = millis();
    }
  }
  return recibidos > 0;
}

bool registrarDongle() {
  uint8_t frame[17];
  frame[0] = 0xAA; frame[1] = 0x55; frame[2] = 0x11;
  frame[3] = 0x02; frame[4] = 0x01;
  for (int i = 0; i < 10; i++) frame[5 + i] = DONGLE_SN[i];
  calcularChecksumLE(frame, 15, &frame[15]);

  uint8_t resp[32];
  size_t recibidos;
  bool ok = enviarYLeer(frame, sizeof(frame), resp, sizeof(resp), recibidos);
  return ok && recibidos >= 5;
}

bool pedirYParsearDatos() {
  uint8_t frame[7] = {0xAA, 0x55, 0x07, 0x01, 0x0F, 0x00, 0x00};
  calcularChecksumLE(frame, 5, &frame[5]);

  uint8_t resp[220];
  size_t recibidos;
  if (!enviarYLeer(frame, sizeof(frame), resp, sizeof(resp), recibidos)) return false;
  if (recibidos < 160 || resp[3] != 0x01 || resp[4] != 0x8F) return false; // respuesta invalida/corta

  uint8_t* p = &resp[5]; // inicio del payload

  auto u16 = [&](int off) -> uint16_t { return p[off] | (p[off+1] << 8); };
  auto u32 = [&](int off) -> uint32_t {
    return (uint32_t)p[off] | ((uint32_t)p[off+1] << 8) | ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
  };

  g_voltR = u16(0) * 0.1f;  g_voltS = u16(2) * 0.1f;  g_voltT = u16(4) * 0.1f;
  g_corrR = u16(6) * 0.1f;  g_corrS = u16(8) * 0.1f;  g_corrT = u16(10) * 0.1f;
  g_potR  = u16(12);        g_potS  = u16(14);        g_potT  = u16(16);
  g_frecR = u16(30) * 0.01f; g_frecS = u16(32) * 0.01f; g_frecT = u16(34) * 0.01f;
  g_eTotal   = u32(38) * 0.1f;
  g_expTotal = u32(152) * 0.01f;
  g_tempActual = (float)p[98];
  g_tempSecundaria = (float)p[144];
  g_potenciaInversor = g_potR + g_potS + g_potT;

  return true;
}

void actualizarDatosInversor() {
  if (!registrarDongle()) {
    Serial.println("No se pudo registrar el dongle (sin respuesta del inversor).");
    return;
  }
  delay(200);
  if (!pedirYParsearDatos()) {
    Serial.println("No se pudo leer la trama de datos del inversor.");
    return;
  }

  Serial.printf("Lectura OK -> eTotal=%.1f kWh | expTotal=%.2f kWh | Temp=%.0f C\n",
                g_eTotal, g_expTotal, g_tempActual);
}

// ============================================================================
// ESTADISTICAS DE TEMPERATURA (persistentes)
// ============================================================================
void actualizarEstadisticasTemperatura() {
  // El inversor deja de actualizar el registro de temperatura cuando esta
  // inactivo (de noche), y devuelve 0 - pero no queremos usar la hora del
  // reloj para filtrarlo, porque un 0C real a mediodia con el inversor
  // activo si seria un dato valido. Usamos el voltaje de red como senal de
  // "inversor realmente activo": si esta a 0, el inversor no esta
  // convirtiendo y la lectura de temperatura no es fiable, se descarta.
  bool inversorActivo = (g_voltR > 50.0f);
  if (!inversorActivo) {
    Serial.println("Inversor inactivo (voltaje de red ~0) - se descarta esta lectura de temperatura.");
    return;
  }

  // Segundo filtro: justo al arrancar, el voltaje ya puede ser valido pero
  // el registro de temperatura todavia no se ha actualizado y devuelve 0.
  // Un 0C exacto es un valor sospechosamente redondo con el inversor ya
  // activo (la variacion real de un sensor rara vez cae justo en 0), asi
  // que tambien se descarta esta muestra concreta sin tocar estadisticas.
  if (g_tempActual == 0.0f) {
    Serial.println("Temperatura leida como 0 con inversor activo (residuo de arranque) - se descarta esta lectura.");
    return;
  }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  int hoy = fechaYYYYMMDD(timeinfo);

  if (g_tempDiaGuardado != hoy) {
    // Nuevo dia (o primer arranque): reiniciar estadisticas
    g_tempMax = g_tempActual;
    g_tempMin = g_tempActual;
    g_tempSum = g_tempActual;
    g_tempSamples = 1;
    g_tempDiaGuardado = hoy;
  } else {
    if (g_tempActual > g_tempMax) g_tempMax = g_tempActual;
    if (g_tempActual < g_tempMin) g_tempMin = g_tempActual;
    g_tempSum += g_tempActual;
    g_tempSamples++;
  }

  // Persistir en cada muestra: si hay un corte de luz, no se pierde el progreso
  prefs.putFloat("tempMax", g_tempMax);
  prefs.putFloat("tempMin", g_tempMin);
  prefs.putFloat("tempSum", g_tempSum);
  prefs.putInt("tempSamples", g_tempSamples);
  prefs.putInt("tempDia", g_tempDiaGuardado);
}

// ============================================================================
// GESTION DE DIA / MES / ENVIO (con resiliencia a reinicios)
// ============================================================================
void chequearTareasDiariasYMensuales() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int hoy = fechaYYYYMMDD(timeinfo);
  int mesActual = mesYYYYMM(timeinfo);

  // --- Nuevo dia: fijar contadores de inicio de dia (solo si no estaba ya fijado hoy) ---
  if (g_diaGuardado != hoy && g_eTotal > 0) {
    g_eInicioDia = g_eTotal;
    g_expInicioDia = g_expTotal;
    g_diaGuardado = hoy;
    prefs.putFloat("eInicioDia", g_eInicioDia);
    prefs.putFloat("expInicioDia", g_expInicioDia);
    prefs.putInt("diaGuardado", g_diaGuardado);
    Serial.println("Nuevo dia detectado. Contadores de inicio de dia guardados.");
  }

  // --- Nuevo mes: fijar contador de inicio de mes (solo si no estaba ya fijado este mes) ---
  if (g_mesGuardado != mesActual && g_eTotal > 0) {
    g_eInicioMes = g_eTotal;
    g_expInicioMes = g_expTotal;
    g_mesGuardado = mesActual;
    prefs.putFloat("eInicioMes", g_eInicioMes);
    prefs.putFloat("expInicioMes", g_expInicioMes);
    prefs.putInt("mesGuardado", g_mesGuardado);
    Serial.println("Nuevo mes detectado. Contador de inicio de mes guardado.");
  }

  aplicarCorreccionDeGeneracionMensualSiToca();

  // --- Envio diario a partir de HORA_ENVIO, con recuperacion si se arranca tarde ---
  if (timeinfo.tm_hour >= HORA_ENVIO && g_ultimoEnvioFecha != hoy && g_diaGuardado == hoy) {
    enviarInformeDiario(timeinfo, hoy);
  }
}

// ============================================================================
// ENVIO DE CORREO
// ============================================================================
// Da el formato adecuado segun la magnitud: MWh si son >=1000 kWh, si no kWh
String formatearEnergia(float kwh) {
  if (kwh >= 1000.0f) {
    return String(kwh / 1000.0f, 2) + " MWh";
  }
  return String(kwh, 1) + " kWh";
}

void enviarInformeDiario(struct tm &timeinfo, int hoy) {
  char fechaStr[11];
  sprintf(fechaStr, "%02d/%02d/%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);

  float eHoy = g_eTotal - g_eInicioDia;
  float eMes = g_eTotal - g_eInicioMes;
  float tempMedia = (g_tempSamples > 0) ? (g_tempSum / g_tempSamples) : 0;

  String cuerpo = "";
  cuerpo += "Informe diario de planta\n";
  cuerpo += "-------------------------\n";
  cuerpo += "Fecha: " + String(fechaStr) + "\n";
  cuerpo += "Instalacion: " + String(NOMBRE_PLANTA) + "\n\n";
  cuerpo += "Produccion diaria: " + formatearEnergia(eHoy) + "\n";
  cuerpo += "Produccion mensual: " + formatearEnergia(eMes) + "\n";
  cuerpo += "Produccion total (desde instalacion): " + formatearEnergia(g_eTotal) + "\n\n";
  cuerpo += "Temperatura inversor:\n";
  cuerpo += "  Maxima: " + String(g_tempMax, 0) + " C\n";
  cuerpo += "  Media:  " + String(tempMedia, 1) + " C\n";
  cuerpo += "  Minima: " + String(g_tempMin, 0) + " C\n";

  SMTPSession smtp;
  Session_Config config;
  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = AUTHOR_EMAIL;
  config.login.password = AUTHOR_PASSWORD;
  config.login.user_domain = "";

  SMTP_Message message;
  message.sender.name = NOMBRE_PLANTA;
  message.sender.email = AUTHOR_EMAIL;
  message.subject = "Informe diario de planta - " + String(fechaStr);
  message.addRecipient("Destinatario", RECIPIENT_EMAIL);
  message.text.content = cuerpo.c_str();

  bool enviado = false;
  if (smtp.connect(&config)) {
    enviado = MailClient.sendMail(&smtp, &message);
    smtp.closeSession();
  }

  if (enviado) {
    g_ultimoEnvioFecha = hoy;
    prefs.putInt("ultimoEnvio", g_ultimoEnvioFecha); // persistido: evita duplicados tras reinicio
    Serial.println("Correo enviado y marcado como enviado hoy.");
  } else {
    Serial.println("Fallo al enviar el correo. Se reintentara en el proximo ciclo.");
    // No se marca como enviado: el propio chequeo del loop lo reintentara
  }
}

// ============================================================================
// SUBIDA A FIREBASE (para ver los datos fuera de casa, con datos moviles)
// ============================================================================
void subirDatosFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;

  struct tm timeinfo;
  char fechaStr[11] = "";
  if (getLocalTime(&timeinfo)) {
    sprintf(fechaStr, "%02d/%02d/%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
  }

  float eHoy   = g_eTotal - g_eInicioDia;
  float expHoy = g_expTotal - g_expInicioDia;
  float eMes   = g_eTotal - g_eInicioMes;
  float expMes = g_expTotal - g_expInicioMes;
  float tempMedia = (g_tempSamples > 0) ? (g_tempSum / g_tempSamples) : 0;

  String json = "{";
  json += "\"nombrePlanta\":\"" + String(NOMBRE_PLANTA) + "\",";
  json += "\"fecha\":\"" + String(fechaStr) + "\",";
  json += "\"timestamp\":" + String((unsigned long)time(nullptr)) + ",";

  json += "\"potenciaFotovoltaica\":" + String(g_potenciaInversor, 0) + ",";
  json += "\"potenciaNominalInversor\":" + String(POTENCIA_NOMINAL_INVERSOR, 0) + ",";

  json += "\"voltajeR\":" + String(g_voltR, 1) + ",";
  json += "\"voltajeS\":" + String(g_voltS, 1) + ",";
  json += "\"voltajeT\":" + String(g_voltT, 1) + ",";
  json += "\"corrienteR\":" + String(g_corrR, 1) + ",";
  json += "\"corrienteS\":" + String(g_corrS, 1) + ",";
  json += "\"corrienteT\":" + String(g_corrT, 1) + ",";
  json += "\"potenciaR\":" + String(g_potR, 0) + ",";
  json += "\"potenciaS\":" + String(g_potS, 0) + ",";
  json += "\"potenciaT\":" + String(g_potT, 0) + ",";
  json += "\"frecuenciaR\":" + String(g_frecR, 2) + ",";
  json += "\"frecuenciaS\":" + String(g_frecS, 2) + ",";
  json += "\"frecuenciaT\":" + String(g_frecT, 2) + ",";

  json += "\"generacionHoy\":" + String(eHoy, 2) + ",";
  json += "\"generacionMensual\":" + String(eMes, 1) + ",";
  json += "\"generacionTotal\":" + String(g_eTotal, 1) + ",";

  json += "\"exportacionHoy\":" + String(expHoy, 2) + ",";
  json += "\"exportacionMensual\":" + String(expMes, 2) + ",";
  json += "\"exportacionTotal\":" + String(g_expTotal, 2) + ",";

  json += "\"temperaturaMaxima\":" + String(g_tempMax, 0) + ",";
  json += "\"temperaturaMedia\":" + String(tempMedia, 1) + ",";
  json += "\"temperaturaMinima\":" + String(g_tempMin, 0) + ",";
  json += "\"temperaturaSecundaria\":" + String(g_tempSecundaria, 0);

  json += "}";

  HTTPClient http;
  String url = String(FIREBASE_HOST) + "/datos.json?auth=" + String(FIREBASE_AUTH);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int codigo = http.PUT(json);
  if (codigo > 0) {
    Serial.printf("Firebase subido OK (codigo %d)\n", codigo);
  } else {
    Serial.printf("Fallo al subir a Firebase: %s\n", http.errorToString(codigo).c_str());
  }
  http.end();
}

/*
  ============================================================================
  COMO CREAR Y CONFIGURAR EL FIREBASE REALTIME DATABASE (una sola vez)
  ============================================================================
  1. Ve a https://console.firebase.google.com y crea un proyecto nuevo
     (gratis, no hace falta tarjeta para este uso).
  2. En el menu lateral, entra en "Realtime Database" -> "Crear base de datos".
     Elige una region cercana (ej. europe-west1).
  3. En "Reglas", de momento para simplificar pon reglas basadas en un
     secreto (modo "Locked mode" con Database Secret): en el proyecto,
     Configuracion del proyecto (rueda dentada) -> "Cuentas de servicio" ->
     pestaña "Secretos de la base de datos en tiempo real" (Database Secrets,
     seccion antigua pero funcional) -> copia el secreto generado.
  4. Copia la URL de tu base de datos (aparece arriba en la seccion Realtime
     Database, algo como https://tu-proyecto-default-rtdb.europe-west1.
     firebasedatabase.app) y pegala en FIREBASE_HOST arriba en este archivo.
  5. Pega el secreto copiado en FIREBASE_AUTH arriba en este archivo.
  6. La app Android leera de la misma base de datos con el SDK oficial de
     Firebase (google-services.json que descargues del mismo proyecto),
     sin necesitar el secreto - eso se explica en el proyecto de Android.
  ============================================================================
*/
