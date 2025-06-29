#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_sleep.h>
#include <esp_pm.h>
#include <driver/adc.h>
#include "DHT.h"

#define DHT_PIN 4
#define LDR 34
#define LED 2

#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t sleepManagerHandle = NULL;
SemaphoreHandle_t sensorSemaphore;

struct SensorData {
  float temperature;
  float humidity;
  int lightLevel;
  unsigned long timestamp;
};

SensorData currentData;
volatile bool sensorReadingReady = false;

#define MEASUREMENT_INTERVAL_MS 30000
#define DEEP_SLEEP_THRESHOLD_MS 35000

void configurePower() {
  // setting up dynamic power management for esp
  // allowing frequency scaling and light sleep
  esp_pm_config_esp32_t config = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 80,
    .light_sleep_enable = true
  };

  // applying the power config
  esp_err_t res = esp_pm_configure(&config);
  if (res != ESP_OK) {
    // fallback if config fails
    Serial.println("power config failed. trying fallback");
    esp_pm_config_esp32_t fallback = {
      .max_freq_mhz = 240,
      .min_freq_mhz = 160,
      .light_sleep_enable = false
    };
    if (esp_pm_configure(&fallback) == ESP_OK) {
      Serial.println("fallback config applied.");
    } else {
      Serial.println("even fallback failed. moving on.");
    }
  } else {
    Serial.println("power config OK");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Setup:");

  pinMode(LED, OUTPUT);
  pinMode(LDR, INPUT);
  dht.begin();

  // setting up adc for ldr (gpio34 = adc1_channel_6)
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);

  // creating semaphore for sync between sensor and sleep tasks
  sensorSemaphore = xSemaphoreCreateBinary();

  configurePower();

  // launching the tasks
  xTaskCreate(sensorTask, "SensorTask", 4096, NULL, 2, &sensorTaskHandle);
  xTaskCreate(sleepManagerTask, "SleepMgr", 2048, NULL, 1, &sleepManagerHandle);

  Serial.println("All good. Looping...");
}

void loop() {
  // nothing in loop since we're fully on freertos
}

void sensorTask(void *param) {
  vTaskDelay(pdMS_TO_TICKS(2000)); // give some time before starting

  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(MEASUREMENT_INTERVAL_MS);

  while (1) {
    Serial.println("--- SENSOR READ ---");
    digitalWrite(LED, HIGH); // show we’re active

    readTempHumidity();
    vTaskDelay(pdMS_TO_TICKS(100)); // just in case dht needs a pause
    readLight();
    packageData();

    digitalWrite(LED, LOW); // done reading

    xSemaphoreGive(sensorSemaphore); // tell sleep mgr that data's here

    vTaskDelayUntil(&lastWake, interval); // wait till next cycle
  }
}

void readTempHumidity() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) currentData.temperature = t;
  if (!isnan(h)) currentData.humidity = h;

  Serial.printf("Temp: %.1f C | Humidity: %.1f %%\n", currentData.temperature, currentData.humidity);
}

void readLight() {
  int raw = adc1_get_raw(ADC1_CHANNEL_6);
  currentData.lightLevel = map(raw, 0, 4095, 0, 100); // converting to %
  Serial.printf("Light: %d%% (raw %d)\n", currentData.lightLevel, raw);
}

void packageData() {
  currentData.timestamp = millis();
  sensorReadingReady = true;

  Serial.printf("Packed: T=%.1f H=%.1f L=%d %% | t=%lu ms\n",
    currentData.temperature, currentData.humidity,
    currentData.lightLevel, currentData.timestamp
  );

  checkThresholds(); // basic range checks
}

void checkThresholds() {
  bool alert = false;
  static int prevLight = -1;

  if (currentData.temperature > 35.0 || currentData.temperature < 5.0) {
    Serial.println("Temp too much");
    alert = true;
  }

  if (currentData.humidity > 80.0 || currentData.humidity < 20.0) {
    Serial.println("humidity out of range");
    alert = true;
  }

  // basic delta check on light % to catch sudden changes
  if (prevLight != -1 && abs(currentData.lightLevel - prevLight) > 30) {
    Serial.println("you are going blind");
    alert = true;
  }

  prevLight = currentData.lightLevel;

  if (alert) {
    Serial.println("alert");
  }
}

void sleepManagerTask(void *parameter) {
  TickType_t idleStart;
  bool isIdle = false;
  bool gotFirst = false;

  Serial.println("SleepMgr waiting...");

  while (1) {
    if (xSemaphoreTake(sensorSemaphore, pdMS_TO_TICKS(5000)) == pdTRUE) {
      Serial.println("SleepMgr: new data");
      handleData(); // process what sensorTask collected
      idleStart = xTaskGetTickCount();
      isIdle = false;
      gotFirst = true;
    } else {
      if (gotFirst && !isIdle) {
        idleStart = xTaskGetTickCount();
        isIdle = true;
        Serial.println("SleepMgr: idle timer started");
      }

      // if idle for too long, go to deep sleep
      if (gotFirst && isIdle) {
        TickType_t idleTime = xTaskGetTickCount() - idleStart;
        if (idleTime > pdMS_TO_TICKS(DEEP_SLEEP_THRESHOLD_MS)) {
          Serial.println("idle > threshold. sleeping...");
          enterDeepSleep();
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500)); // check every half sec
  }
}

void handleData() {
  if (!sensorReadingReady) return;

  Serial.println("processing data...");
  vTaskDelay(pdMS_TO_TICKS(500)); // simulate some processing delay
  sensorReadingReady = false;
}

void enterDeepSleep() {
  Serial.println("Entering deep sleep...");
  Serial.flush();

  // setup wakeup timer
  esp_sleep_enable_timer_wakeup((uint64_t)MEASUREMENT_INTERVAL_MS * 1000);
  esp_deep_sleep_start(); // boom, sleeping now
}
