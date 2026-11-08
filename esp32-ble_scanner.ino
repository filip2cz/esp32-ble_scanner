/*
 * ============================================================================
 * Description: This program acts as a standalone BLE (Bluetooth Low Energy) 
 *              radar. It creates its own Wi-Fi Access Point and hosts a 
 *              web interface that displays real-time data of discovered BLE 
 *              devices in the vicinity, including their MAC address, signal 
 *              strength (RSSI), raw HEX data, and basic type parsing.
 *              Includes decoders for common unencrypted BLE sensors.
 * Target:      ESP32-C3
 * Note:        This code was vibecoded using the Gemini AI model.
 * ============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <map>

// AP network configuration
const char* ssid = "ESP32_Radar";
const char* password = ""; // Open network for easy access

WebServer server(80);
BLEScan* pBLEScan;

// Structure to hold device data
struct DeviceData {
    String name;
    int rssi;
    String hexData;
    String parsedInfo;
    unsigned long lastSeen;
};

// Map to store devices by MAC address
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

// Callback class for handling discovered BLE devices
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        String mac = advertisedDevice.getAddress().toString().c_str();
        String name = advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "Neznámé";
        int rssi = advertisedDevice.getRSSI();

        String hexData = "-";
        String parsedInfo = "Neznámé zařízení";

        // 1. Check Manufacturer Data (Apple, Microsoft, Ruuvi, Govee)
        if (advertisedDevice.haveManufacturerData()) {
            String md = advertisedDevice.getManufacturerData();
            hexData = getHex((uint8_t*)md.c_str(), md.length());
            uint8_t* raw = (uint8_t*)md.c_str();
            
            // Apple (0x4C 0x00)
            if(md.length() >= 2 && raw[0] == 0x4C && raw[1] == 0x00) {
                parsedInfo = "🍎 Apple (AirTag/iPhone)";
            } 
            // Microsoft (0x06 0x00)
            else if (md.length() >= 2 && raw[0] == 0x06 && raw[1] == 0x00) {
                parsedInfo = "🪟 Microsoft";
            }
            // RuuviTag - Manufacturer ID 0x0499 (Little Endian: 0x99 0x04)
            else if (md.length() >= 7 && raw[0] == 0x99 && raw[1] == 0x04) {
                if (raw[2] == 0x05) { // Data Format 5 (RAWv2)
                    int16_t tempRaw = (raw[3] << 8) | raw[4];
                    float temp = tempRaw * 0.005;
                    uint16_t humRaw = (raw[5] << 8) | raw[6];
                    float hum = humRaw * 0.0025;
                    parsedInfo = "🦌 RuuviTag: " + String(temp, 1) + " °C, " + String(hum, 1) + " %";
                } else {
                    parsedInfo = "🦌 RuuviTag (Starší formát)";
                }
            }
            // Govee Thermometers (Often 0x88EC, e.g., H5075)
            else if (md.length() >= 7 && raw[0] == 0x88 && raw[1] == 0xEC) {
                int rawVal = (raw[3] << 16) | (raw[4] << 8) | raw[5];
                float temp = (rawVal / 1000) / 10.0;
                float hum = (rawVal % 1000) / 10.0;
                uint8_t batt = raw[6];
                parsedInfo = "💡 Govee: " + String(temp, 1) + " °C, " + String(hum, 1) + " %, 🔋 " + String(batt) + " %";
            }
        } 
        // 2. Check Service Data (Environmental sensors & beacons)
        else if (advertisedDevice.haveServiceData()) {
            int count = advertisedDevice.getServiceDataCount();
            if (count > 0) {
                String sd = advertisedDevice.getServiceData(0);
                BLEUUID serviceUUID = advertisedDevice.getServiceDataUUID(0);
                hexData = getHex((uint8_t*)sd.c_str(), sd.length());
                uint8_t* raw = (uint8_t*)sd.c_str();
                
                // Xiaomi Mijia with ATC1441 custom firmware (UUID 0x181A)
                if (serviceUUID.equals(BLEUUID((uint16_t)0x181A)) && sd.length() >= 11) {
                    int16_t tempRaw = (raw[6] << 8) | raw[7];
                    float temp = tempRaw / 10.0;
                    uint8_t hum = raw[8];
                    uint8_t batt = raw[9];
                    parsedInfo = "🌡️ Xiaomi ATC: " + String(temp, 1) + " °C, " + String(hum) + " %, 🔋 " + String(batt) + " %";
                }
                // SwitchBot Meter / Thermometer (UUID 0x0FD3)
                else if (serviceUUID.equals(BLEUUID((uint16_t)0x0FD3)) && sd.length() >= 6) {
                    uint8_t batt = raw[2] & 0x7F;
                    float temp = ((raw[3] & 0x0F) * 10.0 + (raw[4] & 0x7F)) / 10.0;
                    if ((raw[4] & 0x80) == 0) temp = -temp; // Sign bit flag
                    uint8_t hum = raw[5] & 0x7F;
                    parsedInfo = "🤖 SwitchBot: " + String(temp, 1) + " °C, " + String(hum) + " %, 🔋 " + String(batt) + " %";
                }
                // Qingping / ClearGrass Sensors (UUID 0xFDCD)
                else if (serviceUUID.equals(BLEUUID((uint16_t)0xFDCD))) {
                    parsedInfo = "🌱 Qingping Senzor";
                }
                // Inkbird BBQ Thermometers (UUID 0xFFF0)
                else if (serviceUUID.equals(BLEUUID((uint16_t)0xFFF0))) {
                    parsedInfo = "🥩 Inkbird BBQ Teploměr";
                }
                // BTHome Standard (UUID 0xFCD2)
                else if (serviceUUID.equals(BLEUUID((uint16_t)0xFCD2))) {
                    parsedInfo = "🏠 BTHome Smart Sensor";
                }
                // Xiaomi MiBeacon Original (UUID 0xFE95)
                else if (serviceUUID.equals(BLEUUID((uint16_t)0xFE95))) {
                    parsedInfo = "🇨🇳 Xiaomi MiBeacon (šifrováno)";
                } 
                // Generic fallback for unknown UUIDs
                else {
                    parsedInfo = "Senzor (UUID: " + String(serviceUUID.toString().c_str()).substring(4,8) + ")";
                }
            }
        }

        // Store or update device in map
        DeviceData d;
        d.name = name;
        d.rssi = rssi;
        d.hexData = hexData;
        d.parsedInfo = parsedInfo;
        d.lastSeen = millis();
        
        devices[mac] = d;
    }
};

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
    unsigned long now = millis();
    
    for (auto it = devices.begin(); it != devices.end(); ) {
        if (now - it->second.lastSeen > 15000) {
            it = devices.erase(it); 
        } else {
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
            ++it;
        }
    }
    
    server.send(200, "text/html", rows);
}

void setup() {
    Serial.begin(115200);
    
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
}