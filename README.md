# Wifi-pocket-dongle-2.0-monitor-solax-with-ESP32-devkit-v1-X3-5.0-T-G-(V.1.0)
Firmware de lectura de datos por el usb del inversor SOLAX X3-5.0-TG, envio a base de datos FireBase y envio a app Android. PROYECTO ELABORADO 100% IA, FUNCIONAL (V.1.0)

REQUISITOS:
-Conexión wifi cercana
-Correo Gmail y key para terceros Gmail (necesaria la 2FA)
-Cuenta Google Firebase (extraer key secreta y URL de la base de datos.
-Esp32 DevKitV1 y IDE Arduino (a la hora de subir el skecht seleccionar Herramientas>Esquema de particiones>Minimal SPIFFS 1.9MB APP con OTA)(Harán falta las librerías correspondientes)
-Cable USB tipo A cortado con sus cables (rojo, negro, verde y blanco)

INCLUYE:
-App .apk y .zip para compilar en Android Studio
-Firmware de lectura de datos
-Firmware sniffer para lectura de trama hexadecimal triple función (ignorar función 0x16)

FUNCIONAMIENTO DEL SISTEMA:
1-Esta todo en Español
2-El puerto del inversor solar no es capar de alimentar por si solo al esp32 con el skecht, necesita alimentación externa.

Se ha hecho una ingeniería inversa de lectura de trama de datos con un sistema sniffer (conexión en paralelo esp32 + wifi pocket dongle 2.0) y se han sacado los datos relevantes para el monitoreo.
El sn simulado que se le asigna al esp32 no es relevante, se ha puesto por defecto "ESP32SOLAX". 
El sistema manda un correo todas las noches a las 22 con los datos capturados de todo el día.
La app muestra en tiempo real los datos requeridos (muestreo de 5s).

En el Firmware debes de rellenar:
-Tu SSID de red wifi, contraseña wifi, nombre de la instalación solar, correo emisor, correo receptor (pueden ser los mismos), key para terceros de gmail (sin espacios), URL proyecto Firebase y key secreta de FireBase.

CONEXIÓN ESP32DEVKITV1
-Cable negro USB-TipoA -> GND
-Cable blanco USB-TipoA -> Rx2
-Cable verde USB-TipoA -> Tx2
-Cable rojo -> No Conectado (Alimentación externa)

DATOS MONITOR:
-RED ELÉCTRICA:
  -Voltaje R/S/T
  -Corriente R/S/T
  -Potencia R/S/T
  -Frecuencia R/S/T
-GENERACIÓN:
  -Hoy/Mensual/Total
-EXPORTACIÓN A RED:
  -Hoy/Mensual/Total
-TEMPERATURA DEL INVERSOR
  -Máxima/Media/Mínima/Sonda2

DATOS DEL INFORME DEL CORREO:
-Fecha
-Nombre de la Instalación
-Producción Diaria
-Vertido a la Red
-Producción Mensual
-Producción Total (Desde instalación)
Temperaturas del inversor Max/Med/Min

Si quieres anular el vertido a red desde el propio inversor entrar en ->settings->password "2014"->Export energy-> Disable


  
