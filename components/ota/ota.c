#include "ota.h"
#include "ble_listening.h"
#include "custom_mqtt.h"

esp_https_ota_config_t esp_ota_conf;
static int interval = 0;
static const char *tag = "HTTPS_OTA";
static esp_http_client_config_t http_conf = {0};
QueueHandle_t LogsQueue = NULL;
TaskHandle_t ota_loop_handle = NULL;
static SemaphoreHandle_t blockUpdate = NULL;

void https_ota_update();
void https_ota_loop(void *pvParameters);
esp_err_t validate_version(esp_app_desc_t *new_app);

esp_err_t https_ota_init(input_ota_conf_t *config)
{
    if (config->cert == NULL || config->URL == NULL) return ESP_FAIL;

    http_conf.cert_pem = strdup(config->cert);  // malloc() + deep copy
    http_conf.url = strdup(config->URL);
    interval = config->update_interval_h;
    blockUpdate = config->_blockUpdate;
    LogsQueue = config->logs_queue;

    interval = interval ? interval : 24;

    xTaskCreate(https_ota_loop, "ota update", 8192, NULL, 5, &ota_loop_handle);

    return ESP_OK;
}

void https_ota_loop(void *pvParameters)
{
    esp_ota_conf.http_config = &http_conf;

    uint32_t notified_bits;

    for (;;)
    {
        esp_err_t ret = xTaskNotifyWait(0x00, ULONG_MAX, &notified_bits, pdMS_TO_TICKS(3600000 * interval));

        if ((notified_bits & (1 << 0)) || ret == pdFAIL)
        {
            if (xSemaphoreTake(blockUpdate, portMAX_DELAY)!= pdPASS) continue;

            ESP_LOGI(tag, "Starting OTA update...");

            LogsMsg_t log_msg = {0};
            snprintf(log_msg.msg, 64, "OTA: Starting OTA update...");
            log_msg.msg_len = strlen(log_msg.msg);
            xQueueSend(LogsQueue, &log_msg, 0); 

            suspend_ble();
            https_ota_update();
        }
    }
}

void force_ota_update()
{
    if (ota_loop_handle != NULL)
    {
        xTaskNotify(ota_loop_handle, (1 << 0), eSetBits);
    }
}


void ota_cancel_rollback()
{
    esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();

    LogsMsg_t log_msg = {0};
    
    if (ret == ESP_OK) snprintf(log_msg.msg, 64, "OTA: APP OK - Rollback canceled.");    
    else snprintf(log_msg.msg, 64, "OTA: APP WARNING - Could not rollback application.");

    log_msg.msg_len = strlen(log_msg.msg);
    xQueueSend(LogsQueue, &log_msg, 0); 
}

void https_ota_update()
{
    for (int i=0; i<3; i++)
    {
        esp_err_t ret;
        esp_https_ota_handle_t ota_handle = NULL;
        esp_app_desc_t app_desc = {};
        LogsMsg_t log_msg = {0};


        ret = esp_https_ota_begin(&esp_ota_conf, &ota_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(tag, "Unexpected error on ota_begin: %s", esp_err_to_name(ret));
            snprintf(log_msg.msg, 64, "OTA: UPDATE ERROR - %s", esp_err_to_name(ret)); 
            log_msg.msg_len = strlen(log_msg.msg);
            xQueueSend(LogsQueue, &log_msg, 0); 

            esp_https_ota_abort(ota_handle);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ret = esp_https_ota_get_img_desc(ota_handle, &app_desc);
        if (ret != ESP_OK)
        {
            ESP_LOGE(tag, "Unexpected error on ota_get_img_desc: %s", esp_err_to_name(ret));
            snprintf(log_msg.msg, 64, "OTA: READING IMAGE ERROR - %s", esp_err_to_name(ret)); 
            log_msg.msg_len = strlen(log_msg.msg);
            xQueueSend(LogsQueue, &log_msg, 0); 

            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ret = validate_version(&app_desc);

        if (ret == ESP_OK) snprintf(log_msg.msg, 64, "OTA: new ver available! - %s", app_desc.version);
        else if (ret != ESP_OK) snprintf(log_msg.msg, 64, "OTA: Found older or same version! Aborting update...");
        log_msg.msg_len = strlen(log_msg.msg);

        xQueueSend(LogsQueue, &log_msg, 0); 
        
        if (ret == ESP_OK)
        {
            int max_size = esp_https_ota_get_image_size(ota_handle);
            int last_log = 0;

            do 
            {
                last_log ++;
                ret = esp_https_ota_perform(ota_handle);
                
                if (max_size != -1 && last_log % 20 == 0)
                {
                    ESP_LOGI(tag, "Got: %.1f %%", (float) 100.0 * esp_https_ota_get_image_len_read(ota_handle) / max_size);
                }
                
                vTaskDelay(1); // 1 tick should me ~1-10 ms
            }
            while (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

            if (esp_https_ota_is_complete_data_received(ota_handle) != true) 
            {
                ESP_LOGE(tag, "Complete data was not received.");
            }
            else 
            {
                esp_err_t ota_finish_err = esp_https_ota_finish(ota_handle);
                if (ret == ESP_OK && ota_finish_err == ESP_OK) 
                {
                    ESP_LOGI(tag, "ESP_HTTPS_OTA upgrade successful. Rebooting...");
                    snprintf(log_msg.msg, 64, "OTA: UPGRADE SUCCESSFUL - Rebooting..."); 
                    log_msg.msg_len = strlen(log_msg.msg);
                    xQueueSend(LogsQueue, &log_msg, 0); 

                    vTaskDelay(pdMS_TO_TICKS(5000));   // 5 sec for sending logs 
                    esp_restart();
                } 
                else 
                {
                    if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) 
                    {
                        ESP_LOGE(tag, "Image validation failed, image is corrupted or not-signed");
                        snprintf(log_msg.msg, 64, "[ERROR]\t\tOTA: Validation FAILED - (not signed or corrupted)"); 
                        log_msg.msg_len = strlen(log_msg.msg);
                        xQueueSend(LogsQueue, &log_msg, 0); 
                    }
                    else
                    {
                        ESP_LOGE(tag, "Attempt %d: ESP_HTTPS_OTA upgrade failed (%s)", i, esp_err_to_name(ota_finish_err));
                    }
                    continue;
                }
            }
        }
        else
        {
            ESP_LOGI(tag, "Same software version - OTA update will not be terminated.");
            esp_https_ota_abort(ota_handle);
            break;
        }

        esp_https_ota_abort(ota_handle);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    xSemaphoreGive(blockUpdate);
    resume_ble();

}

esp_err_t validate_version(esp_app_desc_t *new_app)
{
    if (new_app == NULL) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_info;

    if (esp_ota_get_partition_description(running, &running_info) == ESP_OK) 
    {
        ESP_LOGI(tag, "Running firmware version: %s", running_info.version);
        ESP_LOGI(tag, "New firmware version: %s", new_app->version);
    }

    int major, minor, patch, major2, minor2, patch2;
    int ret = -1;

    ret = sscanf(running_info.version, "%d.%d.%d", &major, &minor, &patch);
    if (ret != 3) ESP_LOGW(tag, "Latest.bin contains wrong versioning format. Aborting update...");
    
    ret = sscanf(new_app->version, "%d.%d.%d", &major2, &minor2, &patch2);
    if (ret != 3) ESP_LOGW(tag, "Latest.bin contains wrong versioning format. Aborting update...");

    if (major2 > major) return ESP_OK;
    else if (major2 < major) return ESP_FAIL;

    if (minor2 > minor) return ESP_OK;
    else if (minor2 < minor) return ESP_FAIL;
    
    if (patch2 > patch) return ESP_OK;
    else return ESP_FAIL;
}

char *getRunningVer()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_info;

    char *version = NULL;

    if (esp_ota_get_partition_description(running, &running_info) == ESP_OK) 
    {
        version = strdup(running_info.version);
    }

    return version;
}