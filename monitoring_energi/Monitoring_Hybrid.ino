#include <WiFi.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>  // Proteksi Rollback OTA & Validasi Firmware
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

// Topic MQTT Publish Sensor & Relay
const char* TOPIC_PLTS    = "Bangkit/ESP32-001/sensor/PLTS";
const char* TOPIC_PLTB    = "Bangkit/ESP32-001/sensor/PLTB";
const char* TOPIC_BATTERY = "Bangkit/ESP32-001/sensor/BATTERY";

// Topic MQTT Subscribe Kontrol dari Web (/config)
const char* TOPIC_CONTROL_THRESHOLD = "Bangkit/ESP32-001/control/threshold";
const char* TOPIC_CONTROL_OTA       = "Bangkit/ESP32-001/control/ota";

// ============================================================
// CONFIGURATION: HARDWARE PINS & I2C
// ============================================================
// Dual Relay (Active HIGH)
#define RELAY1_PIN_PLTS 18   // Relay 1 -> PLTS (GPIO 18)
#define RELAY2_PIN_PLTB 19   // Relay 2 -> PLTB (GPIO 19)

#define RELAY_ACTIVE    HIGH // Active HIGH = Relay ON
#define RELAY_INACTIVE  LOW  // Active LOW = Relay OFF

// Pin RS485 MAX485
#define MAX485_DE       4    // DE/RE Pin Transceiver RS485
#define RX2_PIN         16   // ESP32 RX2 -> MAX485 RO
#define TX2_PIN         17   // ESP32 TX2 -> MAX485 DI

// Pin I2C ESP32
#define I2C_SDA         22   // SDA Pin I2C (INA219)
#define I2C_SCL         21   // SCL Pin I2C (INA219)
#define DS18B20_PIN     23   // Data DS18B20 (gunakan pull-up 4.7k ke 3.3V)

// I2C Address 2 Modul INA219
#define INA219_ADDR_PLTS 0x40 // Default tanpa jumper
#define INA219_ADDR_PLTB 0x41 // Jumper A0 terhubung/disolder

// ============================================================
// OBJECT INITIALIZATION
// ============================================================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
ModbusMaster epever;

Adafruit_INA219 ina_plts(INA219_ADDR_PLTS);
Adafruit_INA219 ina_pltb(INA219_ADDR_PLTB);
OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);

// Threshold Tegangan Dinamis (Default: 24.0V, dapat diubah realtime dari Web)
float voltageThreshold = 24.0f;
float loadTemperatureC = 0.0f;
bool loadTemperatureValid = false;

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
  bool  isValidRS485 = false;
  bool  isValidINA   = false;
  uint8_t lastErrorCode = 0;
} plts;

struct PLTB_Data {
  float voltage = 0.0;
  float current = 0.0;
  float power   = 0.0;
  bool  isValid = false;
} pltb;

// Status Relay: 0 = PLTS Aktif, 1 = PLTB Aktif, -1 = Standby
int currentRelayState = -1;
String relayStatusStr = "STANDBY";
String relayReasonStr = "Inisialisasi";

// OTA Update State
bool isUpdatingOTA = false;

// Non-blocking Timers
unsigned long lastReadPLTS  = 0;
unsigned long lastReadPLTB  = 0;
unsigned long lastReadTemp  = 0;
unsigned long lastMqttPub   = 0;
unsigned long lastMqttRetry = 0;

// Function Declarations
void preTransmission();
void postTransmission();
void setupWiFi();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void performHTTPOTA(String firmwareUrl);
void readEpeverData();
void readINAData();
void readLoadTemperature();
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
  Serial.println(" ESP32 MONITORING ENERGI HYBRID (DUAL INA + DUAL RELAY + OTA) ");
  Serial.println("==============================================================");
  Serial.printf("[INIT] Device ID    : %s\n", DEVICE_ID);
  Serial.printf("[INIT] Relay 1 PLTS : GPIO %d (Active HIGH)\n", RELAY1_PIN_PLTS);
  Serial.printf("[INIT] Relay 2 PLTB : GPIO %d (Active HIGH)\n", RELAY2_PIN_PLTB);
  Serial.printf("[INIT] RS485 Pins   : RX=%d, TX=%d, DE/RE=%d\n", RX2_PIN, TX2_PIN, MAX485_DE);
  Serial.printf("[INIT] I2C Pins     : SDA=GPIO %d, SCL=GPIO %d\n", I2C_SDA, I2C_SCL);
  Serial.printf("[INIT] INA Addr     : PLTS=0x%02X, PLTB=0x%02X\n", INA219_ADDR_PLTS, INA219_ADDR_PLTB);
  Serial.printf("[INIT] Default Thres: %.2f V\n", voltageThreshold);

  // 1. Setup Relay Pins (Keduanya default OFF / Standby)
  pinMode(RELAY1_PIN_PLTS, OUTPUT);
  pinMode(RELAY2_PIN_PLTB, OUTPUT);
  digitalWrite(RELAY1_PIN_PLTS, RELAY_INACTIVE);
  digitalWrite(RELAY2_PIN_PLTB, RELAY_INACTIVE);
  Serial.println("[INIT] Relay 1 & Relay 2 di-set STANDBY (LOW)");

  // 2. Setup RS485 & Serial2 Modbus (Timeout 200ms)
  pinMode(MAX485_DE, OUTPUT);
  digitalWrite(MAX485_DE, 0);
  Serial2.begin(115200, SERIAL_8N1, RX2_PIN, TX2_PIN);
  Serial2.setTimeout(200); 
  epever.begin(1, Serial2);
  epever.preTransmission(preTransmission);
  epever.postTransmission(postTransmission);
  Serial.println("[OK] Modbus RS485 Epever siap.");

  // 3. Setup I2C & Dual Sensor INA219
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!ina_plts.begin(&Wire)) {
    Serial.printf("[WARN] Sensor INA219 PLTS (0x%02X) tidak terdeteksi!\n", INA219_ADDR_PLTS);
    plts.isValidINA = false;
  } else {
    ina_plts.setCalibration_32V_2A();
    plts.isValidINA = true;
    Serial.printf("[OK] Sensor INA219 PLTS (0x%02X) berhasil inisialisasi.\n", INA219_ADDR_PLTS);
  }

  if (!ina_pltb.begin(&Wire)) {
    Serial.printf("[WARN] Sensor INA219 PLTB (0x%02X) tidak terdeteksi!\n", INA219_ADDR_PLTB);
    pltb.isValid = false;
  } else {
    ina_pltb.setCalibration_32V_2A();
    pltb.isValid = true;
    Serial.printf("[OK] Sensor INA219 PLTB (0x%02X) berhasil inisialisasi.\n", INA219_ADDR_PLTB);
  }

  // 4. Inisialisasi DS18B20 (GPIO23)
  temperatureSensor.begin();
  temperatureSensor.setWaitForConversion(false);
  temperatureSensor.requestTemperatures();
  Serial.printf("[INIT] DS18B20 siap di GPIO %d (%d sensor terdeteksi).\n",
                DS18B20_PIN, temperatureSensor.getDeviceCount());

  // 5. Inisialisasi Watchdog Timer (WDT)
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
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);

  Serial.println("==============================================================");
  Serial.println("[SUCCESS] Setup Selesai! Memulai pemantauan...");
  
  // Konfirmasi ke Bootloader ESP32 bahwa firmware ini valid & berfungsi normal (Batalkan Rollback)
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.println("[OTA OK] Firmware ditandai VALID. Sistem boot aman.");
  Serial.println("==============================================================\n");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  // Jika sedang flashing OTA firmware, abaikan pembacaan sensor
  if (isUpdatingOTA) {
    esp_task_wdt_reset();
    delay(100);
    return;
  }

  // 1. Reset Watchdog Timer di setiap iterasi loop
  esp_task_wdt_reset();

  // 2. Jaga Koneksi Jaringan & MQTT Loop
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFi();
  } else if (!mqttClient.connected()) {
    reconnectMQTT();
  } else {
    mqttClient.loop();
  }

  unsigned long currentMillis = millis();

  // 3. Baca Data PLTS (Epever RS485) setiap 1000ms
  if (currentMillis - lastReadPLTS >= 1000) {
    lastReadPLTS = currentMillis;
    readEpeverData();
  }

  // 4. Baca Data Sensor INA219 (PLTS & PLTB) setiap 500ms
  if (currentMillis - lastReadPLTB >= 500) {
    lastReadPLTB = currentMillis;
    readINAData();
  }

  // 5. Baca suhu DS18B20 tanpa menunggu konversi.
  if (currentMillis - lastReadTemp >= 1000) {
    lastReadTemp = currentMillis;
    readLoadTemperature();
  }

  // 6. Eksekusi Switching Relay & Publish MQTT + Debug Serial setiap 2000ms
  if (currentMillis - lastMqttPub >= 2000) {
    lastMqttPub = currentMillis;
    controlRelayLogic();
    if (mqttClient.connected()) {
      publishSensorData();
    }
    printSerialDashboard();
  }
}

// ============================================================
// MQTT CALLBACK: MENERIMA THRESHOLD & TRIGGER OTA DARI WEBSITE
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[length + 1];
  for (unsigned int i = 0; i < length; i++) {
    msg[i] = (char)payload[i];
  }
  msg[length] = '\0';

  Serial.printf("\n[MQTT INCOMING] Topik: %s | Pesan: %s\n", topic, msg);

  // A. KONTROL THRESHOLD TEGANGAN
  if (String(topic) == TOPIC_CONTROL_THRESHOLD) {
    StaticJsonDocument<128> doc;
    DeserializationError error = deserializeJson(doc, msg);
    if (!error) {
      if (doc.containsKey("threshold")) {
        float newTh = doc["threshold"];
        if (newTh >= 0) {
          voltageThreshold = newTh;
          Serial.printf("[KONTROL] Ambang Batas Tegangan Diperbarui: %.2f V\n", voltageThreshold);
        }
      }
    } else {
      float directVal = atof(msg);
      if (directVal > 0) {
        voltageThreshold = directVal;
        Serial.printf("[KONTROL] Ambang Batas Tegangan Diperbarui (Plain): %.2f V\n", voltageThreshold);
      }
    }
  }

  // B. KONTROL OTA UPDATE DARI JARAK JAUH DENGAN VERIFIKASI MD5
  else if (String(topic) == TOPIC_CONTROL_OTA) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, msg);
    if (!error) {
      String action = doc["action"] | "";
      String firmwareUrl = doc["url"] | "";
      String expectedMD5 = doc["md5"] | "";

      if (action == "UPDATE_OTA" && firmwareUrl.length() > 0) {
        Serial.println("\n[OTA] >>> PERINTAH OTA DITERIMA DARI WEBSITE <<<");
        Serial.printf("[OTA] URL Firmware   : %s\n", firmwareUrl.c_str());
        Serial.printf("[OTA] Checksum MD5   : %s\n", expectedMD5.length() > 0 ? expectedMD5.c_str() : "(Tidak ada)");
        performHTTPOTA(firmwareUrl, expectedMD5);
      }
    }
  }
}

// ============================================================
// FUNGSI EKSEKUSI OTA UPDATE HTTP DENGAN VERIFIKASI MD5
// Mengunduh firmware .bin dari server Django, memeriksa MD5, dan mem-flash partisi OTA ESP32
// ============================================================
void performHTTPOTA(String firmwareUrl, String expectedMD5) {
  isUpdatingOTA = true;

  // Matikan kedua relay agar aman selama flashing firmware
  digitalWrite(RELAY1_PIN_PLTS, RELAY_INACTIVE);
  digitalWrite(RELAY2_PIN_PLTB, RELAY_INACTIVE);

  Serial.println("[OTA] Menghubungkan ke server untuk unduh firmware...");
  HTTPClient http;
  http.begin(firmwareUrl);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[OTA ERROR] Server merespon kode: %d (Gagal unduh)\n", httpCode);
    http.end();
    isUpdatingOTA = false;
    return;
  }

  int contentLength = http.getSize();
  Serial.printf("[OTA] Ukuran file firmware: %d bytes\n", contentLength);

  if (contentLength <= 0) {
    Serial.println("[OTA ERROR] Ukuran file tidak valid (0 bytes). Dibatalkan.");
    http.end();
    isUpdatingOTA = false;
    return;
  }

  // 1. Inisialisasi Update Partition
  bool canBegin = Update.begin(contentLength);
  if (!canBegin) {
    Serial.printf("[OTA ERROR] Ruang partisi OTA tidak cukup! Error #: %u\n", Update.getError());
    http.end();
    isUpdatingOTA = false;
    return;
  }

  // 2. Set Verifikasi MD5 jika disediakan oleh server
  if (expectedMD5.length() == 32) {
    Update.setMD5(expectedMD5.c_str());
    Serial.printf("[OTA] Verifikasi MD5 aktif: %s\n", expectedMD5.c_str());
  } else {
    Serial.println("[OTA WARN] MD5 tidak disediakan, verifikasi dilewati.");
  }

  Serial.println("[OTA] Sedang menulis firmware ke partisi flash...");
  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buff[1024];

  while (http.connected() && (written < contentLength)) {
    esp_task_wdt_reset(); // Reset WDT agar tidak timeout saat download
    size_t sizeAvailable = stream->available();
    if (sizeAvailable) {
      int readBytes = stream->readBytes(buff, ((sizeAvailable > sizeof(buff)) ? sizeof(buff) : sizeAvailable));
      Update.write(buff, readBytes);
      written += readBytes;
      
      // Log progress per 15%
      if (written % (contentLength / 7 + 1) == 0) {
        Serial.printf("[OTA PROGRESS] %d%% (%u / %d bytes)\n", (written * 100) / contentLength, written, contentLength);
      }
    }
    delay(1);
  }

  // 3. Verifikasi Akhir Integritas File & MD5
  if (Update.end()) {
    if (Update.isFinished()) {
      Serial.println("\n==============================================================");
      Serial.println("[OTA SUCCESS] FIRMWARE 100% VALID & INTEGRITAS COCOK!");
      Serial.println("[OTA REBOOT] Me-restart ESP32 untuk memuat firmware baru...");
      Serial.println("==============================================================\n");
      delay(1500);
      ESP.restart();
    } else {
      Serial.println("[OTA ERROR] File tidak selesai terunduh (Incomplete). Firmware lama tetap aman!");
    }
  } else {
    Serial.printf("[OTA ERROR] Verifikasi Gagal! (Kemungkinan File Corrupt/MD5 Beda). Error #: %u\n", Update.getError());
    Serial.println("[OTA SAFEGUARD] Partisi lama TIDAK diubah. ESP32 tetap berjalan normal.");
  }

  http.end();
  isUpdatingOTA = false;
}

// ============================================================
// MODBUS READ: EPEVER TRACER (RS485)
// ============================================================
void readEpeverData() {
  uint8_t result;

  // Baca Register Tegangan/Arus PV & Baterai (0x3100 s/d 0x3105)
  result = epever.readInputRegisters(0x3100, 6);
  plts.lastErrorCode = result;
  
  if (result == epever.ku8MBSuccess) {
    if (!plts.isValidINA) {
      plts.pvVoltage = epever.getResponseBuffer(0x00) / 100.0f;
      plts.pvCurrent = epever.getResponseBuffer(0x01) / 100.0f;
      plts.pvPower   = (epever.getResponseBuffer(0x02) | ((uint32_t)epever.getResponseBuffer(0x03) << 16)) / 100.0f;
    }
    plts.battVoltage = epever.getResponseBuffer(0x04) / 100.0f;
    plts.battCurrent = epever.getResponseBuffer(0x05) / 100.0f;
    plts.isValidRS485 = true;
  } else {
    plts.isValidRS485 = false;
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
    plts.energyTotal = (epever.getResponseBuffer(0x02) | ((uint32_t)epever.getResponseBuffer(0x03) << 16)) / 100.0f;
  }
}

// ============================================================
// READ DUAL INA219 (PLTS & PLTB)
// ============================================================
void readINAData() {
  if (plts.isValidINA) {
    float busV   = ina_plts.getBusVoltage_V();
    float shuntV = ina_plts.getShuntVoltage_mV() / 1000.0f;
    float currentA = ina_plts.getCurrent_mA() / 1000.0f;
    if (currentA < 0) currentA = 0.0f;

    plts.pvVoltage = busV + shuntV;
    plts.pvCurrent = currentA;
    plts.pvPower   = plts.pvVoltage * plts.pvCurrent;
  }

  if (pltb.isValid) {
    float busV   = ina_pltb.getBusVoltage_V();
    float shuntV = ina_pltb.getShuntVoltage_mV() / 1000.0f;
    float currentA = ina_pltb.getCurrent_mA() / 1000.0f;
    if (currentA < 0) currentA = 0.0f;

    pltb.voltage = busV + shuntV;
    pltb.current = currentA;
    pltb.power   = pltb.voltage * pltb.current;
  }
}

void readLoadTemperature() {
  float temperatureC = temperatureSensor.getTempCByIndex(0);
  loadTemperatureValid = temperatureC != DEVICE_DISCONNECTED_C &&
                         temperatureC >= -55.0f &&
                         temperatureC <= 125.0f;
  if (loadTemperatureValid) {
    loadTemperatureC = temperatureC;
  } else {
    Serial.println("[WARN] Pembacaan DS18B20 gagal atau sensor tidak terhubung.");
  }
  temperatureSensor.requestTemperatures();
}

// ============================================================
// LOGIKA SWITCHING DUAL RELAY
// ============================================================
void controlRelayLogic() {
  bool pltsMelebihi = (plts.pvVoltage >= voltageThreshold);
  bool pltbMelebihi = (pltb.voltage >= voltageThreshold);

  if (pltsMelebihi || pltbMelebihi) {
    if (pltsMelebihi && pltbMelebihi) {
      if (plts.pvVoltage >= pltb.voltage) {
        digitalWrite(RELAY1_PIN_PLTS, RELAY_ACTIVE);
        digitalWrite(RELAY2_PIN_PLTB, RELAY_INACTIVE);
        currentRelayState = 0; // 0 = PLTS
        relayStatusStr = "PLTS AKTIF (Relay 1 ON / State 0)";
        relayReasonStr = "Keduanya >= " + String(voltageThreshold, 1) + "V, Tegangan PLTS (" + String(plts.pvVoltage, 2) + "V) >= PLTB (" + String(pltb.voltage, 2) + "V)";
      } else {
        digitalWrite(RELAY1_PIN_PLTS, RELAY_INACTIVE);
        digitalWrite(RELAY2_PIN_PLTB, RELAY_ACTIVE);
        currentRelayState = 1; // 1 = PLTB
        relayStatusStr = "PLTB AKTIF (Relay 2 ON / State 1)";
        relayReasonStr = "Keduanya >= " + String(voltageThreshold, 1) + "V, Tegangan PLTB (" + String(pltb.voltage, 2) + "V) > PLTS (" + String(plts.pvVoltage, 2) + "V)";
      }
    } 
    else if (pltsMelebihi) {
      digitalWrite(RELAY1_PIN_PLTS, RELAY_ACTIVE);
      digitalWrite(RELAY2_PIN_PLTB, RELAY_INACTIVE);
      currentRelayState = 0; // 0 = PLTS
      relayStatusStr = "PLTS AKTIF (Relay 1 ON / State 0)";
      relayReasonStr = "Hanya PLTS >= " + String(voltageThreshold, 1) + "V (PLTS=" + String(plts.pvVoltage, 2) + "V, PLTB=" + String(pltb.voltage, 2) + "V)";
    } 
    else {
      digitalWrite(RELAY1_PIN_PLTS, RELAY_INACTIVE);
      digitalWrite(RELAY2_PIN_PLTB, RELAY_ACTIVE);
      currentRelayState = 1; // 1 = PLTB
      relayStatusStr = "PLTB AKTIF (Relay 2 ON / State 1)";
      relayReasonStr = "Hanya PLTB >= " + String(voltageThreshold, 1) + "V (PLTB=" + String(pltb.voltage, 2) + "V, PLTS=" + String(plts.pvVoltage, 2) + "V)";
    }
  } 
  else {
    digitalWrite(RELAY1_PIN_PLTS, RELAY_INACTIVE);
    digitalWrite(RELAY2_PIN_PLTB, RELAY_INACTIVE);
    currentRelayState = -1; // Standby
    relayStatusStr = "STANDBY (Keduanya OFF)";
    relayReasonStr = "Kedua sumber < " + String(voltageThreshold, 1) + "V (PLTS=" + String(plts.pvVoltage, 2) + "V, PLTB=" + String(pltb.voltage, 2) + "V)";
  }
}

// ============================================================
// PUBLISH DATA JSON VIA MQTT
// ============================================================
void publishSensorData() {
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
  if (loadTemperatureValid) {
    docPLTS["suhu_beban"] = loadTemperatureC;
  } else {
    docPLTS["suhu_beban"] = nullptr;
  }
  docPLTS["energy_today"] = plts.energyToday;
  docPLTS["energy_total"] = plts.energyTotal;
  docPLTS["relay"]        = currentRelayState;
  docPLTS["threshold"]    = voltageThreshold;

  char bufferPLTS[400];
  serializeJson(docPLTS, bufferPLTS);
  bool pubPLTS = mqttClient.publish(TOPIC_PLTS, bufferPLTS);

  StaticJsonDocument<256> docPLTB;
  docPLTB["device_id"] = DEVICE_ID;
  docPLTB["system"]    = "PLTB";
  docPLTB["voltage"]   = pltb.voltage;
  docPLTB["current"]   = pltb.current;
  docPLTB["power"]     = pltb.power;
  docPLTB["relay"]     = currentRelayState;
  docPLTB["threshold"] = voltageThreshold;

  char bufferPLTB[256];
  serializeJson(docPLTB, bufferPLTB);
  bool pubPLTB = mqttClient.publish(TOPIC_PLTB, bufferPLTB);

  StaticJsonDocument<256> docBAT;
  docBAT["device_id"] = DEVICE_ID;
  docBAT["system"]    = "BATTERY";
  docBAT["voltage"]   = plts.battVoltage;
  docBAT["current"]   = plts.battCurrent;
  docBAT["power"]     = plts.battVoltage * plts.battCurrent;
  docBAT["soc"]       = plts.battSOC;
  docBAT["temp"]      = plts.battTemp;
  docBAT["relay"]     = currentRelayState;

  char bufferBAT[256];
  serializeJson(docBAT, bufferBAT);
  bool pubBAT = mqttClient.publish(TOPIC_BATTERY, bufferBAT);

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
  Serial.printf("[THRESHOLD AKTIF] %.2f V\n", voltageThreshold);
  Serial.println("--------------------------------------------------------------");

  // PLTS Section
  Serial.println("[1. PLTS - TENAGA SURYA]");
  Serial.printf("   • Panel Surya  : %6.2f V | %5.2f A | %7.2f W (INA: %s, RS485: %s)\n", 
                plts.pvVoltage, plts.pvCurrent, plts.pvPower, 
                plts.isValidINA ? "OK" : "NO", plts.isValidRS485 ? "OK" : "NO");
  Serial.printf("   • Baterai/Aki  : %6.2f V | %5.2f A | SOC: %3.0f%% | Suhu: %.1f °C\n", 
                plts.battVoltage, plts.battCurrent, plts.battSOC, plts.battTemp);
  Serial.printf("   • Produksi     : Hari ini = %.2f kWh | Total = %.2f kWh\n", 
                plts.energyToday, plts.energyTotal);

  Serial.println("--------------------------------------------------------------");

  // PLTB Section
  Serial.println("[2. PLTB - TENAGA BAYU/ANGIN (INA219 0x41)]");
  if (pltb.isValid) {
    Serial.printf("   • Generator    : %6.2f V | %5.2f A | %7.2f W\n", pltb.voltage, pltb.current, pltb.power);
  } else {
    Serial.println("   • Status INA219: [TIDAK AKTIF / Address 0x41 Belum Terdeteksi]");
  }

  Serial.println("--------------------------------------------------------------");

  // Dual Relay Section
  Serial.println("[3. KONTROL DUAL RELAY ATS]");
  Serial.printf("   • Relay 1 (PLTS - GPIO %d) : %s\n", RELAY1_PIN_PLTS, (currentRelayState == 0) ? "ON (HIGH)" : "OFF (LOW)");
  Serial.printf("   • Relay 2 (PLTB - GPIO %d) : %s\n", RELAY2_PIN_PLTB, (currentRelayState == 1) ? "ON (HIGH)" : "OFF (LOW)");
  Serial.printf("   • State Relay Terkirim    : %d (%s)\n", currentRelayState, relayStatusStr.c_str());
  Serial.printf("   • Alasan Keputusan        : %s\n", relayReasonStr.c_str());
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
      // 1. Subscribe ke kontrol threshold
      mqttClient.subscribe(TOPIC_CONTROL_THRESHOLD);
      Serial.printf("[MQTT] ✓ Subscribe ke: %s\n", TOPIC_CONTROL_THRESHOLD);

      // 2. Subscribe ke kontrol OTA Update
      mqttClient.subscribe(TOPIC_CONTROL_OTA);
      Serial.printf("[MQTT] ✓ Subscribe ke: %s\n", TOPIC_CONTROL_OTA);
    } else {
      Serial.print("[MQTT] ✗ Gagal konek, State Error = ");
      Serial.println(mqttClient.state());
    }
  }
}