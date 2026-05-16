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

esp_err_t https_ota_init(input_ota_conf_t *config);
void ota_cancel_rollback();
void force_ota_update();
char *getRunningVer();
void otaGiveLogQueue(QueueHandle_t queue);

// https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32/api-reference/system/ota.html#secure-ota-updates