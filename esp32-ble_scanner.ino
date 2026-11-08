/*
 * =================================================================================
 * Description:     This program acts as a standalone BLE (Bluetooth Low Energy) 
 *                  radar. It creates its own Wi-Fi Access Point and hosts a 
 *                  web interface. It also displays the count of actively decoded
 *                  sensors on an attached I2C OLED screen using U8g2 library 
 *                  with a specific portrait offset.
 * Target:          ESP32-C3
 * Note:            This code was vibecoded using the Gemini Pro 3.1 model.
 * Conversation:    https://share.gemini.google/kFOl8hnWSdq7
 * =================================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <map>
#include <Wire.h>
#include <U8g2lib.h>

// I2C pins based on your working configuration
#define I2C_SDA 5
#define I2C_SCL 6

// U8g2 setup for portrait rotated screen
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R3, /* reset=*/ U8X8_PIN_NONE);

// Offsets for centering text on the specific 40x72 physical cutout
#define OFFSET_X 0
#define OFFSET_Y 32

// AP network configuration
const char* ssid = "ESP32_Radar";
const char* password = ""; 

WebServer server(80);
BLEScan* pBLEScan;

// Structure to hold device data
struct DeviceData {
    String name;
    int rssi;
    String hexData;
    String parsedInfo;
    bool isDecodedSensor; 
    unsigned long lastSeen;
};

std::map<String, DeviceData> devices;

// Helper function to convert raw byte array into readable HEX string
String getHex(uint8_t* data, size_t length) {
    String hex = "";
    for(int i = 0; i < length; i++) {
        char buf[3];
        sprintf(buf, "%02X", data[i]);
        hex += buf;
        if(i < length - 1) hex += " ";
    }
    return hex;
}

// Custom function to draw the sensor count using your specific screen offsets
void drawSensorCount(int count) {
    u8g2.clearBuffer();
    
    // Top line (Small font) - Title
    u8g2.setFont(u8g2_font_helvB10_tf);
    String msg1 = "BLE";
    int msg1Width = u8g2.getUTF8Width(msg1.c_str());
    int startX1 = OFFSET_X + ((40 - msg1Width) / 2);
    if (startX1 < OFFSET_X) startX1 = OFFSET_X; 
    u8g2.drawUTF8(startX1, OFFSET_Y + 28, msg1.c_str());
    
    // Bottom line (Giant font) - The actual number
    u8g2.setFont(u8g2_font_logisoso24_tf);
    String msg2 = String(count);
    int msg2Width = u8g2.getUTF8Width(msg2.c_str());
    int startX2 = OFFSET_X + ((40 - msg2Width) / 2);
    if (startX2 < OFFSET_X) startX2 = OFFSET_X;
    u8g2.drawUTF8(startX2, OFFSET_Y + 64, msg2.c_str());
    
    u8g2.sendBuffer();
}

// Callback class for handling discovered BLE devices
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        String mac = advertisedDevice.getAddress().toString().c_str();
        String name = advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "Unknown";
        int rssi = advertisedDevice.getRSSI();

        String hexData = "-";
        String parsedInfo = "Unknown device";
        bool isDecoded = false; 

        // Check Manufacturer Data
        if (advertisedDevice.haveManufacturerData()) {
            String md = advertisedDevice.getManufacturerData();
            hexData = getHex((uint8_t*)md.c_str(), md.length());
            uint8_t* raw = (uint8_t*)md.c_str();
            
            if(md.length() >= 2 && raw[0] == 0x4C && raw[1] == 0x00) {
                parsedInfo = "Apple (AirTag/iPhone)";
            } 
            else if (md.length() >= 2 && raw[0] == 0x06 && raw[1] == 0x00) {
                parsedInfo = "Microsoft";
            }
            else if (md.length() >= 7 && raw[0] == 0x99 && raw[1] == 0x04) {
                if (raw[2] == 0x05) { 
                    int16_t tempRaw = (raw[3] << 8) | raw[4];
                    float temp = tempRaw * 0.005;
                    uint16_t humRaw = (raw[5] << 8) | raw[6];
                    float hum = humRaw * 0.0025;
                    parsedInfo = "Ruuvi: " + String(temp, 1) + " C, " + String(hum, 1) + " %";
                    isDecoded = true; 
                }
            }
            else if (md.length() >= 7 && raw[0] == 0x88 && raw[1] == 0xEC) {
                int rawVal = (raw[3] << 16) | (raw[4] << 8) | raw[5];
                float temp = (rawVal / 1000) / 10.0;
                float hum = (rawVal % 1000) / 10.0;
                uint8_t batt = raw[6];
                parsedInfo = "Govee: " + String(temp, 1) + " C";
                isDecoded = true; 
            }
        } 
        // Check Service Data
        else if (advertisedDevice.haveServiceData()) {
            int count = advertisedDevice.getServiceDataCount();
            if (count > 0) {
                String sd = advertisedDevice.getServiceData(0);
                BLEUUID serviceUUID = advertisedDevice.getServiceDataUUID(0);
                hexData = getHex((uint8_t*)sd.c_str(), sd.length());
                uint8_t* raw = (uint8_t*)sd.c_str();
                
                if (serviceUUID.equals(BLEUUID((uint16_t)0x181A)) && sd.length() >= 11) {
                    int16_t tempRaw = (raw[6] << 8) | raw[7];
                    float temp = tempRaw / 10.0;
                    parsedInfo = "Xiaomi ATC: " + String(temp, 1) + " C";
                    isDecoded = true; 
                }
                else if (serviceUUID.equals(BLEUUID((uint16_t)0x0FD3)) && sd.length() >= 6) {
                    float temp = ((raw[3] & 0x0F) * 10.0 + (raw[4] & 0x7F)) / 10.0;
                    if ((raw[4] & 0x80) == 0) temp = -temp; 
                    parsedInfo = "SwitchBot: " + String(temp, 1) + " C";
                    isDecoded = true; 
                }
                else if (serviceUUID.equals(BLEUUID((uint16_t)0xFDCD))) {
                    parsedInfo = "Qingping Sensor";
                    isDecoded = true;
                }
                else if (serviceUUID.equals(BLEUUID((uint16_t)0xFFF0))) {
                    parsedInfo = "Inkbird BBQ";
                    isDecoded = true;
                }
                else if (serviceUUID.equals(BLEUUID((uint16_t)0xFCD2))) {
                    parsedInfo = "BTHome Sensor";
                    isDecoded = true;
                }
                else if (serviceUUID.equals(BLEUUID((uint16_t)0xFE95))) {
                    parsedInfo = "Xiaomi MiBeacon (Encrypted)";
                } 
                else {
                    parsedInfo = "Sensor (UUID: " + String(serviceUUID.toString().c_str()).substring(4,8) + ")";
                }
            }
        }

        // Store device
        DeviceData d;
        d.name = name;
        d.rssi = rssi;
        d.hexData = hexData;
        d.parsedInfo = parsedInfo;
        d.isDecodedSensor = isDecoded; 
        d.lastSeen = millis();
        
        devices[mac] = d;
    }
};

// Periodic cleanup and OLED update
void updateOLEDAndCleanup() {
    unsigned long now = millis();
    int targetSensorsCount = 0;
    
    for (auto it = devices.begin(); it != devices.end(); ) {
        if (now - it->second.lastSeen > 15000) {
            it = devices.erase(it); 
        } else {
            if (it->second.isDecodedSensor) {
                targetSensorsCount++;
            }
            ++it;
        }
    }
    
    // Update display only if count changed to prevent flicker
    static int lastCount = -1;
    if (targetSensorsCount != lastCount) {
        drawSensorCount(targetSensorsCount);
        lastCount = targetSensorsCount;
    }
}

// Handle root URL "/"
void handleRoot() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>BLE Radar</title>";
    html += "<style>";
    html += "body { font-family: system-ui, sans-serif; background: #121212; color: #fff; margin: 20px; }";
    html += "h1 { color: #00ffcc; font-size: 1.5rem; }";
    html += "table { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 0.95rem; }";
    html += "th, td { padding: 12px; border-bottom: 1px solid #333; text-align: left; }";
    html += "th { background: #222; position: sticky; top: 0; }";
    html += ".hex { font-family: monospace; color: #888; font-size: 0.85em; word-break: break-all; max-width: 250px; }";
    html += ".bars { font-family: monospace; font-size: 1.1em; margin-right: 5px; }";
    html += "</style>";
    
    html += "<script>";
    html += "setInterval(() => {";
    html += "  fetch('/data').then(res => res.text()).then(html => {";
    html += "    document.getElementById('tableBody').innerHTML = html;";
    html += "  });";
    html += "}, 2000);";
    html += "</script>";
    
    html += "</head><body>";
    html += "<h1>📡 ESP32-C3 BLE Radar</h1>";
    html += "<table><thead><tr><th>MAC / Jméno</th><th>Signál</th><th>Typ a data</th><th>RAW Hex Payload</th></tr></thead>";
    html += "<tbody id='tableBody'></tbody></table>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
}

// Handle "/data" URL
void handleData() {
    String rows = "";
    
    for (auto it = devices.begin(); it != devices.end(); ++it) {
        rows += "<tr>";
        rows += "<td><strong>" + it->first + "</strong><br><small style='color: #aaa;'>" + it->second.name + "</small></td>";
        
        String bars = "";
        String color = "";
        if(it->second.rssi > -60) {
            color = "#00ffcc"; bars = "▂▃▅▆▇";
        } else if (it->second.rssi > -75) {
            color = "#ffcc00"; bars = "▂▃▅--";
        } else if (it->second.rssi > -85) {
            color = "#ff8800"; bars = "▂▃---";
        } else {
            color = "#ff4444"; bars = "▂----";
        }
        
        rows += "<td style='color: " + color + ";'><span class='bars'>" + bars + "</span><br><small>" + String(it->second.rssi) + " dBm</small></td>";
        rows += "<td>" + it->second.parsedInfo + "</td>";
        rows += "<td class='hex'>" + it->second.hexData + "</td>";
        rows += "</tr>";
    }
    
    server.send(200, "text/html", rows);
}

void setup() {
    Serial.begin(115200);
    
    // Initialize I2C and U8g2 Display
    Wire.begin(I2C_SDA, I2C_SCL);
    u8g2.begin();
    u8g2.enableUTF8Print(); 
    
    // Initial loading screen
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.drawUTF8(OFFSET_X + 5, OFFSET_Y + 40, "Start");
    u8g2.sendBuffer();
    
    WiFi.softAP(ssid, password);
    WiFi.setSleep(false); 
    
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP Started. IP Address: ");
    Serial.println(IP);

    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.begin();

    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(false); 
    pBLEScan->setInterval(500);
    pBLEScan->setWindow(100); 
}

void loop() {
    server.handleClient();
    pBLEScan->start(1, false); 
    pBLEScan->clearResults(); 
    
    updateOLEDAndCleanup();
}