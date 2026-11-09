#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------- Chân kết nối ----------
#define TRIG_PIN   5
#define ECHO_PIN   18
#define ONEWIRE_PIN 4
#define PUMP_PIN   25   // LED xanh dương, PWM
#define VALVE_PIN  26   // LED xanh lá

// ---------- Cấu hình ----------
const char* WIFI_SSID = "Wokwi-GUEST";   // đổi nếu chạy trên mạch thật
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;
const char* P = "lab3/thuysan/";
const float TANK_H = 250.0;              // cm, chiều cao từ cảm biến đến đáy

// ---------- Đối tượng RTOS ----------
typedef struct { float level; float temp; } SensorData;
QueueHandle_t dataQueue, alertQueue;
SemaphoreHandle_t stateMutex;

// ---------- Trạng thái dùng chung (bảo vệ bằng stateMutex) ----------
int  pumpPct = 0;
bool valveOn = false;
bool manualMode = false;

WiFiClient espClient;
PubSubClient mqtt(espClient);
OneWire oneWire(ONEWIRE_PIN);
DallasTemperature ds18b20(&oneWire);

// ---------- Điều khiển phần cứng ----------
void pumpWrite(int pct) {
  int duty = map(pct, 0, 100, 0, 255);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PUMP_PIN, duty);
#else
  ledcWrite(0, duty);
#endif
}

// Đặt trạng thái và xuất ra chân (gọi khi đã giữ mutex)
void applyOutputs() {
  pumpWrite(pumpPct);
  digitalWrite(VALVE_PIN, valveOn ? HIGH : LOW);
}

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long us = pulseIn(ECHO_PIN, HIGH, 30000);   // timeout 30 ms
  if (us == 0) return -1;
  return us * 0.0343 / 2.0;
}

// ---------- Nhận lệnh từ Web (chạy trong mqtt.loop của TaskMQTT) ----------
void onMessage(char* topic, byte* payload, unsigned int len) {
  String t = topic, m;
  for (unsigned int i = 0; i < len; i++) m += (char)payload[i];
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (t == String(P) + "cmd/mode") {
    manualMode = (m == "MANUAL");
    if (manualMode) { pumpPct = 0; valveOn = false; applyOutputs(); }
  } else if (manualMode && t == String(P) + "cmd/pump") {
    pumpPct = (m == "ON") ? 100 : 0;
    applyOutputs();
  } else if (manualMode && t == String(P) + "cmd/valve") {
    valveOn = (m == "ON");
    applyOutputs();
  }
  xSemaphoreGive(stateMutex);
}

void pub(const char* sub, String v) {
  mqtt.publish((String(P) + sub).c_str(), v.c_str());
}

// ================= TASK TRUYỀN DATA - CORE 0 =================
void TaskMQTT(void* pv) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  unsigned long lastState = 0;

  for (;;) {
    // 1. Kết nối Wi-Fi
    if (WiFi.status() != WL_CONNECTED) {
      Serial.print("Connecting WiFi");
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
      }
      Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
    }
    // 2. Kết nối MQTT
    if (!mqtt.connected()) {
      String id = "esp32-aqua-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqtt.connect(id.c_str())) {
        mqtt.subscribe((String(P) + "cmd/#").c_str());
        Serial.println("MQTT OK");
      } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }
    mqtt.loop();

    // 3. Đọc dataQueue và gửi lên Broker
    SensorData d;
    if (xQueueReceive(dataQueue, &d, pdMS_TO_TICKS(100)) == pdTRUE) {
      pub("level", String(d.level, 1));
      pub("temp",  String(d.temp, 1));
    }

    // 4. Gửi trạng thái cơ cấu chấp hành mỗi 1 s
    if (millis() - lastState > 1000) {
      lastState = millis();
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      pub("pump",  String(pumpPct));
      pub("valve", valveOn ? "ON" : "OFF");
      pub("mode",  manualMode ? "MANUAL" : "AUTO");
      xSemaphoreGive(stateMutex);
    }
  }
}

// ================= TASK ĐỌC DATA - CORE 1 =================
void TaskRead(void* pv) {
  for (;;) {
    float dist = readDistanceCm();
    ds18b20.requestTemperatures();
    float temp = ds18b20.getTempCByIndex(0);

    if (dist >= 0) {
      float level = constrain(TANK_H - dist, 0, TANK_H);
      SensorData d = { level, temp };

      Serial.printf("[Read] Level: %.1f cm | Temp: %.1f C\n", level, temp);

      xQueueSend(dataQueue,  &d,     0);   // dữ liệu đầy đủ -> TaskMQTT
      xQueueSend(alertQueue, &level, 0);   // mực nước -> TaskAlert
    } else {
      Serial.println("[Read] HC-SR04 out of range");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ================= TASK CẢNH BÁO - CORE 1 =================
enum Action { IDLE, PUMP50, PUMP100, VALVE };

void TaskAlert(void* pv) {
  Action act = IDLE;
  unsigned long endAt = 0;
  float level;

  for (;;) {
    bool got = (xQueueReceive(alertQueue, &level, pdMS_TO_TICKS(200)) == pdTRUE);

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (manualMode) {                 // chế độ Manual: Web điều khiển
      act = IDLE;
      xSemaphoreGive(stateMutex);
      continue;
    }

    // Hết thời gian hành động -> tắt thiết bị
    if (act != IDLE && millis() >= endAt) {
      act = IDLE; pumpPct = 0; valveOn = false; applyOutputs();
      Serial.println("[Alert] Action finished");
    }

    // Đang rảnh và có dữ liệu mới -> chọn hành động
    if (act == IDLE && got) {
      if (level < 50) {
        act = VALVE;   valveOn = true;  endAt = millis() + 10000;
      } else if (level >= 100 && level < 150) {
        act = PUMP50;  pumpPct = 50;    endAt = millis() + 10000;
      } else if (level >= 150 && level <= 200) {
        act = PUMP100; pumpPct = 100;   endAt = millis() + 20000;
      }
      if (act != IDLE) {
        applyOutputs();
        Serial.printf("[Alert] Level %.1f cm -> action %d\n", level, act);
      }
    }
    xSemaphoreGive(stateMutex);
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(VALVE_PIN, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PUMP_PIN, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PUMP_PIN, 0);
#endif
  ds18b20.begin();

  dataQueue  = xQueueCreate(5, sizeof(SensorData));
  alertQueue = xQueueCreate(5, sizeof(float));
  stateMutex = xSemaphoreCreateMutex();

  //                         hàm      tên         stack  tham số  ưu tiên  handle  core
  xTaskCreatePinnedToCore(TaskMQTT,  "TaskMQTT",  8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskRead,  "TaskRead",  4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(TaskAlert, "TaskAlert", 4096, NULL, 2, NULL, 1);
}

void loop() { vTaskDelete(NULL); }   // loop() không dùng, mọi việc do các Task đảm nhiệm
