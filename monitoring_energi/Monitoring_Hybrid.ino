#include <WiFi.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h> // Library Watchdog Timer bawaan ESP32

// Timeout WDT dalam detik
#define WDT_TIMEOUT 10

// ============================================================
// CONFIGURATION: WIFI & MQTT BROKER
// ============================================================
const char* WIFI_SSID     = "Tselhome-9553";
const char* WIFI_PASSWORD = "74367777";

const char* MQTT_SERVER   = "broker.emqx.io";
const int   MQTT_PORT     = 1883;
const char* DEVICE_ID     = "ESP32-001";

// Topic MQTT (Sesuai dengan Django Subscriber)
const char* TOPIC_PLTS    = "Bangkit/ESP32-001/sensor/PLTS";
const char* TOPIC_PLTB    = "Bangkit/ESP32-001/sensor/PLTB";
const char* TOPIC_BATTERY = "Bangkit/ESP32-001/sensor/BATTERY";

// ============================================================
// CONFIGURATION: HARDWARE PINS
// ============================================================
#define RELAY_PIN       18   // Pin Relay Control
#define MAX485_DE       4    // DE/RE Pin Transceiver RS485
#define RX2_PIN         16   // ESP32 RX2 -> MAX485 RO
#define TX2_PIN         17   // ESP32 TX2 -> MAX485 DI

// [KONFIGURASI PIN I2C ANDA]
#define I2C_SDA         22   // SDA Pin I2C (INA219)
#define I2C_SCL         21   // SCL Pin I2C (INA219)

#define RELAY_ON_PLTS   HIGH // Switch ke PLTS
#define RELAY_OFF_PLTB  LOW  // Switch ke PLTB / Standby

// ============================================================
// OBJECT INITIALIZATION
// ============================================================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
ModbusMaster epever;
Adafruit_INA219 ina219;

// Structure Data Sensor
struct PLTS_Data {
  float pvVoltage   = 0.0;
  float pvCurrent   = 0.0;
  float pvPower     = 0.0;
  float battVoltage = 0.0;
  float battCurrent = 0.0;
  float battSOC     = 0.0;
  float battTemp    = 0.0;
  float devTemp     = 0.0;
  float energyToday = 0.0;
  float energyTotal = 0.0;
  bool  isValid     = false;
  uint8_t lastErrorCode = 0;
} plts;

struct PLTB_Data {
  float voltage = 0.0;
  float current = 0.0;
  float power   = 0.0;
  bool  isValid = false;
} pltb;

// Status Relay & Keterangan untuk Debug
String relayStatusStr = "STANDBY";
String relayReasonStr = "Inisialisasi";

// Non-blocking Timers
unsigned long lastReadPLTS  = 0;
unsigned long lastReadPLTB  = 0;
unsigned long lastMqttPub   = 0;
unsigned long lastMqttRetry = 0;

// Function Declarations
void preTransmission();
void postTransmission();
void setupWiFi();
void reconnectMQTT();
void readEpeverData();
void readINA219Data();
void controlRelayLogic();
void publishSensorData();
void printSerialDashboard();
String getModbusErrorStr(uint8_t err);

// ============================================================
// RS485 HARDWARE CONTROL FUNCTION
// ============================================================
void preTransmission() {
  digitalWrite(MAX485_DE, 1);
}

void postTransmission() {
  digitalWrite(MAX485_DE, 0);
}

// ============================================================
// HELPER: MODBUS ERROR STRING
// ============================================================
String getModbusErrorStr(uint8_t err) {
  switch (err) {
    case 0x00: return "OK (Success)";
    case 0xE0: return "Illegal Function";
    case 0xE1: return "Illegal Data Address";
    case 0xE2: return "Illegal Data Value";
    case 0xE3: return "Slave Device Failure";
    case 0xE4: return "Invalid Slave ID";
    case 0xE5: return "Invalid Function";
    case 0xE6: return "Response Timed Out (Tidak Ada Respon Epever)";
    case 0xE7: return "Invalid CRC";
    default:   return "Error Code: 0x" + String(err, HEX);
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n==============================================================");
  Serial.println("       ESP32 MONITORING ENERGI HYBRID (PLTS & PLTB)           ");
  Serial.println("==============================================================");
  Serial.printf("[INIT] Device ID  : %s\n", DEVICE_ID);
  Serial.printf("[INIT] Relay Pin  : GPIO %d\n", RELAY_PIN);
  Serial.printf("[INIT] RS485 Pins : RX=%d, TX=%d, DE/RE=%d\n", RX2_PIN, TX2_PIN, MAX485_DE);
  Serial.printf("[INIT] I2C Pins   : SDA=GPIO %d, SCL=GPIO %d\n", I2C_SDA, I2C_SCL);

  // 1. Setup Relay Pin
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF_PLTB);
  Serial.println("[INIT] Relay diset ke mode STANDBY / PLTB (LOW)");

  // 2. Setup RS485 & Serial2 Modbus (Timeout 200ms)
  pinMode(MAX485_DE, OUTPUT);
  digitalWrite(MAX485_DE, 0);
  Serial2.begin(115200, SERIAL_8N1, RX2_PIN, TX2_PIN);
  Serial2.setTimeout(200); 
  epever.begin(1, Serial2);
  epever.preTransmission(preTransmission);
  epever.postTransmission(postTransmission);
  Serial.println("[OK] Modbus RS485 Epever siap.");

  // 3. Setup I2C INA219 (PLTB) dengan Pin Kustom (SDA=22, SCL=21)
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!ina219.begin()) {
    Serial.println("[WARN] Sensor INA219 tidak terdeteksi di pin I2C (SDA:22, SCL:21)!");
  } else {
    ina219.setCalibration_32V_2A();
    Serial.println("[OK] Sensor INA219 (PLTB) berhasil diinisialisasi.");
  }

  // 4. Inisialisasi Watchdog Timer (WDT)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = WDT_TIMEOUT * 1000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
#else
  esp_task_wdt_init(WDT_TIMEOUT, true);
#endif
  esp_task_wdt_add(NULL);
  Serial.println("[OK] Watchdog Timer aktif (10s).");

  // 5. Setup WiFi & MQTT Configuration
  setupWiFi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(512);

  Serial.println("==============================================================");
  Serial.println("[SUCCESS] Setup Selesai! Memulai pemantauan...");
  Serial.println("==============================================================\n");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  // 1. Reset Watchdog Timer di setiap iterasi loop
  esp_task_wdt_reset();

  // 2. Jaga Koneksi Jaringan (Non-blocking)
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFi();
  } else if (!mqttClient.connected()) {
    reconnectMQTT();
  } else {
    mqttClient.loop();
  }

  unsigned long currentMillis = millis();

  // 3. Baca Data PLTS (EPEver RS485) setiap 1000ms
  if (currentMillis - lastReadPLTS >= 1000) {
    lastReadPLTS = currentMillis;
    readEpeverData();
  }

  // 4. Baca Data PLTB (INA219) setiap 500ms
  if (currentMillis - lastReadPLTB >= 500) {
    lastReadPLTB = currentMillis;
    readINA219Data();
  }

  // 5. Eksekusi Switching Relay & Publish MQTT + Debug Serial setiap 2000ms
  if (currentMillis - lastMqttPub >= 2000) {
    lastMqttPub = currentMillis;
    controlRelayLogic();
    if (mqttClient.connected()) {
      publishSensorData();
    }
    // Cetak Dashboard Debug ke Serial Monitor
    printSerialDashboard();
  }
}

// ============================================================
// MODBUS READ: EPEVER TRACER
// ============================================================
void readEpeverData() {
  uint8_t result;

  // Baca Register Tegangan/Arus PV & Baterai (0x3100 s/d 0x3105)
  result = epever.readInputRegisters(0x3100, 6);
  plts.lastErrorCode = result;
  
  if (result == epever.ku8MBSuccess) {
    plts.pvVoltage   = epever.getResponseBuffer(0x00) / 100.0f;
    plts.pvCurrent   = epever.getResponseBuffer(0x01) / 100.0f;
    plts.pvPower     = (epever.getResponseBuffer(0x02) | ((uint32_t)epever.getResponseBuffer(0x03) << 16)) / 100.0f;
    plts.battVoltage = epever.getResponseBuffer(0x04) / 100.0f;
    plts.battCurrent = epever.getResponseBuffer(0x05) / 100.0f;
    plts.isValid     = true;
  } else {
    plts.isValid     = false;
    return;
  }

  // Baca Register SOC & Suhu (0x3110 s/d 0x311A)
  result = epever.readInputRegisters(0x3110, 11);
  if (result == epever.ku8MBSuccess) {
    plts.battTemp = epever.getResponseBuffer(0x00) / 100.0f;
    plts.devTemp  = epever.getResponseBuffer(0x01) / 100.0f;
    plts.battSOC  = epever.getResponseBuffer(0x0A);
  }

  // Baca Register Statistik Energi (0x330C s/d 0x3313)
  result = epever.readInputRegisters(0x330C, 8);
  if (result == epever.ku8MBSuccess) {
    plts.energyToday = (epever.getResponseBuffer(0x06) | ((uint32_t)epever.getResponseBuffer(0x07) << 16)) / 100.0f;
    plts.energyTotal = (epever.getResponseBuffer(0x04) | ((uint32_t)epever.getResponseBuffer(0x05) << 16)) / 100.0f;
  }
}

// ============================================================
// I2C READ: INA219 (PLTB)
// ============================================================
void readINA219Data() {
  float busVoltage = ina219.getBusVoltage_V();
  float shuntVoltage = ina219.getShuntVoltage_mV() / 1000.0f;
  
  pltb.voltage = busVoltage + shuntVoltage;
  pltb.current = ina219.getCurrent_mA() / 1000.0f;
  if (pltb.current < 0) pltb.current = 0.0f; // Filter noise arus minus

  pltb.power   = pltb.voltage * pltb.current;
  pltb.isValid = true;
}

// ============================================================
// LOGIKA SWITCHING RELAY (SYARAT PEMBANGKIT >= 24V)
// ============================================================
void controlRelayLogic() {
  bool pltsSiap = (plts.pvVoltage >= 24.0f);
  bool pltbSiap = (pltb.voltage >= 24.0f);

  if (pltsSiap || pltbSiap) {
    if (pltsSiap && pltbSiap) {
      // Kedua sumber siap, bandingkan daya (Watt)
      if (plts.pvPower >= pltb.power) {
        digitalWrite(RELAY_PIN, RELAY_ON_PLTS);
        relayStatusStr = "PLTS (Aktif / Relay HIGH)";
        relayReasonStr = "Keduanya >= 24V, Daya PLTS (" + String(plts.pvPower, 2) + "W) >= PLTB (" + String(pltb.power, 2) + "W)";
      } else {
        digitalWrite(RELAY_PIN, RELAY_OFF_PLTB);
        relayStatusStr = "PLTB (Aktif / Relay LOW)";
        relayReasonStr = "Keduanya >= 24V, Daya PLTB (" + String(pltb.power, 2) + "W) > PLTS (" + String(plts.pvPower, 2) + "W)";
      }
    } 
    else if (pltsSiap) {
      digitalWrite(RELAY_PIN, RELAY_ON_PLTS);
      relayStatusStr = "PLTS (Aktif / Relay HIGH)";
      relayReasonStr = "Hanya PLTS siap (V >= 24V), PLTB = " + String(pltb.voltage, 2) + "V";
    } 
    else {
      digitalWrite(RELAY_PIN, RELAY_OFF_PLTB);
      relayStatusStr = "PLTB (Aktif / Relay LOW)";
      relayReasonStr = "Hanya PLTB siap (V >= 24V), PLTS = " + String(plts.pvVoltage, 2) + "V";
    }
  } 
  else {
    digitalWrite(RELAY_PIN, RELAY_OFF_PLTB);
    relayStatusStr = "STANDBY (Relay LOW)";
    relayReasonStr = "Tegangan kedua sumber < 24V (PLTS=" + String(plts.pvVoltage, 1) + "V, PLTB=" + String(pltb.voltage, 1) + "V)";
  }
}

// ============================================================
// PUBLISH DATA JSON VIA MQTT
// ============================================================
void publishSensorData() {
  // 1. JSON Payload PLTS
  StaticJsonDocument<400> docPLTS;
  docPLTS["device_id"]    = DEVICE_ID;
  docPLTS["system"]       = "PLTS";
  docPLTS["voltage"]      = plts.pvVoltage;
  docPLTS["current"]      = plts.pvCurrent;
  docPLTS["power"]        = plts.pvPower;
  docPLTS["pv_voltage"]   = plts.pvVoltage;
  docPLTS["pv_current"]   = plts.pvCurrent;
  docPLTS["pv_power"]     = plts.pvPower;
  docPLTS["batt_voltage"] = plts.battVoltage;
  docPLTS["batt_current"] = plts.battCurrent;
  docPLTS["batt_soc"]     = plts.battSOC;
  docPLTS["batt_temp"]    = plts.battTemp;
  docPLTS["dev_temp"]     = plts.devTemp;
  docPLTS["energy_today"] = plts.energyToday;
  docPLTS["energy_total"] = plts.energyTotal;

  char bufferPLTS[400];
  serializeJson(docPLTS, bufferPLTS);
  bool pubPLTS = mqttClient.publish(TOPIC_PLTS, bufferPLTS);

  // 2. JSON Payload PLTB
  StaticJsonDocument<200> docPLTB;
  docPLTB["device_id"] = DEVICE_ID;
  docPLTB["system"]    = "PLTB";
  docPLTB["voltage"]   = pltb.voltage;
  docPLTB["current"]   = pltb.current;
  docPLTB["power"]     = pltb.power;

  char bufferPLTB[200];
  serializeJson(docPLTB, bufferPLTB);
  bool pubPLTB = mqttClient.publish(TOPIC_PLTB, bufferPLTB);

  // 3. JSON Payload BATTERY
  StaticJsonDocument<200> docBAT;
  docBAT["device_id"] = DEVICE_ID;
  docBAT["system"]    = "BATTERY";
  docBAT["voltage"]   = plts.battVoltage;
  docBAT["current"]   = plts.battCurrent;
  docBAT["power"]     = plts.battVoltage * plts.battCurrent;
  docBAT["soc"]       = plts.battSOC;
  docBAT["temp"]      = plts.battTemp;

  char bufferBAT[200];
  serializeJson(docBAT, bufferBAT);
  bool pubBAT = mqttClient.publish(TOPIC_BATTERY, bufferBAT);

  // Log singkat status publish jika ada kegagalan
  if (!pubPLTS || !pubPLTB || !pubBAT) {
    Serial.println("[MQTT WARN] Satu atau lebih topik gagal dipublish!");
  }
}

// ============================================================
// SERIAL MONITOR DEBUG DASHBOARD
// ============================================================
void printSerialDashboard() {
  unsigned long uptimeSec = millis() / 1000;
  int minutes = (uptimeSec % 3600) / 60;
  int seconds = uptimeSec % 60;
  int hours   = uptimeSec / 3600;

  Serial.println("\n==============================================================");
  Serial.printf("[TELEMETRI] Uptime: %02d:%02d:%02d | WiFi: %d dBm | IP: %s\n", 
                hours, minutes, seconds, WiFi.RSSI(), WiFi.localIP().toString().c_str());
  Serial.printf("[MQTT STATUS] Broker: %s | Terhubung: %s\n", 
                MQTT_SERVER, mqttClient.connected() ? "YA (Connected)" : "TIDAK (Disconnected)");
  Serial.println("--------------------------------------------------------------");

  // PLTS Section
  Serial.println("[1. PLTS - TENAGA SURYA (Epever Tracer)]");
  if (plts.isValid) {
    Serial.printf("   • Panel Surya  : %6.2f V | %5.2f A | %7.2f W\n", plts.pvVoltage, plts.pvCurrent, plts.pvPower);
    Serial.printf("   • Baterai/Aki  : %6.2f V | %5.2f A | SOC: %3.0f%% | Suhu: %.1f °C\n", plts.battVoltage, plts.battCurrent, plts.battSOC, plts.battTemp);
    Serial.printf("   • Produksi     : Hari ini = %.2f kWh | Total = %.2f kWh\n", plts.energyToday, plts.energyTotal);
    Serial.println("   • Status RS485 : OK");
  } else {
    Serial.printf("   • Status RS485 : [GAGAL] %s\n", getModbusErrorStr(plts.lastErrorCode).c_str());
  }

  Serial.println("--------------------------------------------------------------");

  // PLTB Section
  Serial.println("[2. PLTB - TENAGA BAYU/ANGIN (INA219)]");
  if (pltb.isValid) {
    Serial.printf("   • Generator    : %6.2f V | %5.2f A | %7.2f W\n", pltb.voltage, pltb.current, pltb.power);
  } else {
    Serial.println("   • Status INA219: [TIDAK AKTIF / Belum Terdeteksi]");
  }

  Serial.println("--------------------------------------------------------------");

  // Relay Section
  Serial.println("[3. KONTROL RELAY SWITCHING AKI]");
  Serial.printf("   • Sumber Aktif : %s\n", relayStatusStr.c_str());
  Serial.printf("   • Alasan       : %s\n", relayReasonStr.c_str());
  Serial.println("==============================================================");
}

// ============================================================
// NETWORK HELPERS (AMAN & NON-BLOCKING)
// ============================================================
void setupWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.print("[WiFi] Menghubungkan ke: ");
  Serial.println(WIFI_SSID);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500);
    esp_task_wdt_reset();
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] ✓ Berhasil Terhubung!");
    Serial.print("[WiFi] IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.printf("[WiFi] Signal Strength (RSSI): %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("\n[WiFi] ✗ Gagal Terhubung, akan mencoba lagi nanti.");
  }
}

void reconnectMQTT() {
  if (millis() - lastMqttRetry >= 4000) {
    lastMqttRetry = millis();
    
    String clientId = "ESP32-Bangkit-" + String(random(0xffff), HEX);
    
    Serial.print("[MQTT] Menghubungkan ke Broker sebagai ID: ");
    Serial.println(clientId);
    
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("[MQTT] ✓ Berhasil Terhubung ke Broker!");
    } else {
      Serial.print("[MQTT] ✗ Gagal konek, State Error = ");
      Serial.println(mqttClient.state());
    }
  }
}