#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <RF24.h>
#include <driver/i2s.h>
#include <math.h>


// =====================================================
// WIFI CONFIGURATION
// =====================================================

const char* ssid =
  "Rajath";

const char* password =
  "12345678";

// Raspberry Pi Ethernet IP
const char* PI_IP =
  "172.16.49.255";

// Raspberry Pi TCP port
#define PI_PORT 5000


// =====================================================
// PIN DEFINITIONS
// =====================================================

#define PIR_PIN 27


// -----------------------------------------------------
// INMP441
// -----------------------------------------------------

#define I2S_SCK 26
#define I2S_WS  25
#define I2S_SD  33


// -----------------------------------------------------
// nRF24L01
// -----------------------------------------------------

#define NRF_CE   4
#define NRF_CSN  5
#define NRF_SCK  18
#define NRF_MISO 19
#define NRF_MOSI 23


// =====================================================
// WIFI CLIENT
// =====================================================

WiFiClient piClient;


// =====================================================
// nRF24L01
// =====================================================

RF24 radio(
  NRF_CE,
  NRF_CSN
);


// Raspberry Pi nRF address
const byte PI_ADDRESS[6] = "PI001";

#define NRF_CHANNEL 76


// =====================================================
// nRF24 MODE
// =====================================================
//
// We keep the nRF24 hardware and transmission active.
//
// Auto ACK is disabled because this configuration was
// the one that previously demonstrated actual packet
// reception on the Raspberry Pi.
//
// Wi-Fi is the main reliable communication path.
//
// Set to true later if you want to revisit ACK.
//

#define NRF_USE_ACK false


// =====================================================
// SENSOR PACKET
// Keep packet <= 32 bytes for nRF24L01
// =====================================================

struct SensorPacket {

  char nodeID[8];

  uint8_t pir;

  uint8_t soundDetected;

  uint16_t micLevel;

  uint32_t sequence;

  uint32_t uptime;
};


SensorPacket packet;

uint32_t sequenceNumber = 0;


// =====================================================
// THREAT THRESHOLD
// =====================================================
//
// RMS is normalized approximately between 0 and 1.
//
// Below threshold -> threat = 0
// At/above threshold -> threat = 1
//

#define THREAT_THRESHOLD 0.08f


// =====================================================
// AUDIO SAMPLE COUNT
// =====================================================

#define SAMPLE_COUNT 512


// =====================================================
// RASPBERRY PI CONNECTION TIMING
// =====================================================

#define PI_RECONNECT_INTERVAL 2000

unsigned long lastPiReconnectAttempt = 0;


// =====================================================
// nRF STATUS
// =====================================================

bool nrfAvailable = false;


// =====================================================
// I2S MICROPHONE SETUP
// =====================================================

void setupI2S() {

  i2s_config_t i2s_config = {

    .mode = (i2s_mode_t)(
      I2S_MODE_MASTER |
      I2S_MODE_RX
    ),

    .sample_rate = 16000,

    .bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT,

    .channel_format =
      I2S_CHANNEL_FMT_ONLY_LEFT,

    .communication_format =
      I2S_COMM_FORMAT_STAND_I2S,

    .intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count = 4,

    .dma_buf_len = 256,

    .use_apll = false,

    .tx_desc_auto_clear = false,

    .fixed_mclk = 0
  };


  i2s_pin_config_t pin_config = {

    .bck_io_num = I2S_SCK,

    .ws_io_num = I2S_WS,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num = I2S_SD
  };


  // ---------------------------------------------------
  // Install I2S driver
  // ---------------------------------------------------

  if (
    i2s_driver_install(
      I2S_NUM_0,
      &i2s_config,
      0,
      NULL
    ) != ESP_OK
  ) {

    Serial.println(
      "I2S install failed!"
    );

    while (true) {

      delay(1000);
    }
  }


  // ---------------------------------------------------
  // Configure I2S pins
  // ---------------------------------------------------

  if (
    i2s_set_pin(
      I2S_NUM_0,
      &pin_config
    ) != ESP_OK
  ) {

    Serial.println(
      "I2S pin setup failed!"
    );

    while (true) {

      delay(1000);
    }
  }


  // ---------------------------------------------------
  // Clear DMA
  // ---------------------------------------------------

  i2s_zero_dma_buffer(
    I2S_NUM_0
  );


  Serial.println(
    "INMP441 ready."
  );
}


// =====================================================
// READ MICROPHONE RMS
// =====================================================

float readMicRMS() {

  // ---------------------------------------------------
  // Audio sample buffer
  // ---------------------------------------------------

  int32_t buffer[
    SAMPLE_COUNT
  ];


  size_t bytesRead = 0;


  // ---------------------------------------------------
  // Read microphone samples
  // ---------------------------------------------------

  esp_err_t result =
    i2s_read(
      I2S_NUM_0,
      buffer,
      sizeof(buffer),
      &bytesRead,
      portMAX_DELAY
    );


  // ---------------------------------------------------
  // Validate read
  // ---------------------------------------------------

  if (
    result != ESP_OK ||
    bytesRead == 0
  ) {

    return 0.0f;
  }


  // ---------------------------------------------------
  // Number of samples
  // ---------------------------------------------------

  int count =
    bytesRead /
    sizeof(int32_t);


  // ---------------------------------------------------
  // Sum of squares
  // ---------------------------------------------------

  double sumSquares =
    0.0;


  // ---------------------------------------------------
  // Process every sample
  // ---------------------------------------------------

  for (
    int i = 0;
    i < count;
    i++
  ) {

    // INMP441 gives 24-bit audio
    // stored inside a 32-bit sample

    float sample =
      (float)(
        buffer[i] >> 8
      );


    // Normalize approximately
    // to -1.0 ... +1.0

    sample =
      sample /
      8388608.0f;


    sumSquares +=
      (double)sample *
      sample;
  }


  // ---------------------------------------------------
  // Calculate RMS
  // ---------------------------------------------------

  float rms =
    sqrt(
      sumSquares /
      count
    );


  return rms;
}


// =====================================================
// CONVERT RMS TO PRINTABLE MIC LEVEL
// =====================================================
//
// This is only for Serial Monitor readability.
// Actual threat decision uses RMS.
//

uint16_t rmsToMicLevel(
  float rms
) {

  float scaled =
    rms *
    65535.0f;


  if (
    scaled < 0
  ) {

    scaled = 0;
  }


  if (
    scaled > 65535
  ) {

    scaled = 65535;
  }


  // Explicit C-style cast
  // to avoid the compile error.

  return (uint16_t)scaled;
}


// =====================================================
// WIFI SETUP
// =====================================================

void setupWiFi() {

  Serial.println();
  Serial.println(
    "Starting Wi-Fi..."
  );


  WiFi.mode(
    WIFI_STA
  );


  WiFi.begin(
    ssid,
    password
  );


  Serial.print(
    "Connecting to Wi-Fi"
  );


  unsigned long startTime =
    millis();


  while (
    WiFi.status() != WL_CONNECTED
  ) {

    delay(500);

    Serial.print(".");


    // -------------------------------------------------
    // Timeout after 15 seconds
    // -------------------------------------------------

    if (
      millis() - startTime > 15000
    ) {

      Serial.println();

      Serial.println(
        "Wi-Fi connection timeout."
      );

      return;
    }
  }


  Serial.println();

  Serial.println(
    "Wi-Fi connected."
  );


  // ---------------------------------------------------
  // ESP32 IP
  // ---------------------------------------------------

  Serial.print(
    "ESP32 IP: "
  );

  Serial.println(
    WiFi.localIP()
  );


  // ---------------------------------------------------
  // Gateway
  // ---------------------------------------------------

  Serial.print(
    "Gateway: "
  );

  Serial.println(
    WiFi.gatewayIP()
  );


  // ---------------------------------------------------
  // Raspberry Pi IP
  // ---------------------------------------------------

  Serial.print(
    "Raspberry Pi IP: "
  );

  Serial.println(
    PI_IP
  );
}


// =====================================================
// CONNECT TO RASPBERRY PI
// =====================================================

bool connectToPi() {

  // ---------------------------------------------------
  // Already connected
  // ---------------------------------------------------

  if (
    piClient.connected()
  ) {

    return true;
  }


  // ---------------------------------------------------
  // Don't reconnect continuously
  // ---------------------------------------------------

  if (
    millis() -
    lastPiReconnectAttempt <
    PI_RECONNECT_INTERVAL
  ) {

    return false;
  }


  lastPiReconnectAttempt =
    millis();


  // ---------------------------------------------------
  // Close previous connection
  // ---------------------------------------------------

  piClient.stop();


  Serial.println();

  Serial.print(
    "Connecting to Raspberry Pi at "
  );

  Serial.print(
    PI_IP
  );

  Serial.print(
    ":"
  );

  Serial.println(
    PI_PORT
  );


  // ---------------------------------------------------
  // Connect
  // ---------------------------------------------------

  if (
    piClient.connect(
      PI_IP,
      PI_PORT
    )
  ) {

    Serial.println(
      "Raspberry Pi Wi-Fi connection SUCCESS."
    );

    return true;

  } else {

    Serial.println(
      "Raspberry Pi Wi-Fi connection FAILED."
    );

    return false;
  }
}


// =====================================================
// nRF24 SETUP
// =====================================================

void setupNRF() {

  Serial.println();
  Serial.println(
    "Starting nRF24L01..."
  );


  // ---------------------------------------------------
  // Start SPI
  // ---------------------------------------------------

  SPI.begin(
    NRF_SCK,
    NRF_MISO,
    NRF_MOSI,
    NRF_CSN
  );


  // ---------------------------------------------------
  // Detect nRF24
  // ---------------------------------------------------

  if (
    !radio.begin()
  ) {

    Serial.println(
      "WARNING: nRF24L01 not detected!"
    );

    Serial.println(
      "Wi-Fi will continue working."
    );


    nrfAvailable =
      false;

    return;
  }


  // ---------------------------------------------------
  // nRF detected
  // ---------------------------------------------------

  nrfAvailable =
    true;


  // ---------------------------------------------------
  // Channel
  // ---------------------------------------------------

  radio.setChannel(
    NRF_CHANNEL
  );


  // ---------------------------------------------------
  // Data rate
  // ---------------------------------------------------

  radio.setDataRate(
    RF24_250KBPS
  );


  // ---------------------------------------------------
  // Power
  // ---------------------------------------------------

  radio.setPALevel(
    RF24_PA_LOW
  );


  // ---------------------------------------------------
  // CRC
  // ---------------------------------------------------

  radio.setCRCLength(
    RF24_CRC_16
  );


  // ---------------------------------------------------
  // ACK
  // ---------------------------------------------------

  if (
    NRF_USE_ACK
  ) {

    radio.setAutoAck(
      true
    );


    radio.setRetries(
      5,
      15
    );


    Serial.println(
      "nRF24 Auto ACK: ENABLED"
    );

  } else {

    radio.setAutoAck(
      false
    );


    radio.setRetries(
      0,
      0
    );


    Serial.println(
      "nRF24 Auto ACK: DISABLED"
    );
  }


  // ---------------------------------------------------
  // TX address
  // ---------------------------------------------------

  radio.openWritingPipe(
    PI_ADDRESS
  );


  // ---------------------------------------------------
  // TX mode
  // ---------------------------------------------------

  radio.stopListening();


  Serial.println(
    "nRF24L01 transmitter ready."
  );
}


// =====================================================
// SEND PACKET THROUGH nRF24
// =====================================================

bool sendNRFPacket() {

  if (
    !nrfAvailable
  ) {

    return false;
  }


  bool success =
    radio.write(
      &packet,
      sizeof(packet)
    );


  return success;
}


// =====================================================
// SEND PACKET THROUGH WIFI
// =====================================================
//
// Same sensor information is sent as JSON.
// One JSON object = one line.
//

bool sendWiFiPacket(
  float rms,
  uint8_t threat
) {

  // ---------------------------------------------------
  // Make sure Pi is connected
  // ---------------------------------------------------

  if (
    !connectToPi()
  ) {

    return false;
  }


  // ---------------------------------------------------
  // Build JSON packet
  // ---------------------------------------------------

  String json =
    "{";


  json +=
    "\"nodeID\":\"NODE01\",";


  json +=
    "\"pir\":";


  json +=
    String(
      packet.pir
    );


  json +=
    ",";


  json +=
    "\"rms\":";


  json +=
    String(
      rms,
      4
    );


  json +=
    ",";


  json +=
    "\"micLevel\":";


  json +=
    String(
      packet.micLevel
    );


  json +=
    ",";


  json +=
    "\"threat\":";


  json +=
    String(
      threat
    );


  json +=
    ",";


  json +=
    "\"soundDetected\":";


  json +=
    String(
      packet.soundDetected
    );


  json +=
    ",";


  json +=
    "\"sequence\":";


  json +=
    String(
      packet.sequence
    );


  json +=
    ",";


  json +=
    "\"uptime\":";


  json +=
    String(
      packet.uptime
    );


  json +=
    "}";


  // ---------------------------------------------------
  // Send JSON
  // ---------------------------------------------------

  size_t bytesSent =
    piClient.println(
      json
    );


  // ---------------------------------------------------
  // Check transmission
  // ---------------------------------------------------

  if (
    bytesSent > 0
  ) {

    return true;

  } else {

    piClient.stop();

    return false;
  }
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(
    115200
  );


  delay(1000);


  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    " ESP32 SENSOR NODE"
  );

  Serial.println(
    " nRF24 + Wi-Fi"
  );

  Serial.println(
    "================================"
  );


  // ===================================================
  // PIR
  // ===================================================

  pinMode(
    PIR_PIN,
    INPUT
  );


  // ===================================================
  // I2S MICROPHONE
  // ===================================================

  setupI2S();


  // ===================================================
  // nRF24
  // ===================================================

  setupNRF();


  // ===================================================
  // WIFI
  // ===================================================

  setupWiFi();


  // ===================================================
  // CONNECT TO PI
  // ===================================================

  connectToPi();


  // ===================================================
  // THRESHOLD INFORMATION
  // ===================================================

  Serial.println();

  Serial.println(
    "Threat detection enabled."
  );


  Serial.print(
    "Threat RMS threshold: "
  );


  Serial.println(
    THREAT_THRESHOLD,
    4
  );


  // ===================================================
  // READY
  // ===================================================

  Serial.println();

  Serial.println(
    "Sensor node ready."
  );

  Serial.println(
    "Wi-Fi + nRF24 communication active."
  );
}


// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  // ===================================================
  // CHECK WIFI
  // ===================================================

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    Serial.println();

    Serial.println(
      "Wi-Fi connection lost."
    );


    setupWiFi();
  }


  // ===================================================
  // PIR
  // ===================================================

  uint8_t pir =
    digitalRead(
      PIR_PIN
    );


  // ===================================================
  // MICROPHONE
  // ===================================================

  float rms =
    readMicRMS();


  // ===================================================
  // MIC LEVEL
  // ===================================================

  uint16_t mic =
    rmsToMicLevel(
      rms
    );


  // ===================================================
  // THREAT DECISION
  // ===================================================

  uint8_t threat =
    0;


  if (
    rms >=
    THREAT_THRESHOLD
  ) {

    threat =
      1;

  } else {

    threat =
      0;
  }


  // ===================================================
  // SOUND EVENT
  // ===================================================
  //
  // Currently follows threat.
  //
  // Later this can become the HELP keyword detector.
  //

  uint8_t soundDetected =
    threat;


  // ===================================================
  // CREATE SENSOR PACKET
  // ===================================================

  strcpy(
    packet.nodeID,
    "NODE01"
  );


  packet.pir =
    pir;


  packet.soundDetected =
    soundDetected;


  packet.micLevel =
    mic;


  packet.sequence =
    sequenceNumber++;


  packet.uptime =
    millis() /
    1000;


  // ===================================================
  // SERIAL MONITOR
  // ===================================================

  Serial.println();

  Serial.println(
    "-------------------------------"
  );


  Serial.print(
    "PIR          : "
  );

  Serial.println(
    packet.pir
  );


  Serial.print(
    "RMS          : "
  );

  Serial.println(
    rms,
    4
  );


  Serial.print(
    "Mic Level    : "
  );

  Serial.println(
    packet.micLevel
  );


  Serial.print(
    "Threshold    : "
  );

  Serial.println(
    THREAT_THRESHOLD,
    4
  );


  Serial.print(
    "THREAT       : "
  );

  Serial.println(
    threat
  );


  Serial.print(
    "Sound Event  : "
  );

  Serial.println(
    packet.soundDetected
  );


  Serial.print(
    "Sequence     : "
  );

  Serial.println(
    packet.sequence
  );


  // ===================================================
  // SEND THROUGH nRF24
  // ===================================================

  bool nrfSuccess =
    sendNRFPacket();


  if (
    nrfAvailable
  ) {

    if (
      nrfSuccess
    ) {

      Serial.println(
        "nRF → Raspberry Pi: SUCCESS"
      );

    } else {

      Serial.println(
        "nRF → Raspberry Pi: FAILED"
      );
    }

  } else {

    Serial.println(
      "nRF → Raspberry Pi: NOT AVAILABLE"
    );
  }


  // ===================================================
  // SEND THROUGH WIFI
  // ===================================================

  bool wifiSuccess =
    sendWiFiPacket(
      rms,
      threat
    );


  if (
    wifiSuccess
  ) {

    Serial.println(
      "Wi-Fi → Raspberry Pi: SUCCESS"
    );

  } else {

    Serial.println(
      "Wi-Fi → Raspberry Pi: FAILED"
    );
  }


  // ===================================================
  // THREAT EVENT
  // ===================================================

  if (
    threat == 1
  ) {

    Serial.println();

    Serial.println(
      ">>> THREAT DETECTED <<<"
    );
  }


  // ===================================================
  // PIR EVENT
  // ===================================================

  if (
    packet.pir == 1
  ) {

    Serial.println();

    Serial.println(
      ">>> PIR MOTION DETECTED <<<"
    );
  }


  // ===================================================
  // COMMUNICATION STATUS
  // ===================================================

  Serial.println();

  Serial.print(
    "nRF24 status : "
  );


  if (
    nrfAvailable
  ) {

    Serial.println(
      nrfSuccess
      ? "OK"
      : "FAILED"
    );

  } else {

    Serial.println(
      "NOT DETECTED"
    );
  }


  Serial.print(
    "Wi-Fi status : "
  );


  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      wifiSuccess
      ? "OK"
      : "PI CONNECTION FAILED"
    );

  } else {

    Serial.println(
      "DISCONNECTED"
    );
  }


  // ===================================================
  // LOOP DELAY
  // ===================================================

  delay(500);
}