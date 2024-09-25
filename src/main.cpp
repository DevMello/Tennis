#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "ICM_20948.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>  // Include WiFi library
#include <WebServer.h>  // Include WebServer library

int sck = 18;
int miso = 19;
int mosi = 21;
int cs = 5;
#define SERIAL_PORT Serial
#define WIRE_PORT Wire
#define AD0_VAL 1
ICM_20948_I2C myICM;

// Define BLE UUIDs
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_FILE_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_DATA_UUID      "e3223119-9445-4e96-a4a1-85358c4046a2"
BLEDescriptor *pDescr;
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristicFile = NULL;
BLECharacteristic* pCharacteristicData = NULL;
BLE2902 *pBLE2902;
bool deviceConnected = false;
bool oldDeviceConnected = false;
void startWiFiWebServer();
void stopWiFiWebServer();

// WiFi and Web Server
WebServer server;
const char* ssid = "DataLog"; // WiFi SSID
const char* password = ""; // Leave empty for open network

// BLE Callbacks
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    }

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      WiFi.disconnect(); // Disconnect WiFi on device disconnection
    }
};

class CharacteristicCallBack: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    uint8_t* pChar2Data = pChar->getData();
    uint8_t valueReceived = pChar2Data[0];
    
    if (valueReceived == 1) {
      startWiFiWebServer();
    } else if (valueReceived == 0) {
      stopWiFiWebServer();
    }
  }

  void onRead(BLECharacteristic *pChar) override {
    // Do nothing
  }
};

void appendFile(fs::FS &fs, const char *path, const char *message) {
  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    SERIAL_PORT.println("Failed to open file for appending");
    return;
  }
  if (file.print(message)) {
    SERIAL_PORT.println("Message appended");
  } else {
    SERIAL_PORT.println("Append failed");
  }
  file.close();
}

// Function to write a message to a file
void writeFile(fs::FS &fs, const char *path, const char *message) {
  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  if (file.print(message)) {
    Serial.println("File written");
  } else {
    Serial.println("Write failed");
  }
  file.close();
}

// Function to send file data over BLE
void sendFileOverBLE(const char *path) {
  File file = SD.open(path, FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }
  
  // Read the file and send it
  while (file.available()) {
    String line = file.readStringUntil('\n');
    pCharacteristicFile->setValue(line.c_str());
    pCharacteristicFile->notify();
    delay(5); // Small delay to prevent flooding
  }

  file.close();
  Serial.println("File sent successfully.");
}

void startWiFiWebServer() {
  WiFi.softAP(ssid, password); // Start WiFi AP
  Serial.println("WiFi AP started.");
  
  // Serve the file with CORS headers
  server.on("/shots.csv", HTTP_GET, []() {
    File file = SD.open("/shots.csv", FILE_READ);
    if (!file) {
      server.send(404, "text/plain", "File not found");
      return;
    }

    // Add CORS headers
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");

    server.streamFile(file, "text/csv");
    file.close();
  });

  // Handle CORS preflight requests
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.sendHeader("Access-Control-Allow-Methods", "GET");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      server.send(204);  // Respond with 'No Content' for CORS preflight
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });

  server.begin(); // Start the server
  Serial.println("HTTP server started.");
}


void stopWiFiWebServer() {
  server.stop(); // Stop the server
  WiFi.softAPdisconnect(true); // Disconnect the AP
  Serial.println("WiFi AP stopped.");
}

void setup() {
  Serial.begin(115200);
  
  // Initialize BLE
  BLEDevice::init("TennisAssistant");
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create characteristics
  pCharacteristicFile = pService->createCharacteristic(
                          CHAR_FILE_UUID,
                          BLECharacteristic::PROPERTY_READ |
                          BLECharacteristic::PROPERTY_WRITE 
                        );

  pCharacteristicFile->setCallbacks(new CharacteristicCallBack());

  pDescr = new BLEDescriptor((uint16_t)0x2901);
  pDescr->setValue("A very interesting variable");
  pCharacteristicFile->addDescriptor(pDescr);
  pCharacteristicFile->addDescriptor(new BLE2902());
  pService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);
  BLEDevice::startAdvertising();
  
  Serial.println("BLE device started, waiting for client connection...");

  // Initialize SD card and IMU
  SPI.begin(sck, miso, mosi, cs);
  if (!SD.begin(cs)) {
    Serial.println("Card Mount Failed");
    return;
  }
  Serial.println("SD Card initialized.");
  
  writeFile(SD, "/shots.csv", "time,w,x,y,z\n");

  // Initialize IMU
  WIRE_PORT.begin();
  WIRE_PORT.setClock(400000);
  bool initialized = false;
  while (!initialized) {
    myICM.begin(WIRE_PORT, AD0_VAL);
    if (myICM.status == ICM_20948_Stat_Ok) {
      initialized = true;
    } else {
      Serial.println(F("Trying again..."));
      delay(500);
    }
  }
  
  // Configure DMP
  bool success = true;
  success &= (myICM.initializeDMP() == ICM_20948_Stat_Ok);
  success &= (myICM.enableDMPSensor(INV_ICM20948_SENSOR_GAME_ROTATION_VECTOR) == ICM_20948_Stat_Ok);
  success &= (myICM.setDMPODRrate(DMP_ODR_Reg_Quat6, 0) == ICM_20948_Stat_Ok);
  success &= (myICM.enableFIFO() == ICM_20948_Stat_Ok);
  success &= (myICM.enableDMP() == ICM_20948_Stat_Ok);
  success &= (myICM.resetDMP() == ICM_20948_Stat_Ok);
  success &= (myICM.resetFIFO() == ICM_20948_Stat_Ok);

  if (success) {
    SERIAL_PORT.println(F("DMP enabled!"));
  } else {
    SERIAL_PORT.println(F("Enable DMP failed!"));
    while (1);
  }
}

void loop() {
  if (deviceConnected) {
    static unsigned long startTime = millis();
    static unsigned long currentTime = 0;

    // Collect data for 45 seconds
    if (currentTime < 10000) {
      icm_20948_DMP_data_t data;
      myICM.readDMPdataFromFIFO(&data);

      if (myICM.status == ICM_20948_Stat_Ok || myICM.status == ICM_20948_Stat_FIFOMoreDataAvail) {
        if ((data.header & DMP_header_bitmap_Quat6) > 0) {
          double q1 = ((double)data.Quat6.Data.Q1) / 1073741824.0;
          double q2 = ((double)data.Quat6.Data.Q2) / 1073741824.0;
          double q3 = ((double)data.Quat6.Data.Q3) / 1073741824.0;
          double q0 = sqrt(1.0 - ((q1 * q1) + (q2 * q2) + (q3 * q3)));

          double qw = q0;
          double qx = q2;
          double qy = q1;
          double qz = -q3;

          // Calculate elapsed time
          currentTime = millis() - startTime;

          // Format data as CSV
          char buffer[100];
          snprintf(buffer, sizeof(buffer), "%lu,%f,%f,%f,%f\n", currentTime, qw, qx, qy, qz);
          
          // Append to the CSV file
          appendFile(SD, "/shots.csv", buffer);
        }
      }

      if (myICM.status != ICM_20948_Stat_FIFOMoreDataAvail) {
        delay(10);
      }
    }
  } else {
    Serial.println("Looping...");
    server.handleClient(); // Handle web server requests
  }
  
}
