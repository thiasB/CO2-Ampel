#include <Arduino.h>

// Additional Serial / UART
#include <SoftwareSerial.h>

// MH-Z19 sensor library
#include <MHZ19.h>

// NewPixel library
#include <Adafruit_NeoPixel.h>

// WiFiMulti
#include <ESP8266WiFiMulti.h>

// InfluxDB
#include <InfluxDbClient.h>

// WiFi config
#include <wifi_config.h>

// InfluxDB config
#include <influxdb_config.h>

// Sensor location
const char *location = "Schlafzimmer";

// CO2 sensor configuration
#define MHZ19_BAUDRATE  9600    // default baudrate of sensor
#define MHZ19_TX_PIN    D7      // TX pin MH-Z19(B) (SoftwareSerail RX)
#define MHZ10_RX_PIN    D8      // RX pin MH-Z19(B) (SoftwareSerial TX)

// CO2 thresholds
#define CO2_THRESHOLD_GOOD    500
#define CO2_THRESHOLD_GOOD_MEDIUM    700
#define CO2_THRESHOLD_MEDIUM  900
#define CO2_THRESHOLD_MEDIUM_BAD  1100
#define CO2_THRESHOLD_BAD     1500
#define CO2_THRESHOLD_DEAD     2000

// NeoPixel configuration
#define NEOPIXEL_COUNT      1
#define NEOPIXEL_PIN        D2
#define NEOPIXEL_BRIGHTNESS 5   // 255 max

// application configuration
#define CONF_WARMUP_TIME_MS             180000  // 180s
#define CONF_ZERO_CALIBRATION_TIME_MS  1260000 // 21m
#define CONF_MEASUREMENT_INTERVALL_MS   300000   // 10s
#define CONF_ZERO_CALIBRATION_PIN       D3


#ifndef WIFI_CONFIG_H
#error Include a valid wifi_config.h
#endif
#ifndef INFLUXDB_CONFIG_H
#error Include a valid influxdb_config.h
#endif


// SoftwareSerial to communicate with sensor
SoftwareSerial mhz19Serial(MHZ19_TX_PIN, MHZ10_RX_PIN);

// MHZ19 sensor
MHZ19 mhz19Sensor;

// NeoPixel strip / ring
Adafruit_NeoPixel neoPixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// WiFi Multi client
ESP8266WiFiMulti wifiMulti;

// WiFi connect timeout per AP. Increase when connecting takes longer.
const uint32_t connectTimeout = 10000;

// InfluxDB client
InfluxDBClient client;

// InfluxDB Data point
Point influxsensors("Sensoren");

static const char alphanum[] ="0123456789"
                              "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                              "abcdefghijklmnopqrstuvwxyz";  // For random generation of client ID.

// values to start "zero calibration"
bool sendZeroCalibrationCmd = true;
unsigned long zeroCalibrationStartTimeMS;
// values to run initial "calibration"
bool initalCalibration = true;
unsigned long initalCalibrationStartTimeMS;
// Assume Wifi does not work, if connect set to true
bool wifiConnectionActive = false;
// Assume InfluxDB does not work, if connect set to true
bool influxdbConnectionActive = false;

// different operating modes
enum APPLICATION_MODE {
  MODE_INITIALIZATION,      // initialization
  MODE_ZERO_CALIBRATION,    // zero calibration
  MODE_MEASUREMENT          // default: do measurement
};

uint8_t currentApplicationMode = MODE_INITIALIZATION;

// change color of NeoPixels
void colorWipe(uint32_t color, int wait);
void loadingAnimation(uint8_t percent);

// interrupt for zero calibration
IRAM_ATTR void detectZeroCalibrationButtonPush() {
  Serial.println("DEBUG: Interrupt");
  currentApplicationMode = MODE_ZERO_CALIBRATION;
  zeroCalibrationStartTimeMS = millis();
}

void checkSensorReturnCode() {
    if (mhz19Sensor.errorCode != RESULT_OK) {
        Serial.println("FAILED TO READ SENSOR!");
        Serial.printf("Error code: %d\n\r", mhz19Sensor.errorCode);
        for (uint8_t i=0; i<2; i++) {
            colorWipe(neoPixels.Color(0, 255, 255), 50);
            delay(500);
            colorWipe(neoPixels.Color(0,0,0), 50);
            delay(500);
        }
    }
}

void setup() {
  // Setup serial for output
  Serial.begin(9600);

  // Setup software serial for communication with sensor
  Serial.println("Setup: SoftwareSerial for MH-Z19 sensor");
  mhz19Serial.begin(MHZ19_BAUDRATE);

  // MHZ19 init
  Serial.println("Setup: Initializing MH-Z19 sensor");
  mhz19Sensor.begin(mhz19Serial);
  // enable auto calibration (lowest value in 24h as baseline for 400ppm)
  mhz19Sensor.autoCalibration();

  // NeoPixel init
  Serial.println("Setup: Initializing NeoPixels");
  neoPixels.begin();  // init
  neoPixels.show();   // off
  neoPixels.setBrightness(NEOPIXEL_BRIGHTNESS);

  // interrupt pin for zero calibration
  pinMode(CONF_ZERO_CALIBRATION_PIN, INPUT_PULLUP);
  attachInterrupt(
    digitalPinToInterrupt(CONF_ZERO_CALIBRATION_PIN), 
    detectZeroCalibrationButtonPush, 
    RISING
  );

  checkSensorReturnCode();

  //WIFi Multi
#ifdef WIFI_CONFIG_H
  WiFi.mode(WIFI_STA);
  Serial.println("Setting up WiFi");

  #ifdef WIFI_CONFIG_SSID_1
  #ifndef WIFI_CONFIG_PASSWORD_1
    wifiMulti.addAP(WIFI_CONFIG_SSID_1);
  #else
    wifiMulti.addAP(WIFI_CONFIG_SSID_1, WIFI_CONFIG_PASSWORD_1);
  #endif
  #endif

  #ifdef WIFI_CONFIG_SSID_2
  #ifndef WIFI_CONFIG_PASSWORD_2
    wifiMulti.addAP(WIFI_CONFIG_SSID_2);
  #else
    wifiMulti.addAP(WIFI_CONFIG_SSID_2, WIFI_CONFIG_PASSWORD_2);
  #endif
  #endif

  #ifdef WIFI_CONFIG_SSID_3
  #ifndef WIFI_CONFIG_PASSWORD_3
    wifiMulti.addAP(WIFI_CONFIG_SSID_3);
  #else
    wifiMulti.addAP(WIFI_CONFIG_SSID_3, WIFI_CONFIG_PASSWORD_3);
  #endif
  #endif

  // InfluxDB
  client.setConnectionParamsV1(INFLUXDB_SERVER, INFLUXDB_DATABASE);
  // Add constant tags - only once
  influxsensors.addTag("location", location);

#else
  Serial.println("No Wifi Info Present, Light Only Mode");
  wifiConnectionActive = false;
#endif

  // finished time of setup, used for initial calibration
  initalCalibrationStartTimeMS = millis();
}

void loop() {
  unsigned long now = millis();

  switch (currentApplicationMode) {
  case MODE_INITIALIZATION:
    // may move into function
    if (now - initalCalibrationStartTimeMS < CONF_WARMUP_TIME_MS) {
      if (now % 1000 == 0) {
        // NOTE: may use Serial.printf()
        Serial.print("Initial calibration in progress: ");
        Serial.print((now - initalCalibrationStartTimeMS + 1000)/1000);
        Serial.print("/");
        Serial.print(CONF_WARMUP_TIME_MS/1000);
        Serial.println("s");
        loadingAnimation(map((now - initalCalibrationStartTimeMS), 0, CONF_WARMUP_TIME_MS, 0, 100));
        delay(10);
      }
    }
    // change mode to measurement
    else {
      Serial.println("Switch to measurement mode.");
      currentApplicationMode = MODE_MEASUREMENT;
    }
    break;
  case MODE_ZERO_CALIBRATION:
    // may move into function
    if (now - initalCalibrationStartTimeMS < CONF_WARMUP_TIME_MS) {
      // send zero calibration command to sensor
      if (sendZeroCalibrationCmd) {
        colorWipe(neoPixels.Color(0,0,0), 100);
        Serial.println("Start zero calibration progress.");
        mhz19Sensor.calibrateZero();
        sendZeroCalibrationCmd = false;
      }
      // send status to serial
      if (now % 10000 == 0) {
        // NOTE: may use Serial.printf()
        Serial.print("Zero calibration in progress: ");
        Serial.print((now - zeroCalibrationStartTimeMS)/1000);
        Serial.print("/");
        Serial.print(CONF_ZERO_CALIBRATION_TIME_MS/1000);
        Serial.println("s");
        delay(10);
      }
      // update neopixels every second
      if (now % 1000 == 0) {
        loadingAnimation(
          map((now - zeroCalibrationStartTimeMS), 0, CONF_ZERO_CALIBRATION_TIME_MS, 0, 100)
        );
      }
    }
    // zero calibration is done -> measurement mode
    else {
      Serial.println("Switch to measurement mode.");
      currentApplicationMode = MODE_MEASUREMENT;
      // set send flag to true if we change again back to zero calibration mode
      sendZeroCalibrationCmd = true;
    }
    break;
  // default mode is measurement
  case MODE_MEASUREMENT:
  default:
    if (now % CONF_MEASUREMENT_INTERVALL_MS == 0) {
      int co2Value = mhz19Sensor.getCO2();
      float temperature = mhz19Sensor.getTemperature();

      // continue loop on failed measurement
      if (co2Value == 0) {
        break;
      }

      influxsensors.clearFields();
      checkSensorReturnCode();

      Serial.printf("CO2 [ppm]: %4i, Temperature [C]: %.1f\n\r", co2Value, temperature);

      if (co2Value <= CO2_THRESHOLD_GOOD) {
        colorWipe(neoPixels.Color(   44, 186,   0), 200);
      }
      else if ((co2Value > CO2_THRESHOLD_GOOD) && (co2Value <= CO2_THRESHOLD_GOOD_MEDIUM)) {
        colorWipe(neoPixels.Color(  163, 255,   0), 200);
      }
      else if ((co2Value > CO2_THRESHOLD_GOOD_MEDIUM) && (co2Value <= CO2_THRESHOLD_MEDIUM)) {
        colorWipe(neoPixels.Color(  255, 244,   0), 200);
      }
      else if ((co2Value > CO2_THRESHOLD_MEDIUM) && (co2Value <= CO2_THRESHOLD_MEDIUM_BAD)) {
        colorWipe(neoPixels.Color(  255, 167,   0), 200);
      }
      else if ((co2Value > CO2_THRESHOLD_MEDIUM_BAD) && (co2Value <= CO2_THRESHOLD_BAD)) {
        colorWipe(neoPixels.Color(  255,   0,   0), 200);
      }
      else if ((co2Value > CO2_THRESHOLD_BAD) && (co2Value <= CO2_THRESHOLD_DEAD)) {
        colorWipe(neoPixels.Color(  255,   0, 127), 200);
      }
      else if (co2Value > CO2_THRESHOLD_DEAD) {
        colorWipe(neoPixels.Color(  255,   0,  255), 200);
      }

//If We have Wifi and InfluxDB, send it
#ifdef WIFI_CONFIG_H
      //      wifiMulti.run(connectTimeout);

      if (wifiMulti.run(connectTimeout) == WL_CONNECTED) {
        Serial.print("Connected to wireless network '");
        Serial.print(WiFi.SSID());
        Serial.print("' with IP: ");
        Serial.println(WiFi.localIP());
        wifiConnectionActive = true;
      } else {
        Serial.println("Could not Connect to Wifi");
        wifiConnectionActive = false;
      }

      // Check InfluxDB server connection
      if (client.validateConnection()) {
        Serial.print("Connected to InfluxDB: ");
        Serial.println(client.getServerUrl());
        influxdbConnectionActive = true;
      } else {
        Serial.print("InfluxDB connection failed: ");
        Serial.println(client.getLastErrorMessage());
        influxdbConnectionActive = false;
      }

      // Write point to InfluxDB
      if ((wifiConnectionActive) && (influxdbConnectionActive)) {
        influxsensors.addField("co2", co2Value);
        influxsensors.addField("temperature", temperature);
        if (!client.writePoint(influxsensors)) {
          Serial.print("InfluxDB write failed: ");
          Serial.println(client.getLastErrorMessage());
        } else {
          Serial.println("Send OK");
        }
      }
#endif

      delay(10);
    }
    break;
  }
}


void colorWipe(uint32_t color, int wait) {
  for(int i=0; i<neoPixels.numPixels(); i++) {
    neoPixels.setPixelColor(i, color);
    neoPixels.show();
    delay(wait);
  }
}

void loadingAnimation(uint8_t percent) {
  int num_pixel_on = map(percent, 0, 100, 0, neoPixels.numPixels()-1);
  for (int i=0; i<num_pixel_on; i++) {
    neoPixels.setPixelColor(i, neoPixels.Color(255, 255, 255));
    neoPixels.show();
  }
  if (num_pixel_on < neoPixels.numPixels()) {
    for (uint8_t j=0; j<255; j++) {
      neoPixels.setPixelColor(num_pixel_on, neoPixels.Color(j, j, j));
      neoPixels.show();
    }
    for (uint8_t k=255; k>0; k--) {
      neoPixels.setPixelColor(num_pixel_on, neoPixels.Color(k, k, k));
      neoPixels.show();
    }
  }
}
