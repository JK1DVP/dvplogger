/*
 * dvplogger - field companion for ham radio operator
 * dvplogger - アマチュア無線家のためのフィールド支援ツール
 * Copyright (c) 2021-2026 Eiichiro Araki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include <assert.h>

#include <string.h>
#include "esp_err.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_loader_io.h"
#include "esp_loader.h"
#include "esp32_port.h"
#include "example_common.h"


//#include <SD.h>
//#include "esp_vfs_fat.h"
//#include "driver/sdspi_host.h"
//#include "sdmmc_cmd.h"


// required in partition based flashing ?
#include "esp_flash.h"
#include "esp_partition.h"
//#include <stdio.h>

#include "mcp_interface.h"
#include "esp32_flasher.h"

//#include <SoftwareSerial.h>
//#include "decl.h"
#include "sd_wrapper.h"

/* Route flasher progress to the same runtime console Stream as commands. */
#define printf(...) dvplogger_console_printf(__VA_ARGS__)
//extern SoftwareSerial Serial3;
//extern SoftwareSerial Serial4;

static const char *TAG = "serial_flasher";

extern const uint8_t  bootloader_bootloader_bin[];
extern const uint32_t bootloader_bootloader_bin_size;
extern const uint8_t  bootloader_bootloader_bin_md5[];
extern const uint8_t  jk1dvplog_ext_bin[];
extern const uint32_t jk1dvplog_ext_bin_size;
extern const uint8_t  jk1dvplog_ext_bin_md5[];
extern const uint8_t  partition_table_partition_table_bin[];
extern const uint32_t partition_table_partition_table_bin_size;
extern const uint8_t  partition_table_partition_table_bin_md5[];
const uint8_t gpio0_trigger_mcp_pin = 14;
const uint8_t reset_trigger_mcp_pin = 15;
// For esp32, esp32s2
#define BOOTLOADER_ADDRESS_V0       0x1000
// For esp8266, esp32s3 and later chips
#define BOOTLOADER_ADDRESS_V1       0x0
// For esp32c5
#define BOOTLOADER_ADDRESS_V2       0x2000
#define PARTITION_ADDRESS           0x8000
#define APPLICATION_ADDRESS         0x10000
#define SPIFFS_ADDRESS              0x190000

#define HIGHER_BAUDRATE 230400

// Max line size
#define BUF_LEN 128
static uint8_t buf[BUF_LEN] = {0};

static const char *get_error_string(const esp_loader_error_t error);

void slave_monitor(void *arg)
{
#if (HIGHER_BAUDRATE != 115200)
    uart_flush_input(UART_NUM_2);
    uart_flush(UART_NUM_2);
    uart_set_baudrate(UART_NUM_2, 115200);
#endif
    while (1) {
        int rxBytes = uart_read_bytes(UART_NUM_2, buf, BUF_LEN, 100 / portTICK_PERIOD_MS);
        buf[rxBytes] = '\0';
        printf("%s", buf);
    }
}

void loader_boot_init_func() {
  printf("[FLASHERSD] boot init: MCP pin %u -> HIGH/output\n", gpio0_trigger_mcp_pin);
  mcp_write_pin(gpio0_trigger_mcp_pin,1);
  mcp_pin_mode(gpio0_trigger_mcp_pin,OUTPUT);
}

void loader_reset_init_func() {
  printf("[FLASHERSD] reset init: MCP pin %u -> HIGH/output\n", reset_trigger_mcp_pin);
  mcp_write_pin(reset_trigger_mcp_pin,1);
  mcp_pin_mode(reset_trigger_mcp_pin,OUTPUT);
}

void loader_port_reset_target_func() {
  printf("[FLASHERSD] reset target: MCP pin %u LOW hold=%u ms\n",
         reset_trigger_mcp_pin, (unsigned int)SERIAL_FLASHER_RESET_HOLD_TIME_MS);
  mcp_write_pin(reset_trigger_mcp_pin, 0);
  loader_port_delay_ms(SERIAL_FLASHER_RESET_HOLD_TIME_MS);
  mcp_write_pin(reset_trigger_mcp_pin, 1);
  printf("[FLASHERSD] reset target: MCP pin %u HIGH\n", reset_trigger_mcp_pin);
}

void loader_port_enter_bootloader_func() {
  printf("[FLASHERSD] enter bootloader: MCP GPIO0 pin %u LOW\n",
         gpio0_trigger_mcp_pin);
  mcp_write_pin(gpio0_trigger_mcp_pin,0);
  loader_port_reset_target_func();
  printf("[FLASHERSD] boot hold=%u ms\n",
         (unsigned int)SERIAL_FLASHER_BOOT_HOLD_TIME_MS);
  loader_port_delay_ms(SERIAL_FLASHER_BOOT_HOLD_TIME_MS);
  mcp_write_pin(gpio0_trigger_mcp_pin,1);
  printf("[FLASHERSD] enter bootloader: MCP GPIO0 pin %u HIGH\n",
         gpio0_trigger_mcp_pin);
}



void esp_flasher_sd(char *which)
{
  printf("[FLASHERSD] ENTRY before config hwver=%d stack_hwm=%u free_heap=%u\n",
         JK1DVPLOG_HWVER,
         (unsigned int)uxTaskGetStackHighWaterMark(NULL),
         (unsigned int)esp_get_free_heap_size());
  fflush(stdout);

  printf("[FLASHERSD] sizeof(loader_esp32_config_t)=%u\n",
         (unsigned int)sizeof(loader_esp32_config_t));
  fflush(stdout);

  const loader_esp32_config_t config = {
    .baud_rate = 115200,
    .uart_port = UART_NUM_2,
    .uart_rx_pin = GPIO_NUM_34,
    .uart_tx_pin = GPIO_NUM_4,
    .reset_trigger_pin = GPIO_NUM_15,
    .gpio0_trigger_pin = GPIO_NUM_27,
    .loader_boot_init_func=loader_boot_init_func, // target boot pin initialization function or NULL
    .loader_reset_init_func=loader_reset_init_func, // target reset pin initialization function or NULL
    .loader_port_reset_target_func=loader_port_reset_target_func, // target reset function or NULL
    .loader_port_enter_bootloader_func=loader_port_enter_bootloader_func // target enter bootloader function or NULL
  };

  printf("[FLASHERSD] config constructed stack_hwm=%u free_heap=%u\n",
         (unsigned int)uxTaskGetStackHighWaterMark(NULL),
         (unsigned int)esp_get_free_heap_size());
  fflush(stdout);

  printf("[FLASHERSD] begin which='%s' hwver=%d\n",
         which ? which : "(null)", JK1DVPLOG_HWVER);
  printf("[FLASHERSD] config uart=%d rx=%d tx=%d initial_baud=%d higher_baud=%d\n",
         (int)config.uart_port, (int)config.uart_rx_pin, (int)config.uart_tx_pin,
         (int)config.baud_rate, (int)HIGHER_BAUDRATE);
  printf("[FLASHERSD] MCP gpio0=%u reset=%u uart2_driver_before=%d\n",
         gpio0_trigger_mcp_pin, reset_trigger_mcp_pin,
         uart_is_driver_installed(UART_NUM_2) ? 1 : 0);
  fflush(stdout);

  printf("[FLASHERSD] BEFORE loader_port_esp32_init stack_hwm=%u\n",
         (unsigned int)uxTaskGetStackHighWaterMark(NULL));
  fflush(stdout);

  //  setup_vfs_sd();
  esp_loader_error_t init_err = loader_port_esp32_init(&config);
  printf("[FLASHERSD] loader_port_esp32_init result=%d (%s) uart2_driver_after=%d\n",
         (int)init_err, get_error_string(init_err),
         uart_is_driver_installed(UART_NUM_2) ? 1 : 0);
  if (init_err != ESP_LOADER_SUCCESS) { // replace this with custom version using mcp
    ESP_LOGE(TAG, "serial initialization failed.");
    return;
  }

  printf("[FLASHERSD] connect_to_target begin\n");
  esp_loader_error_t connect_err = connect_to_target(HIGHER_BAUDRATE);
  printf("[FLASHERSD] connect_to_target result=%d (%s)\n",
         (int)connect_err, get_error_string(connect_err));
  if (connect_err == ESP_LOADER_SUCCESS) {
    if (strstr(which,"boot")!=NULL) {
      ESP_LOGI(TAG, "Loading bootloader...");
      int rc = flash_bin_file_streaming("/bootload.bin",BOOTLOADER_ADDRESS_V0);
      printf("[FLASHERSD] boot result=%d\n", rc);
    }
    if (strstr(which,"part") != NULL) {
      ESP_LOGI(TAG, "Loading partition table...");
      int rc = flash_bin_file_streaming("/partitio.bin",PARTITION_ADDRESS);
      printf("[FLASHERSD] partition result=%d\n", rc);
    }
    if (strstr(which,"app")!=NULL) {
      ESP_LOGI(TAG, "Loading app...");
      int rc = flash_bin_file_streaming("/app0.bin",APPLICATION_ADDRESS);
      printf("[FLASHERSD] app result=%d\n", rc);
    }
    if (strstr(which,"spiffs")!= NULL) {
      ESP_LOGI(TAG, "Loading SPIFFS...");
      int rc = flash_bin_file_streaming("/spiffs.bin",SPIFFS_ADDRESS);
      printf("[FLASHERSD] spiffs result=%d\n", rc);
    }
    ESP_LOGI(TAG, "Done!");
    printf("[FLASHERSD] esp_loader_reset_target begin\n");
    esp_loader_reset_target();
    printf("[FLASHERSD] esp_loader_reset_target returned\n");

    // Delay for skipping the boot message of the targets
    vTaskDelay(500 / portTICK_PERIOD_MS);
  } else {
    printf("[FLASHERSD] abort: target connection failed\n");
  }
  printf("[FLASHERSD] end uart2_driver=%d free_heap=%u\n",
         uart_is_driver_installed(UART_NUM_2) ? 1 : 0,
         (unsigned int)esp_get_free_heap_size());
  //  unmount_sdcard_vfs();
}

void esp_flasher(void)
{
    example_binaries_t bin;
    printf("esp_flasher() in\n");
    const loader_esp32_config_t config = {
        .baud_rate = 115200,
        .uart_port = UART_NUM_2,
        .uart_rx_pin = GPIO_NUM_34,
        .uart_tx_pin = GPIO_NUM_4,
        .reset_trigger_pin = GPIO_NUM_15,
        .gpio0_trigger_pin = GPIO_NUM_27,
	.loader_boot_init_func=loader_boot_init_func, // target boot pin initialization function or NULL
	.loader_reset_init_func=loader_reset_init_func, // target reset pin initialization function or NULL
	.loader_port_reset_target_func=loader_port_reset_target_func, // target reset function or NULL
	.loader_port_enter_bootloader_func=loader_port_enter_bootloader_func // target enter bootloader function or NULL
    };
    printf("esp_flasher() 1\n");
    
    if (loader_port_esp32_init(&config) != ESP_LOADER_SUCCESS) { // replace this with custom version using mcp 
        ESP_LOGE(TAG, "serial initialization failed.");
        return;
    }
    printf("esp_flasher() 2\n");
    if (connect_to_target(HIGHER_BAUDRATE) == ESP_LOADER_SUCCESS) {

      //      get_example_binaries(esp_loader_get_target(), &bin);
        bin.boot.data = bootloader_bootloader_bin;
        bin.boot.size = bootloader_bootloader_bin_size;
        bin.boot.md5 = bootloader_bootloader_bin_md5;
        bin.boot.addr = BOOTLOADER_ADDRESS_V0;
        bin.part.data = partition_table_partition_table_bin;
        bin.part.size = partition_table_partition_table_bin_size;
        bin.part.md5 = partition_table_partition_table_bin_md5;
        bin.part.addr = PARTITION_ADDRESS;
        bin.app.data  = jk1dvplog_ext_bin;
        bin.app.size  = jk1dvplog_ext_bin_size;
        bin.app.md5 = jk1dvplog_ext_bin_md5;
        bin.app.addr  = APPLICATION_ADDRESS;
      
	printf("esp_flasher() 3\n");
        ESP_LOGI(TAG, "Loading bootloader...");
        flash_binary(bin.boot.data, bin.boot.size, bin.boot.addr);
        ESP_LOGI(TAG, "Loading partition table...");
        flash_binary(bin.part.data, bin.part.size, bin.part.addr);
        ESP_LOGI(TAG, "Loading app...");
        flash_binary(bin.app.data,  bin.app.size,  bin.app.addr);
        ESP_LOGI(TAG, "Done!");
	
	printf("esp_flasher() 4\n");
        esp_loader_reset_target(); // replace this with local custom version
	printf("esp_flasher() 5\n");
        // Delay for skipping the boot message of the targets
        vTaskDelay(500 / portTICK_PERIOD_MS);

    }
}

static const char *get_error_string(const esp_loader_error_t error)
{
    const char *mapping[ESP_LOADER_ERROR_INVALID_RESPONSE + 1] = {
        "NONE", "UNKNOWN", "TIMEOUT", "IMAGE SIZE",
        "INVALID MD5", "INVALID PARAMETER", "INVALID TARGET",
        "UNSUPPORTED CHIP", "UNSUPPORTED FUNCTION", "INVALID RESPONSE"
    };

    assert(error <= ESP_LOADER_ERROR_INVALID_RESPONSE);

    return mapping[error];
}

esp_loader_error_t connect_to_target(uint32_t higher_transmission_rate)
{
    esp_loader_connect_args_t connect_config = ESP_LOADER_CONNECT_DEFAULT();

    esp_loader_error_t err = esp_loader_connect(&connect_config);
    if (err != ESP_LOADER_SUCCESS) {
        printf("Cannot connect to target. Error: %s\n", get_error_string(err));

        if (err == ESP_LOADER_ERROR_TIMEOUT) {
            printf("Check if the host and the target are properly connected.\n");
        } else if (err == ESP_LOADER_ERROR_INVALID_TARGET) {
            printf("You could be using an unsupported chip, or chip revision.\n");
        } else if (err == ESP_LOADER_ERROR_INVALID_RESPONSE) {
            printf("Try lowering the transmission rate or using shorter wires to connect the host and the target.\n");
        }

        return err;
    }
    printf("Connected to target\n");

#if (defined SERIAL_FLASHER_INTERFACE_UART) || (defined SERIAL_FLASHER_INTERFACE_USB)
    if (higher_transmission_rate && esp_loader_get_target() != ESP8266_CHIP) {
        err = esp_loader_change_transmission_rate(higher_transmission_rate);
        if (err == ESP_LOADER_ERROR_UNSUPPORTED_FUNC) {
            printf("ESP8266 does not support change transmission rate command.");
            return err;
        } else if (err != ESP_LOADER_SUCCESS) {
            printf("Unable to change transmission rate on target.");
            return err;
        } else {
            err = loader_port_change_transmission_rate(higher_transmission_rate);
            if (err != ESP_LOADER_SUCCESS) {
                printf("Unable to change transmission rate.");
                return err;
            }
            printf("Transmission rate changed.\n");
        }
    }
#endif /* SERIAL_FLASHER_INTERFACE_UART || SERIAL_FLASHER_INTERFACE_USB */

    return ESP_LOADER_SUCCESS;
}

#if (defined SERIAL_FLASHER_INTERFACE_UART) || (defined SERIAL_FLASHER_INTERFACE_USB)
esp_loader_error_t connect_to_target_with_stub(const uint32_t current_transmission_rate,
        const uint32_t higher_transmission_rate)
{
    esp_loader_connect_args_t connect_config = ESP_LOADER_CONNECT_DEFAULT();

    esp_loader_error_t err = esp_loader_connect_with_stub(&connect_config);
    if (err != ESP_LOADER_SUCCESS) {
        printf("Cannot connect to target. Error: %s\n", get_error_string(err));

        if (err == ESP_LOADER_ERROR_TIMEOUT) {
            printf("Check if the host and the target are properly connected.\n");
        } else if (err == ESP_LOADER_ERROR_INVALID_TARGET) {
            printf("You could be using an unsupported chip, or chip revision.\n");
        } else if (err == ESP_LOADER_ERROR_INVALID_RESPONSE) {
            printf("Try lowering the transmission rate or using shorter wires to connect the host and the target.\n");
        }

        return err;
    }
    printf("Connected to target\n");

    if (higher_transmission_rate != current_transmission_rate) {
        err = esp_loader_change_transmission_rate_stub(current_transmission_rate,
                higher_transmission_rate);

        if (err == ESP_LOADER_ERROR_UNSUPPORTED_FUNC) {
            printf("ESP8266 does not support change transmission rate command.");
            return err;
        } else if (err != ESP_LOADER_SUCCESS) {
            printf("Unable to change transmission rate on target.");
            return err;
        } else {
            err = loader_port_change_transmission_rate(higher_transmission_rate);
            if (err != ESP_LOADER_SUCCESS) {
                printf("Unable to change transmission rate.");
                return err;
            }
            printf("Transmission rate changed.\n");
        }
    }

    return ESP_LOADER_SUCCESS;
}

esp_loader_error_t flash_binary(const uint8_t *bin, size_t size, size_t address)
{
    esp_loader_error_t err;
    static uint8_t payload[1024];
    const uint8_t *bin_addr = bin;

    printf("Erasing flash (this may take a while)...\n");
    err = esp_loader_flash_start(address, size, sizeof(payload));
    if (err != ESP_LOADER_SUCCESS) {
        printf("Erasing flash failed with error: %s.\n", get_error_string(err));

        if (err == ESP_LOADER_ERROR_INVALID_PARAM) {
            printf("If using Secure Download Mode, double check that the specified\
                    target flash size is correct.\n");
        }
        return err;
    }
    printf("Start programming\n");

    size_t binary_size = size;
    size_t written = 0;

    while (size > 0) {
        size_t to_read = MIN(size, sizeof(payload));
        memcpy(payload, bin_addr, to_read);

        err = esp_loader_flash_write(payload, to_read);
        if (err != ESP_LOADER_SUCCESS) {
            printf("\nPacket could not be written! Error %s.\n", get_error_string(err));
            return err;
        }

        size -= to_read;
        bin_addr += to_read;
        written += to_read;

        int progress = (int)(((float)written / binary_size) * 100);
        printf("\rProgress: %d %%", progress);
    };

    printf("\nFinished programming\n");

#if MD5_ENABLED
    err = esp_loader_flash_verify();
    if (err == ESP_LOADER_ERROR_UNSUPPORTED_FUNC) {
        printf("ESP8266 does not support flash verify command.");
        return err;
    } else if (err != ESP_LOADER_SUCCESS) {
        printf("MD5 does not match. Error: %s\n", get_error_string(err));
        return err;
    }
    printf("Flash verified\n");
#endif

    return ESP_LOADER_SUCCESS;
}
#endif /* SERIAL_FLASHER_INTERFACE_UART || SERIAL_FLASHER_INTERFACE_USB */

esp_loader_error_t load_ram_binary(const uint8_t *bin)
{
    printf("Start loading\n");
    esp_loader_error_t err;
    const esp_loader_bin_header_t *header = (const esp_loader_bin_header_t *)bin;
    esp_loader_bin_segment_t segments[header->segments];

    // Parse segments
    uint32_t seg;
    uint32_t *cur_seg_pos;
    // ESP8266 does not have extended header
    uint32_t offset = esp_loader_get_target() == ESP8266_CHIP ? BIN_HEADER_SIZE : BIN_HEADER_EXT_SIZE;
    for (seg = 0, cur_seg_pos = (uint32_t *)(&bin[offset]);
            seg < header->segments;
            seg++) {
        segments[seg].addr = *cur_seg_pos++;
        segments[seg].size = *cur_seg_pos++;
        segments[seg].data = (uint8_t *)cur_seg_pos;
        cur_seg_pos += (segments[seg].size) / 4;
    }

    // Download segments
    for (seg = 0; seg < header->segments; seg++) {
        printf("Downloading %"PRIu32" bytes at 0x%08"PRIx32"...\n", segments[seg].size, segments[seg].addr);

        err = esp_loader_mem_start(segments[seg].addr, segments[seg].size, ESP_RAM_BLOCK);
        if (err != ESP_LOADER_SUCCESS) {
            printf("Loading to RAM could not be started. Error: %s.\n", get_error_string(err));

            if (err == ESP_LOADER_ERROR_INVALID_PARAM) {
                printf("Check if the chip has Secure Download Mode enabled.\n");
            }
            return err;
        }

        size_t remain_size = segments[seg].size;
        uint8_t *data_pos = segments[seg].data;
        while (remain_size > 0) {
            size_t data_size = MIN(ESP_RAM_BLOCK, remain_size);
            err = esp_loader_mem_write(data_pos, data_size);
            if (err != ESP_LOADER_SUCCESS) {
                printf("\nPacket could not be written! Error: %s.\n", get_error_string(err));
                return err;
            }
            data_pos += data_size;
            remain_size -= data_size;
        }
    }

    err = esp_loader_mem_finish(header->entrypoint);
    if (err != ESP_LOADER_SUCCESS) {
        printf("\nLoading to RAM finished with error: %s.\n", get_error_string(err));
        return err;
    }
    printf("\nFinished loading\n");

    return ESP_LOADER_SUCCESS;
}


// flash from SD card
#define CHUNK_SIZE 1024  // 一度に読み込むバイト数（1KB）

int flash_bin_file_streaming(const char *filepath, uint32_t flash_address) {
    printf("[FLASHERSD-STAGE 01] enter path=%s address=0x%08" PRIX32 "\n",
           filepath, flash_address);
    fflush(stdout);

    printf("[FLASHERSD-STAGE 02] before sd_fopen\n");
    fflush(stdout);
    SDFileHandle *fp = sd_fopen(filepath, "r");
    printf("[FLASHERSD-STAGE 03] after sd_fopen fp=%p\n", (void *)fp);
    fflush(stdout);

    esp_loader_error_t err;
    if (!fp) {
        printf("[FLASHERSD] file open failed path=%s\n", filepath);
        fflush(stdout);
        return -1;
    }

    printf("[FLASHERSD-STAGE 04] before sd_fsize\n");
    fflush(stdout);
    size_t total_size = sd_fsize(fp);
    printf("[FLASHERSD-STAGE 05] after sd_fsize size=%zu\n", total_size);
    fflush(stdout);

    printf("[FLASHERSD-STAGE 06] before sd_fseek(0)\n");
    fflush(stdout);
    sd_fseek(fp, 0);
    printf("[FLASHERSD-STAGE 07] after sd_fseek(0)\n");
    fflush(stdout);

    printf("[FLASHERSD-STAGE 08] before flash_start address=0x%08" PRIX32
           " size=%zu block=%u\n", flash_address, total_size,
           (unsigned int)CHUNK_SIZE);
    fflush(stdout);
    err = esp_loader_flash_start(flash_address, total_size, CHUNK_SIZE);
    printf("[FLASHERSD-STAGE 09] after flash_start result=%d (%s)\n",
           (int)err, get_error_string(err));
    fflush(stdout);
    if (err != ESP_LOADER_SUCCESS) {
        printf("[FLASHERSD] flash_start failed error=%d (%s)\n",
               (int)err, get_error_string(err));
        fflush(stdout);
        if (err == ESP_LOADER_ERROR_INVALID_PARAM) {
            printf("If using Secure Download Mode, double check that the specified target flash size is correct.\n");
            fflush(stdout);
        }
        sd_fclose(fp);
        return err;
    }

    static uint8_t buffer[CHUNK_SIZE];
    size_t offset = 0;
    unsigned int block_no = 0;

    while (!sd_feof(fp) && offset < total_size) {
        size_t to_read = CHUNK_SIZE;
        if (offset + to_read > total_size) {
            to_read = total_size - offset;
        }

        if (block_no == 0) {
            printf("[FLASHERSD-STAGE 10] before first sd_fread offset=%zu len=%zu\n",
                   offset, to_read);
            fflush(stdout);
        }
        size_t read_bytes = sd_fread(buffer, to_read, fp);
        if (block_no == 0) {
            printf("[FLASHERSD-STAGE 11] after first sd_fread read=%zu\n",
                   read_bytes);
            fflush(stdout);
        }
        if (read_bytes != to_read) {
            printf("[FLASHERSD] file read error block=%u offset=%zu requested=%zu read=%zu\n",
                   block_no, offset, to_read, read_bytes);
            fflush(stdout);
            break;
        }

        if (block_no == 0) {
            printf("[FLASHERSD-STAGE 12] before first flash_write len=%zu\n",
                   read_bytes);
            fflush(stdout);
        }
        err = esp_loader_flash_write(buffer, read_bytes);
        if (block_no == 0) {
            printf("[FLASHERSD-STAGE 13] after first flash_write result=%d (%s)\n",
                   (int)err, get_error_string(err));
            fflush(stdout);
        }
        if (err != ESP_LOADER_SUCCESS) {
            printf("[FLASHERSD] flash_write failed block=%u offset=%zu len=%zu error=%d (%s)\n",
                   block_no, offset, read_bytes, (int)err, get_error_string(err));
            fflush(stdout);
            sd_fclose(fp);
            return err;
        }

        offset += read_bytes;
        block_no++;

        if ((offset % (16 * 1024)) == 0 || offset == total_size) {
            printf("[FLASHERSD-STAGE 20] progress block=%u offset=%zu/%zu (%u%%)\n",
                   block_no, offset, total_size,
                   total_size ? (unsigned int)((offset * 100U) / total_size) : 100U);
            fflush(stdout);
        }
    }

    printf("[FLASHERSD-STAGE 30] before sd_fclose offset=%zu/%zu\n",
           offset, total_size);
    fflush(stdout);
    sd_fclose(fp);
    printf("[FLASHERSD-STAGE 31] after sd_fclose\n");
    fflush(stdout);
    printf("[FLASHERSD] file complete path=%s written=%zu/%zu\n",
           filepath, offset, total_size);
    fflush(stdout);
    return 0;
}


#define CHUNK_SIZE 1024

void flash_file_to_partition(const char *filepath) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "mydata");
    
    if (!partition) {
        printf("Partition not found!\n");
        return;
    }

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        printf("File open failed\n");
        return;
    }

    uint8_t buffer[CHUNK_SIZE];
    size_t offset = 0;

    while (!feof(fp)) {
        size_t read_bytes = fread(buffer, 1, CHUNK_SIZE, fp);
        if (read_bytes <= 0) break;

        esp_err_t err = esp_partition_write(partition, offset, buffer, read_bytes);
        if (err != ESP_OK) {
            printf("Write failed at offset %zu: %s\n", offset, esp_err_to_name(err));
            break;
        }

        offset += read_bytes;
    }

    fclose(fp);
    printf("Write complete.\n");
}
