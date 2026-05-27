#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <driver/gpio.h>
#include <esp_mac.h>
#include <esp_sleep.h>

namespace {

/**
 * 引脚定义 - 匹配硬件电路设计
 */
constexpr uint8_t kMuxAdcPin = 35;       // CD74HC4051 复用器输出连接到 ESP32 的采样引脚
constexpr uint8_t kMuxS0Pin = 32;        // 复用器地址位 0
constexpr uint8_t kMuxS1Pin = 33;        // 复用器地址位 1
constexpr uint8_t kMuxS2Pin = 27;        // 复用器地址位 2
constexpr uint8_t kFsrEnablePin = 14;    // 传感器电源总开关
constexpr uint8_t kPowerKeyPin = 13;     // 板载电源/多功能按键
constexpr uint8_t kBoostEnablePin = 21;  // 升压电路使能
constexpr uint8_t kAmpShutdownPin = 5;   // 放大器休眠控制
constexpr uint8_t kVbusDetPin = 23;      // USB 充电检测引脚
constexpr uint8_t kLedRedPin = 17;       // RGB LED 红灯
constexpr uint8_t kLedGreenPin = 18;     // RGB LED 绿灯
constexpr uint8_t kLedBluePin = 19;      // RGB LED 蓝灯

/**
 * 系统参数与阈值
 */
constexpr uint8_t kFsrPowerActiveLevel = LOW; 
constexpr bool kRgbLedActiveLow = true;       
constexpr uint16_t kTriggerThreshold = 3000;  
constexpr uint32_t kSampleIntervalMs = 80;    
constexpr uint32_t kPowerKeyLongPressMs = 2000; 
constexpr uint32_t kAdvertisingRestartDelayMs = 500; 
constexpr uint32_t kAdvertisingWatchdogMs = 30000;   
constexpr uint32_t kNotifyTimeoutMs = 3000;     

constexpr uint8_t kPayloadHeader = 0xA5;      
constexpr uint8_t kLedRedChannel = 0;         
constexpr uint8_t kLedGreenChannel = 1;
constexpr uint8_t kLedBlueChannel = 2;
constexpr uint16_t kLedPwmFrequency = 5000;
constexpr uint8_t kLedPwmResolution = 8;

constexpr char kDeviceNamePrefix[] = "FSR-MAT";
constexpr char kServiceUuid[] = "2f3a0001-3c18-4b22-9c70-364d4f535200";
constexpr char kNotifyUuid[] = "2f3a0002-3c18-4b22-9c70-364d4f535200";
constexpr char kControlUuid[] = "2f3a0003-3c18-4b22-9c70-364d4f535200";

struct FsrSensor {
  const char *key;
  const char *label;
  uint8_t muxChannel;
};

constexpr FsrSensor kSensors[] = {
    {"HEAD", "Head", 3},       {"HAND_R", "Right hand", 0},
    {"HAND_L", "Left hand", 4}, {"KNEE_R", "Right knee", 1},
    {"KNEE_L", "Left knee", 6}, {"FOOT_R", "Right foot", 2},
    {"FOOT_L", "Left foot", 5},
};
constexpr size_t kSensorCount = sizeof(kSensors) / sizeof(kSensors[0]);

BLEServer *bleServer = nullptr;
BLECharacteristic *notifyCharacteristic = nullptr;
BLECharacteristic *controlCharacteristic = nullptr;

bool bleStarted = false;          
bool bleClientConnected = false;  
bool hasActiveConnId = false;     
uint16_t activeConnId = 0;        
volatile bool disconnectRequested = false; 

uint16_t adcValues[kSensorCount] = {}; 
uint32_t lastSampleAt = 0;             
uint32_t lastNotifySuccessAt = 0;      
uint32_t powerKeyPressedAt = 0;        
uint32_t advertisingRestartAt = 0;     
uint32_t lastAdvertisingKickAt = 0;    
char bleDeviceName[16] = {};           

// 函数预声明
bool isPowerKeyPressed();
void setRgb(uint8_t red, uint8_t green, uint8_t blue);
void setPeripheralPowerOff();

/**
 * 进入 ESP32 深度睡眠
 */
void enterDeepSleep() {
  Serial.println("[PWR] Entering deep sleep...");
  
  // 1. 先关闭所有可见和耗电的功能
  if (bleStarted) {
    BLEDevice::getAdvertising()->stop();
    BLEDevice::deinit(true);
    bleStarted = false;
  }
  setPeripheralPowerOff();
  setRgb(0, 0, 0);

  // 2. 关键修复：等待用户松开按键。防止进入睡眠瞬间又被当前的低电平唤醒。
  while (isPowerKeyPressed()) {
    delay(10);
  }
  delay(100);

  // 3. 配置唤醒源并开始深度睡眠
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(kPowerKeyPin), 0); // 0 = 低电平（按下）唤醒
  esp_deep_sleep_start();
}

/**
 * 蓝牙服务器回调类：处理连接与断开事件
 */
class ServerCallbacks final : public BLEServerCallbacks {
  void onConnect(BLEServer *server, esp_ble_gatts_cb_param_t *param) override {
    bleClientConnected = true;
    hasActiveConnId = true;
    activeConnId = param->connect.conn_id;
    lastNotifySuccessAt = millis();
    Serial.print("[BLE] Connected, session id: ");
    Serial.println(activeConnId);
  }

  void onDisconnect(BLEServer *server, esp_ble_gatts_cb_param_t *param) override {
    bleClientConnected = false;
    hasActiveConnId = false;
    disconnectRequested = false;
    activeConnId = 0;
    advertisingRestartAt = millis() + kAdvertisingRestartDelayMs;
    Serial.println("[BLE] Disconnected, preparing to restart advertising...");
  }
};

/**
 * 蓝牙控制特征回调类：处理 App 下发的指令
 */
class ControlCallbacks final : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
    const std::string value = characteristic->getValue();
    if (value.empty()) return;
    if (value[0] == 'D' || value[0] == 'd' || value[0] == 0x44) {
      disconnectRequested = true;
      bleClientConnected = false; 
      Serial.println("[BLE] Disconnect command received");
    }
  }
};

/**
 * 切换 CD74HC4051 的选通通道
 */
void selectMuxChannel(uint8_t channel) {
  digitalWrite(kMuxS0Pin, channel & 0x01);
  digitalWrite(kMuxS1Pin, (channel >> 1) & 0x01);
  digitalWrite(kMuxS2Pin, (channel >> 2) & 0x01);
  delayMicroseconds(60); 
}

/**
 * 读取复用器指定通道的 ADC 值
 */
uint16_t readMuxAdc(uint8_t channel) {
  selectMuxChannel(channel);
  uint32_t total = 0;
  constexpr uint8_t samples = 8;
  for (uint8_t i = 0; i < samples; ++i) {
    total += analogRead(kMuxAdcPin);
    delayMicroseconds(120);
  }
  return static_cast<uint16_t>(total / samples);
}

/**
 * 循环采样所有传感器并更新掩码
 */
uint8_t sampleSensors() {
  uint8_t triggerMask = 0;
  for (size_t i = 0; i < kSensorCount; ++i) {
    adcValues[i] = readMuxAdc(kSensors[i].muxChannel);
    if (adcValues[i] < kTriggerThreshold) {
      triggerMask |= (1U << i);
    }
  }
  return triggerMask;
}

/**
 * 串口打印采样结果（调试用）
 */
void printSample(uint8_t triggerMask) {
  Serial.printf("[FSR] Mask=0x%02X ", triggerMask);
  for (size_t i = 0; i < kSensorCount; ++i) {
    Serial.printf("%s=%u ", kSensors[i].key, adcValues[i]);
  }
  Serial.println();
}

/**
 * 将采样数据通过蓝牙 Notify 发送给 App
 */
void publishBleSample(uint8_t triggerMask) {
  if (notifyCharacteristic == nullptr || !bleClientConnected) return;

  uint8_t payload[2 + kSensorCount * 2] = {};
  payload[0] = kPayloadHeader;
  payload[1] = triggerMask;

  for (size_t i = 0; i < kSensorCount; ++i) {
    const size_t offset = 2 + i * 2;
    payload[offset] = adcValues[i] & 0xFF;        
    payload[offset + 1] = (adcValues[i] >> 8) & 0xFF; 
  }

  notifyCharacteristic->setValue(payload, sizeof(payload));
  notifyCharacteristic->notify();
  lastNotifySuccessAt = millis();
}

// 基础 IO 状态获取
bool isPowerKeyPressed() { return digitalRead(kPowerKeyPin) == LOW; }
bool isCharging() { return digitalRead(kVbusDetPin) == HIGH; }

uint8_t ledDuty(uint8_t brightness) {
  return kRgbLedActiveLow ? 255 - brightness : brightness;
}

/**
 * 设置 RGB LED 颜色
 */
void setRgb(uint8_t red, uint8_t green, uint8_t blue) {
  ledcWrite(kLedRedChannel, ledDuty(red));
  ledcWrite(kLedGreenChannel, ledDuty(green));
  ledcWrite(kLedBlueChannel, ledDuty(blue));
}

/**
 * 计算呼吸灯当前的亮度值
 */
uint8_t breatheValue(uint32_t now, uint32_t periodMs, uint8_t minValue, uint8_t maxValue) {
  const uint32_t phase = now % periodMs;
  const uint32_t halfPeriod = periodMs / 2;
  const uint32_t ramp = phase < halfPeriod ? phase : periodMs - phase;
  return minValue + (ramp * (maxValue - minValue)) / halfPeriod;
}

/**
 * 更新状态指示灯逻辑
 */
void updateStatusLed(uint32_t now) {
  if (!bleClientConnected) {
    const uint8_t blue = breatheValue(now, 1800, 12, 180);
    setRgb(0, 0, blue);
    return;
  }
  if (isCharging()) {
    const uint8_t green = breatheValue(now, 1100, 70, 255);
    setRgb(0, green, 0);
    return;
  }
  setRgb(0, 0, 120);
}

void startAdvertising() {
  if (!bleStarted || bleClientConnected) return;
  BLEDevice::getAdvertising()->stop();
  delay(10);
  BLEDevice::startAdvertising();
  lastAdvertisingKickAt = millis();
  Serial.println("[BLE] Advertising started");
}

void updateAdvertising(uint32_t now) {
  if (bleClientConnected && (now - lastNotifySuccessAt > kNotifyTimeoutMs)) {
    Serial.println("[BLE] Notify timeout - killing session");
    disconnectRequested = true; 
    bleClientConnected = false; 
  }
  if (bleClientConnected) return;
  if (advertisingRestartAt != 0 && static_cast<int32_t>(now - advertisingRestartAt) >= 0) {
    advertisingRestartAt = 0;
    startAdvertising();
    return;
  }
  if (now - lastAdvertisingKickAt >= kAdvertisingWatchdogMs) {
    startAdvertising();
  }
}

void handleBleControl() {
  if (!disconnectRequested) return;
  if (bleServer != nullptr && hasActiveConnId) {
    const uint16_t connId = activeConnId;
    Serial.print("[BLE] Server executing disconnect for conn=");
    Serial.println(connId);
    bleServer->disconnect(connId);
    bleClientConnected = false;
    hasActiveConnId = false;
    activeConnId = 0;
  }
  disconnectRequested = false;
}

void setupRgbLed() {
  // 1. 初始化 PWM 定时器配置
  ledcSetup(kLedRedChannel, kLedPwmFrequency, kLedPwmResolution);
  ledcSetup(kLedGreenChannel, kLedPwmFrequency, kLedPwmResolution);
  ledcSetup(kLedBlueChannel, kLedPwmFrequency, kLedPwmResolution);

  // 2. 关键优化：在把引脚绑定到 PWM 通道之前，先预设好“全灭”的占空比。
  // 这样当 ledcAttachPin 执行的瞬间，输出电平就是我们期望的关闭状态。
  ledcWrite(kLedRedChannel, ledDuty(0));
  ledcWrite(kLedGreenChannel, ledDuty(0));
  ledcWrite(kLedBlueChannel, ledDuty(0));

  // 3. 此时再让 PWM 外设接管物理引脚
  ledcAttachPin(kLedRedPin, kLedRedChannel);
  ledcAttachPin(kLedGreenPin, kLedGreenChannel);
  ledcAttachPin(kLedBluePin, kLedBlueChannel);
}

void setPeripheralPowerOff() {
  digitalWrite(kFsrEnablePin, !kFsrPowerActiveLevel);
  digitalWrite(kBoostEnablePin, LOW);
  digitalWrite(kAmpShutdownPin, LOW);
  digitalWrite(kMuxS0Pin, LOW);
  digitalWrite(kMuxS1Pin, LOW);
  digitalWrite(kMuxS2Pin, LOW);
}

/**
 * 唤醒检测：如果是按键唤醒，必须按住 2 秒才开机，否则继续睡（防误触）
 */
void confirmWakeFromPowerKey() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT0) return;

  Serial.println("[PWR] Wake up by power key, confirming...");

  const uint32_t startedAt = millis();
  while (millis() - startedAt < kPowerKeyLongPressMs) {
    if (!isPowerKeyPressed()) {
      Serial.println("[PWR] Confirm failed, going back to sleep");
      enterDeepSleep();
    }
    delay(10);
  }
  Serial.println("[PWR] Confirm success, booting up...");
}

void updatePowerKey(uint32_t now) {
  if (!isPowerKeyPressed()) {
    powerKeyPressedAt = 0;
    return;
  }
  if (powerKeyPressedAt == 0) {
    powerKeyPressedAt = now;
    return;
  }
  if (now - powerKeyPressedAt >= kPowerKeyLongPressMs) {
    enterDeepSleep();
  }
}

void setupPins() {
  // 1. 立即清理 Deep Sleep 保持状态
  gpio_deep_sleep_hold_dis();

  // 2. 核心防御：在 PWM 初始化之前，先通过普通 GPIO 模式强制将引脚设为灭灯电平。
  // 这里的 ledDuty(0) 会根据是共阳极还是共阴极自动返回 HIGH 或 LOW。
  pinMode(kLedRedPin, OUTPUT);
  pinMode(kLedGreenPin, OUTPUT);
  pinMode(kLedBluePin, OUTPUT);
  digitalWrite(kLedRedPin, ledDuty(0));
  digitalWrite(kLedGreenPin, ledDuty(0));
  digitalWrite(kLedBluePin, ledDuty(0));

  // 3. 其它引脚初始化
  pinMode(kMuxS0Pin, OUTPUT);
  pinMode(kMuxS1Pin, OUTPUT);
  pinMode(kMuxS2Pin, OUTPUT);
  pinMode(kFsrEnablePin, OUTPUT);
  pinMode(kBoostEnablePin, OUTPUT);
  pinMode(kAmpShutdownPin, OUTPUT);
  pinMode(kPowerKeyPin, INPUT_PULLUP);
  pinMode(kVbusDetPin, INPUT);
  
  digitalWrite(kFsrEnablePin, kFsrPowerActiveLevel);

  // 4. 此时引脚已经是稳定的灭灯状态，再进行 PWM 转换
  setupRgbLed();

  analogReadResolution(12);
  analogSetPinAttenuation(kMuxAdcPin, ADC_11db);
}

void setupBle() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(bleDeviceName, sizeof(bleDeviceName), "%s-%02X%02X", kDeviceNamePrefix, mac[4], mac[5]);
  BLEDevice::init(bleDeviceName);
  bleStarted = true;
  BLEDevice::setMTU(128);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  BLEService *service = bleServer->createService(kServiceUuid);
  notifyCharacteristic = service->createCharacteristic(
      kNotifyUuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  notifyCharacteristic->addDescriptor(new BLE2902());
  controlCharacteristic = service->createCharacteristic(
      kControlUuid, BLECharacteristic::PROPERTY_WRITE);
  controlCharacteristic->setCallbacks(new ControlCallbacks());
  service->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  BLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setCompleteServices(BLEUUID(kServiceUuid));
  BLEAdvertisementData scanResp;
  scanResp.setName(bleDeviceName);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanResp);
  adv->setScanResponse(true);
  startAdvertising();
  Serial.print("[BLE] Ready, advertising as: ");
  Serial.println(bleDeviceName);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  setupPins();
  
  // 关键优化：开机确认前保持灭灯
  setRgb(0, 0, 0); 
  confirmWakeFromPowerKey();
  
  // 只有确认开机（按满2秒）后，才开启蓝牙和亮灯
  setupBle();
  Serial.println("[System] Boot complete");
}

void loop() {
  const uint32_t now = millis();
  updatePowerKey(now);
  updateStatusLed(now);
  updateAdvertising(now);
  handleBleControl();
  if (now - lastSampleAt < kSampleIntervalMs) {
    delay(2);
    return;
  }
  lastSampleAt = now;
  const uint8_t triggerMask = sampleSensors();
  publishBleSample(triggerMask);
  static uint32_t lastPrintAt = 0;
  if (now - lastPrintAt > 1000) {
    printSample(triggerMask);
    lastPrintAt = now;
  }
}
