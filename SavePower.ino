#include "DHT.h"

#define DHT_PIN 4
#define LDR_PIN 34
#define LED_PIN 2


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
#define DEEP_SLEEP_THRESHOLD_MS 35000  // enter sleep if idle > 35 seconds  

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(LDR_PIN, INPUT);
  dht.begin();
  
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
  
  sensorSemaphore = xSemaphoreCreateBinary();
  
  // Configure power management
  configurePowerManagement();
  
  // Create FreeRTOS tasks
  xTaskCreate(sensorTask, "SensorTask", 4096, NULL, 2, &sensorTaskHandle);
  xTaskCreate(sleepManagerTask, "SleepManager", 2048, NULL, 1, &sleepManagerHandle);
  
  Serial.println("System initialized. Entering tickless operation...");
}

void loop() {
  // Main loop intentionally empty - FreeRTOS handles execution
  vTaskDelete(NULL);
}

void configurePowerManagement() {
  // Configure automatic light sleep with conservative parameters
  esp_pm_config_esp32_t pm_config = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 80,    // More conservative minimum frequency
    .light_sleep_enable = true
  };
  
  esp_err_t result = esp_pm_configure(&pm_config);
  if (result != ESP_OK) {
    Serial.printf("Power management configuration failed with error: 0x%x\n", result);
    Serial.println("Attempting fallback configuration...");
    
    // Fallback configuration without automatic light sleep
    esp_pm_config_esp32_t fallback_config = {
      .max_freq_mhz = 240,
      .min_freq_mhz = 160,   // Higher minimum for stability
      .light_sleep_enable = false
    };
    
    result = esp_pm_configure(&fallback_config);
    if (result == ESP_OK) {
      Serial.println("Fallback power management configured successfully");
      Serial.println("Note: Light sleep disabled - using manual deep sleep only");
    } else {
      Serial.printf("Fallback configuration also failed: 0x%x\n", result);
      Serial.println("Continuing with default power management");
    }
  } else {
    Serial.println("Power management configured for tickless operation");
  }
}

void sensorTask(void *parameter) {
  // Allow initial system stabilization before first measurement
  vTaskDelay(pdMS_TO_TICKS(2000));
  
  TickType_t lastWakeTime = xTaskGetTickCount();
  const TickType_t measurementPeriod = pdMS_TO_TICKS(MEASUREMENT_INTERVAL_MS);
  
  while (true) {
    Serial.println("=== Starting Sensor Measurement Cycle ===");
    digitalWrite(LED_PIN, HIGH);  // Indicate active measurement
    
    // Read DHT11 sensor
    readDHTSensor();
    
    // Small delay between sensor readings
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Read light level
    readLightSensor();
    
    // Process and store data
    processSensorData();
    
    digitalWrite(LED_PIN, LOW);  // Measurement complete
    
    // Signal sleep manager that reading is complete
    xSemaphoreGive(sensorSemaphore);
    
    Serial.println("=== Sensor measurements complete ===");
    Serial.printf("Next measurement in %d seconds\n", MEASUREMENT_INTERVAL_MS/1000);
    Serial.flush(); // Ensure all data is transmitted before potential sleep
    
    // Wait for next scheduled measurement time
    vTaskDelayUntil(&lastWakeTime, measurementPeriod);
  }
}

void readDHTSensor() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  
  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("DHT sensor reading failed - using previous values");
  } else {
    currentData.temperature = temperature;
    currentData.humidity = humidity;
    Serial.printf("DHT11 - Temperature: %.1f°C, Humidity: %.1f%%\n", 
                  temperature, humidity);
  }
}

void readLightSensor() {
  // Enable ADC for single measurement
  int rawValue = adc1_get_raw(ADC1_CHANNEL_6);
  
  // Convert to percentage (0-100%)
  currentData.lightLevel = map(rawValue, 0, 4095, 0, 100);
  
  Serial.printf("Light Level: %d%% (Raw: %d)\n", 
                currentData.lightLevel, rawValue);
}

void processSensorData() {
  currentData.timestamp = millis();
  sensorReadingReady = true;
  
  // Log complete sensor reading
  Serial.printf("Sensor Data Package: T=%.1f°C, H=%.1f%%, L=%d%%, Time=%lu\n",
                currentData.temperature, currentData.humidity, 
                currentData.lightLevel, currentData.timestamp);
  
  // Check for threshold conditions that might affect sleep strategy
  checkSensorThresholds();
}

void checkSensorThresholds() {
  bool alertCondition = false;
  
  // Check temperature thresholds
  if (currentData.temperature > 35.0 || currentData.temperature < 5.0) {
    Serial.println("ALERT: Temperature outside normal range");
    alertCondition = true;
  }
  
  // Check humidity thresholds
  if (currentData.humidity > 80.0 || currentData.humidity < 20.0) {
    Serial.println("ALERT: Humidity outside normal range");
    alertCondition = true;
  }
  
  // Check light level for dramatic changes
  static int previousLight = -1;
  if (previousLight >= 0 && abs(currentData.lightLevel - previousLight) > 30) {
    Serial.println("ALERT: Significant light level change detected");
    alertCondition = true;
  }
  previousLight = currentData.lightLevel;
  
  // Adjust sleep behavior based on alert conditions
  if (alertCondition) {
    Serial.println("Alert condition detected - reducing sleep duration");
    // Could implement shorter sleep cycles here
  }
}

void sleepManagerTask(void *parameter) {
  TickType_t idleStartTime;
  bool systemIdle = false;
  bool initialMeasurementComplete = false;
  
  Serial.println("Sleep Manager: Waiting for initial sensor measurement...");
  
  while (true) {
    // Wait for sensor task completion or timeout
    if (xSemaphoreTake(sensorSemaphore, pdMS_TO_TICKS(5000)) == pdTRUE) {
      // Sensor reading completed
      Serial.println("Sleep Manager: Sensor data received");
      initialMeasurementComplete = true;
      systemIdle = false;
      
      // Perform any data transmission or processing here
      processDataForTransmission();
      
      // Reset idle timer after processing
      idleStartTime = xTaskGetTickCount();
      Serial.println("Sleep Manager: System ready for sleep after data processing");
    } else {
      // No sensor activity - check if we should enter idle state
      if (initialMeasurementComplete && !systemIdle) {
        systemIdle = true;
        idleStartTime = xTaskGetTickCount();
        Serial.println("Sleep Manager: System entering idle state");
      }
      
      // Only consider deep sleep after initial measurement is complete
      if (initialMeasurementComplete && systemIdle) {
        TickType_t idleDuration = xTaskGetTickCount() - idleStartTime;
        if (idleDuration > pdMS_TO_TICKS(DEEP_SLEEP_THRESHOLD_MS)) {
          Serial.printf("Sleep Manager: Idle duration %lu ms exceeded threshold\n", 
                       pdTICKS_TO_MS(idleDuration));
          enterDeepSleep();
        }
      }
    }
    
    // Brief delay to prevent task monopolization
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void processDataForTransmission() {
  if (!sensorReadingReady) return;
  
  Serial.println("Processing sensor data for transmission...");
  
  // Simulate data processing/transmission delay
  vTaskDelay(pdMS_TO_TICKS(500));
  
  // In a real implementation, this would handle:
  // - Data formatting
  // - WiFi connection management
  // - Cloud transmission
  // - Local storage
  
  Serial.println("Data processing complete");
  sensorReadingReady = false;
}

void enterDeepSleep() {
  Serial.println("Entering deep sleep mode...");
  Serial.flush();
  
  // Configure wake-up timer
  uint64_t sleepDuration = MEASUREMENT_INTERVAL_MS * 1000; // Convert to microseconds
  esp_sleep_enable_timer_wakeup(sleepDuration);
  
  // Optionally configure external wake-up sources
  // esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Wake on button press
  
  // Enter deep sleep
  esp_deep_sleep_start();
}

// Optional: Add interrupt handlers for immediate wake-up conditions
void IRAM_ATTR emergencyWakeISR() {
  // Handle critical sensor conditions that require immediate attention
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  vTaskNotifyGiveFromISR(sensorTaskHandle, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}