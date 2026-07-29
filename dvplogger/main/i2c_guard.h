#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void init_i2c_guard();
void i2c_set_owner_task(TaskHandle_t owner = nullptr);
bool i2c_is_owner_task();
bool i2c_bus_lock(const char *owner, TickType_t timeout_ticks = pdMS_TO_TICKS(20));
void i2c_bus_unlock(const char *owner);
void i2c_diag_io(const char *owner, uint32_t elapsed_us);
