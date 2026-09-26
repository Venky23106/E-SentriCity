#include "esp_camera.h"
#include <WiFi.h>

// =====================================================
// WIFI
// =====================================================

const char* ssid     = "Rajath";
const char* password = "12345678";

// =====================================================
// TRIGGER
// ESP32 Dev GPIO32 -> ESP32-CAM GPIO13
// =====================================================

#define TRIGGER_PIN 13

// =====================================================
// AI-THINKER ESP32-CAM CAMERA PINS
// =====================================================

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5

#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// =====================================================
// SERVER
// =====================================================

WiFiServer server(80);

// =====================================================
// CAMERA INITIALIZATION
// =====================================================

bool initCamera() {

  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;

  config.pixel_format = PIXFORMAT_JPEG;

  // Good starting point for Wi-Fi streaming
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;

  // Use PSRAM when available
  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.print("Camera init failed: 0x");
    Serial.println(err, HEX);
    return false;
  }

  // Optional image tuning
  sensor_t* s = esp_camera_sensor_get();

  if (s != nullptr) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
  }

  return true;
}

// =====================================================
// SEND SIMPLE HTML PAGE
// =====================================================

void sendHomePage(WiFiClient& client) {

  String html =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>SentriPole Camera</title>"
    "</head>"
    "<body>"
    "<h2>SentriPole ESP32-CAM</h2>"
    "<p>Trigger: "
    + String(digitalRead(TRIGGER_PIN) ? "ACTIVE" : "IDLE")
    + "</p>"
    "<p><a href=\"/stream\">Open Camera Stream</a></p>"
    "</body>"
    "</html>";

  client.print(html);
}

// =====================================================
// STREAM VIDEO
// =====================================================

void streamCamera(WiFiClient& client) {

  // Camera must be triggered
  if (digitalRead(TRIGGER_PIN) == LOW) {

    client.print(
      "HTTP/1.1 403 Forbidden\r\n"
      "Content-Type: text/plain\r\n"
      "Connection: close\r\n"
      "\r\n"
      "Camera stream is waiting for distress trigger."
    );

    delay(10);
    client.stop();

    return;
  }

  client.print(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    "Cache-Control: no-cache\r\n"
    "Pragma: no-cache\r\n"
    "Connection: close\r\n"
    "\r\n"
  );

  Serial.println("Camera streaming started.");

  while (client.connected()) {

    // Stop stream when distress trigger goes LOW
    if (digitalRead(TRIGGER_PIN) == LOW) {
      break;
    }

    camera_fb_t* fb = esp_camera_fb_get();

    if (!fb) {
      Serial.println("Camera capture failed.");
      break;
    }

    client.printf(
      "--frame\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: %u\r\n"
      "\r\n",
      fb->len
    );

    client.write(
      fb->buf,
      fb->len
    );

    client.print("\r\n");

    esp_camera_fb_return(fb);

    delay(30);
  }

  client.stop();

  Serial.println("Camera streaming stopped.");
}

// =====================================================
// HANDLE HTTP REQUEST
// =====================================================

void handleClient(WiFiClient& client) {

  String request = "";

  unsigned long timeout = millis();

  while (client.connected() &&
         millis() - timeout < 1000) {

    if (client.available()) {

      char c = client.read();

      request += c;

      if (request.endsWith("\r\n\r\n")) {
        break;
      }

      timeout = millis();
    }
  }

  if (request.indexOf("GET /stream") >= 0) {

    streamCamera(client);

    return;
  }

  sendHomePage(client);

  delay(10);

  client.stop();
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("==================================");
  Serial.println("      SENTRIPOLE ESP32-CAM");
  Serial.println("==================================");

  // Trigger input
  pinMode(
    TRIGGER_PIN,
    INPUT
  );

  // Camera
  if (!initCamera()) {

    Serial.println("Camera initialization failed.");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("Camera initialized.");

  // Wi-Fi
  WiFi.mode(WIFI_STA);

  WiFi.begin(
    ssid,
    password
  );

  Serial.print("Connecting to Wi-Fi");

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi connected.");

  Serial.print("ESP32-CAM IP: ");
  Serial.println(WiFi.localIP());

  server.begin();

  Serial.println("Web server started.");

  Serial.println();
  Serial.println("Camera is READY.");
  Serial.println("Waiting for distress trigger on GPIO13.");
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  WiFiClient client = server.available();

  if (client) {

    Serial.println("Client connected.");

    handleClient(client);
  }

  delay(2);
}