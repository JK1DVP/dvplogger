#include "i2c_guard.h"

#include "decl.h"
#include "variables.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_i2c_mutex = nullptr;
static TaskHandle_t s_i2c_owner_task = nullptr;
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_last_diag_ms = 0;
static const char *s_owner = nullptr;

void init_i2c_guard()
{
  if (s_i2c_mutex == nullptr) {
    s_i2c_mutex = xSemaphoreCreateMutex();
  }
}


void i2c_set_owner_task(TaskHandle_t owner)
{
  s_i2c_owner_task = owner ? owner : xTaskGetCurrentTaskHandle();
}

bool i2c_is_owner_task()
{
  return s_i2c_owner_task == nullptr ||
         xTaskGetCurrentTaskHandle() == s_i2c_owner_task;
}

static bool diag_allowed()
{
  uint32_t now = millis();
  bool allowed = false;
  portENTER_CRITICAL(&s_diag_mux);
  if ((uint32_t)(now - s_last_diag_ms) >= 1000U) {
    s_last_diag_ms = now;
    allowed = true;
  }
  portEXIT_CRITICAL(&s_diag_mux);
  return allowed;
}

bool i2c_bus_lock(const char *owner, TickType_t timeout_ticks)
{
  // All Wire/I2C transactions belong to the Arduino setup()/loop() task.
  // A mutex alone is not sufficient for U8G2 because its RAM framebuffer can
  // be modified before sendBuffer(); reject accidental worker-task access.
  if (!i2c_is_owner_task()) {
    if (diag_allowed()) {
      console->printf("I2C access rejected owner=%s core=%d task=%s\n",
                      owner ? owner : "?", xPortGetCoreID(),
                      pcTaskGetName(nullptr));
    }
    return false;
  }
  if (s_i2c_mutex == nullptr) init_i2c_guard();
  if (s_i2c_mutex == nullptr) return false;

  uint32_t started_us = micros();
  if (xSemaphoreTake(s_i2c_mutex, timeout_ticks) != pdTRUE) {
    if (diag_allowed()) {
      console->printf("I2C lock timeout owner=%s held_by=%s wait=%lu us core=%d\n",
                      owner ? owner : "?", s_owner ? s_owner : "?",
                      (unsigned long)(micros() - started_us), xPortGetCoreID());
    }
    return false;
  }

  s_owner = owner;
  uint32_t waited_us = micros() - started_us;
  if ((verbose & 1024) && waited_us >= 2000U && diag_allowed()) {
    console->printf("I2C lock slow owner=%s wait=%lu us core=%d\n",
                    owner ? owner : "?", (unsigned long)waited_us,
                    xPortGetCoreID());
  }
  return true;
}

void i2c_bus_unlock(const char *owner)
{
  (void)owner;
  if (s_i2c_mutex == nullptr) return;
  s_owner = nullptr;
  xSemaphoreGive(s_i2c_mutex);
}

void i2c_diag_io(const char *owner, uint32_t elapsed_us)
{
  if ((verbose & 1024) && elapsed_us >= 5000U && diag_allowed()) {
    console->printf("I2C io slow owner=%s io=%lu us core=%d\n",
                    owner ? owner : "?", (unsigned long)elapsed_us,
                    xPortGetCoreID());
  }
}
