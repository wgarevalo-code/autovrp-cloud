# AutoVRP — Instrucciones de despliegue

## PASO 1 — Subir el servidor a Railway

1. Entra a https://railway.app y crea una cuenta gratuita
2. Haz clic en "New Project" → "Deploy from GitHub repo"
   - Primero necesitas subir la carpeta D:\AutoVRP_Cloud a GitHub:
   - Crea una cuenta en https://github.com si no tienes
   - Crea un repositorio nuevo llamado "autovrp-cloud"
   - Sube los archivos: server.js, package.json y la carpeta public/
3. Railway detecta automaticamente que es Node.js y lo despliega
4. Railway te da una URL publica como: https://autovrp-cloud.up.railway.app

## PASO 2 — Modificar el Gateway ESP32

1. Abre el archivo GATEWAY_MODIFICADO.ino en Arduino IDE
2. Busca esta linea cerca del inicio:
   const char* URL_NUBE = "https://TU-PROYECTO.up.railway.app/actualizar";
3. Cambia TU-PROYECTO por la URL que te dio Railway
   Ejemplo: "https://autovrp-cloud.up.railway.app/actualizar"
4. Sube el codigo al Gateway Heltec WiFi LoRa 32 V3
5. Abre el Monitor Serie — deberas ver "Nube: 200" cada 5 segundos (200 = OK)

## PASO 3 — Configurar IFTTT para "OK Google"

1. Entra a https://ifttt.com y crea una cuenta gratuita
2. Haz clic en "Create"
3. IF THIS → busca "Google Assistant" → "Say a simple phrase"
   - What do you want to say? → "dime la presion de la camara"
   - What do you want the Assistant to say in response? → "$"
4. THEN THAT → busca "Webhooks" → "Make a web request"
   - URL: https://TU-PROYECTO.up.railway.app/camara1/presion
   - Method: GET
   - Content Type: text/plain
5. Guarda el applet

### Comandos disponibles para Google Assistant:

| Lo que dices              | URL que llama IFTTT                    |
|---------------------------|----------------------------------------|
| "presion de la camara"    | /camara1/presion                       |
| "humedad de la camara"    | /camara1/humedad                       |
| "temperatura de la camara"| /camara1/temperatura                   |
| "estado de la camara"     | /camara1/estado                        |
| "boya de la camara"       | /camara1/boya                          |

## PASO 4 — Cuando llegue el sensor de temperatura/humedad

El codigo del Gateway ya tiene la variable `temperatura` preparada.
Solo necesitas:
1. Conectar el sensor (DHT22 o SHT31) al Nodo E213
2. Agregar la lectura en el codigo del Nodo
3. Enviar el dato en el mensaje LoRa junto con la presion
4. Parsear el dato en el Gateway (funcion parsearRespuesta)

No hay que tocar el servidor en la nube — ya acepta temperatura.
