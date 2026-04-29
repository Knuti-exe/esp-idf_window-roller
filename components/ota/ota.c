#include "ota.h"

static input_ota_conf_t ota_conf;
esp_https_ota_config_t esp_ota_conf;
int retries = 0;
static const char *tag = "HTTPS_OTA";


void https_ota_update();
void https_ota_loop(void *pvParameters);
esp_err_t validate_version(esp_app_desc_t *new_app);


esp_err_t https_ota_init(input_ota_conf_t *config)
{
    ota_conf.cert = strdup(config->cert);  // malloc() + deep copy
    ota_conf.URL = strdup(config->URL);
    ota_conf.update_interval_h = config->update_interval_h;

    if (ota_conf.cert == NULL || ota_conf.URL == NULL) return ESP_FAIL;

    xTaskCreate(https_ota_loop, "ota update", 8192, NULL, 5, NULL);

    return ESP_OK;
}

void https_ota_loop(void *pvParameters)
{
    esp_http_client_config_t http_conf = {
        .url = (const char*) ota_conf.URL,
        .cert_pem = (char*) ota_conf.cert,
    };
    esp_ota_conf.http_config = &http_conf;

    int interval = ota_conf.update_interval_h;
    interval = interval ? interval : 2;

    uint32_t notified_bits;

    for (;;)
    {
        esp_err_t ret = xTaskNotifyWait(0x00, ULONG_MAX, &notified_bits, pdMS_TO_TICKS(3600000 * interval));

        if ((notified_bits & (1 << 0)) || ret == pdFAIL)
        {
            https_ota_update();
        }
    }
}

// TODO esp_ota_mark_app_valid_cancel_rollback();


void https_ota_update()
{
    for (int i=0; i<3; i++)
    {
        esp_err_t ret;
        esp_https_ota_handle_t ota_handle = NULL;
        esp_app_desc_t app_desc = {};

        ret = esp_https_ota_begin(&esp_ota_conf, &ota_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(tag, "Unexpected error on ota_begin: %s", esp_err_to_name(ret));
            esp_https_ota_abort(ota_handle);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ret = esp_https_ota_get_img_desc(ota_handle, &app_desc);
        if (ret != ESP_OK)
        {
            ESP_LOGE(tag, "Unexpected error on ota_get_img_desc: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ret = validate_version(&app_desc);
        
        if (ret == ESP_OK)
        {
            ESP_LOGI(tag, "Found newer soft version - starting OTA update.");
            int max_size = esp_https_ota_get_image_size(ota_handle);
            do 
            {
                ret = esp_https_ota_perform(ota_handle);
                
                if (max_size != -1)
                {
                    ESP_LOGI(tag, "Got: %.1f %%", (float) 100.0 * esp_https_ota_get_image_len_read(ota_handle) / max_size);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
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
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                } 
                else 
                {
                    if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) 
                    {
                        ESP_LOGE(tag, "Image validation failed, image is corrupted");
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
    
}

esp_err_t validate_version(esp_app_desc_t *new_app)
{
    if (new_app == NULL) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_app_info;

    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) 
    {
        ESP_LOGI(tag, "Running firmware version: %s", running_app_info.version);
    }

    if (memcmp(new_app->version, running_app_info.version, sizeof(new_app->version)) == 0) return ESP_FAIL;

    return ESP_OK;
}