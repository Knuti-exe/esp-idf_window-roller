#include <esp_https_ota.h>
#include <stdio.h>
#include <string.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_log.h>

typedef struct {
    char *cert;
    char *URL;
    int update_interval_h;
    SemaphoreHandle_t _blockUpdate;
    QueueHandle_t logs_queue;
}input_ota_conf_t;

/**
 * @brief Initializes HTTPS OTA feature
 * 
 * Configuration parameters:
 * - @p cert: Certificate for HTTPS connection,
 * - @p URL: Address where binary file is,
 * - @p update_interval_h: Update interval in hours,
 * - @p _blockUpdate: Semaphore to block other functions during update,
 * - @p logs_queue: Queue for logs,
 */
esp_err_t https_ota_init(input_ota_conf_t *config);

/**
 * @brief Cancel rollback. Should indicate that new version is stable
*/
void ota_cancel_rollback();

/**
 * @brief Try to fetch new version from HTTPS server
 */
void force_ota_update();

/**
 * @brief Get running version
 * @returns Running version as string
 */
char *getRunningVer();

// https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/api-reference/system/ota.html#secure-ota-updates