#define USE_NIMBLE
#include <NimBLEDevice.h>
#include <BleMouse.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "nvs_flash.h"

// --- Hardware Pins (ESP32-C3 SuperMini) ---
#define SDA_PIN 5
#define SCL_PIN 6
#define BUTTON_PIN 3
#define LED_PIN 10

// --- Parameters ---
#define SENSITIVITY 18
#define DEADZONE 0.08
#define HOLD_TIME 400      // Clutch hold time (ms)
#define RESET_TIME 5000    // BLE unpair / erase time (ms)
#define CLICK_WINDOW 800   // Triple click window (ms)

Adafruit_MPU6050 mpu;
BleMouse bleMouse("SuperMini Glove", "DIY", 100);

float calibGx = 0, calibGy = 0, calibGz = 0;
bool mpuReady = false;
bool globalLock = false;
bool holdPause = false;

int clickCount = 0;
unsigned long firstClickTime = 0;
unsigned long lastDebounceTime = 0;
bool lastButtonState = HIGH;
bool buttonState = HIGH;

bool initMPU() {
  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  // Wake MPU-6050 up directly
  Wire.beginTransmission(0x68);
  Wire.write(0x6B); // PWR_MGMT_1 register
  Wire.write(0x00); // Wake up
  Wire.endTransmission();
  delay(50);
  return mpu.begin(0x68, &Wire);
}

void resetPairing() {
  Serial.println("[BLE] Resetting paired devices...");
  for (int i = 0; i < 20; i++) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(50);
  }
  nvs_flash_erase();
  nvs_flash_init();
  Serial.println("[BLE] Done. Restarting MCU...");
  delay(500);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // 1. BLE setup
  Serial.println("[SYS] Starting BLE stack...");
  NimBLEDevice::init("SuperMini Glove");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Maximum TX power

  auto* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setAppearance(0x03C2); // Generic Mouse appearance
  pAdvertising->setMinInterval(32);    // 20ms
  pAdvertising->setMaxInterval(64);    // 40ms

  bleMouse.begin();
  Serial.println("[SYS] BLE Mouse advertising started.");

  // 2. I2C setup
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  if (initMPU()) {
    Serial.println("[MPU] MPU-6050 connected successfully.");
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

    // Auto-calibration
    float gx = 0, gy = 0, gz = 0;
    for (int i = 0; i < 150; i++) {
      sensors_event_t a, g, temp;
      mpu.getEvent(&a, &g, &temp);
      gx += g.gyro.x;
      gy += g.gyro.y;
      gz += g.gyro.z;
      delay(5);
    }
    calibGx = gx / 150.0;
    calibGy = gy / 150.0;
    calibGz = gz / 150.0;
    mpuReady = true;
    Serial.println("[MPU] Calibration completed.");
  } else {
    Serial.println("[MPU] Warning: MPU-6050 not responding at boot.");
    mpuReady = false;
  }

  digitalWrite(LED_PIN, LOW);
}

void loop() {
  unsigned long now = millis();
  int reading = digitalRead(BUTTON_PIN);

  // --- Button debouncing ---
  if (reading != lastButtonState) {
    lastDebounceTime = now;
  }

  if ((now - lastDebounceTime) > 50) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        if (clickCount == 0) firstClickTime = now;
        clickCount++;
      } else {
        // Released
        if (now - lastDebounceTime > RESET_TIME) {
          resetPairing();
        }
        holdPause = false;
      }
    }
  }

  // --- Hold & Long-press detection ---
  if (buttonState == LOW) {
    unsigned long pressDur = now - lastDebounceTime;
    if (pressDur > RESET_TIME) {
      // Fast strobe: ready to unpair
      digitalWrite(LED_PIN, (now / 100) % 2);
    } else if (pressDur > HOLD_TIME) {
      // Clutch hold: pause mouse movement
      holdPause = true;
      digitalWrite(LED_PIN, HIGH);
      clickCount = 0;
    }
  }

  // --- Triple-click detection ---
  if (clickCount > 0 && (now - firstClickTime > CLICK_WINDOW)) {
    if (clickCount == 3) {
      globalLock = !globalLock;
      Serial.printf("[SYS] Toggle Lock: %s\n", globalLock ? "LOCKED" : "UNLOCKED");
    }
    clickCount = 0;
  }
  lastButtonState = reading;

  // --- Re-try MPU initialization if it was missed at cold boot ---
  if (!mpuReady) {
    static unsigned long lastMpuRetry = 0;
    if (now - lastMpuRetry > 3000) {
      lastMpuRetry = now;
      if (initMPU()) {
        mpuReady = true;
        Serial.println("[MPU] Late detection: MPU-6050 initialized!");
      }
    }
  }

  // --- Mouse movement execution ---
  if (!globalLock && !holdPause && bleMouse.isConnected() && mpuReady) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    float gz = g.gyro.z - calibGz;
    float gy = g.gyro.y - calibGy;

    if (abs(gz) < DEADZONE) gz = 0;
    if (abs(gy) < DEADZONE) gy = 0;

    int moveX = (int)(-gz * SENSITIVITY);
    int moveY = (int)(-gy * SENSITIVITY);

    if (moveX != 0 || moveY != 0) {
      bleMouse.move(moveX, moveY);
    }
  }

  // --- Status LED indication ---
  if (globalLock && buttonState == HIGH) {
    // Slow blink: locked
    digitalWrite(LED_PIN, (now / 500) % 2);
  } else if (!holdPause && !globalLock && buttonState == HIGH) {
    digitalWrite(LED_PIN, LOW);
  }

  delay(10);
}
