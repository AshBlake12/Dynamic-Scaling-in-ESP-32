# Dynamic-Scaling-in-ESP-32

conducting dynamic frequency scaling to conserve power on an esp32 using freertos.

this project demonstrates how to configure and utilize the esp32's dynamic frequency scaling and light/deep sleep modes to reduce power consumption in freertos-based applications.

---

## objective

- implement dynamic frequency scaling between defined min and max frequencies
- enable light sleep to reduce idle power usage
- transition into deep sleep after a period of inactivity
- evaluate impact on power efficiency while maintaining periodic task execution

---

## key components

- **esp_pm_configure**: to set dynamic frequency scaling and light sleep configuration
- **freertos tasks**: separate tasks for sensing and power management
- **esp_sleep**: for entering deep sleep based on idle time logic
- **semaphore-based signaling**: to coordinate between tasks

---

## implementation overview

1. set up power configuration using `esp_pm_configure()`  
   - min frequency: 80 mhz  
   - max frequency: 240 mhz  
   - light sleep: enabled (with fallback)

2. define two tasks:
   - `sensorTask`: reads data from peripherals (dht11, ldr) at a fixed interval
   - `sleepManagerTask`: monitors activity and controls sleep state transitions

3. enter **deep sleep** if no sensor activity is detected beyond the threshold (e.g. 35 seconds)

4. upon wakeup (via timer), resume sensing

---

## hardware used

- **esp32 dev board**
- **dht11 sensor** for temperature and humidity
- **ldr** (photoresistor) connected to adc pin for ambient light sensing
- **gpio-led** used as a simple activity indicator

---

## wiring

| device        | gpio pin       |
|---------------|----------------|
| dht11 sensor  | gpio 4         |
| ldr sensor    | gpio 34 (adc1) |
| onboard led   | gpio 2         |

---

## code behavior

- every 30 seconds: sensor data is read and printed
- thresholds for alerting are applied to temperature, humidity, and light
- if system remains idle (no new data) for over 35 seconds, it enters deep sleep
- on wakeup, it resumes operation

---

## power configuration example

```cpp
esp_pm_config_esp32_t config = {
  .max_freq_mhz = 240,
  .min_freq_mhz = 80,
  .light_sleep_enable = true
};

esp_pm_configure(&config);

a fallback configuration is also applied if this fails.
