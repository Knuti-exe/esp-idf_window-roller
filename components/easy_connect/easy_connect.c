#include "easy_connect.h"

static const char *wifi_tag = "WiFi STA";
static const char *dpp_tag = "DPP";

static int s_retry_num = 0;
static wifi_config_t s_dpp_wifi_config = {};
static EventGroupHandle_t s_dpp_event_group;
uint8_t ssid[32];
uint8_t passwd[64];

static esp_err_t write_nvs_conf(uint8_t *_ssid, uint8_t *_passwd);


static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_ERROR_CHECK(esp_supp_dpp_start_listen());
            ESP_LOGI(dpp_tag, "Started listening for DPP Authentication");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry_num < WIFI_MAX_RETRY_NUM) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGW(wifi_tag, "Disconnect event, retry to connect to the AP");
            } else {
                xEventGroupSetBits(s_dpp_event_group, DPP_CONNECT_FAIL_BIT);
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(wifi_tag, "Successfully connected to the AP ssid : %s ", s_dpp_wifi_config.sta.ssid);
            break;
        case WIFI_EVENT_DPP_URI_READY:
            ESP_LOGI(dpp_tag, "U can scan ur QR code (not generated)");
            wifi_event_dpp_uri_ready_t *uri_data = event_data;
            ESP_LOGI(dpp_tag, "%s", uri_data->uri);
            break;
        case WIFI_EVENT_DPP_CFG_RECVD:
            ESP_LOGI(dpp_tag, "Got DPP conf");

            wifi_event_dpp_config_received_t *config = event_data;

            ESP_LOGI(dpp_tag, "Saving to NVS: %s", esp_err_to_name(write_nvs_conf(config->wifi_cfg.sta.ssid, config->wifi_cfg.sta.password)));

            ESP_LOGW(dpp_tag, "RESTARTING!");

            abort(); // ouch!

            break;
        case WIFI_EVENT_DPP_FAILED:
            wifi_event_dpp_failed_t *dpp_failure = event_data;
            if (s_retry_num < 5) {
                ESP_ERROR_CHECK(esp_supp_dpp_start_listen());
                s_retry_num++;

                switch(dpp_failure->failure_reason)
                {
                    case ESP_ERR_DPP_FAILURE:
                        ESP_LOGW(dpp_tag, "Failure: Probably wrong QR code. Trying again...");
                        break;
                    case ESP_ERR_DPP_TX_FAILURE:
                        ESP_LOGW(dpp_tag, "Failure: Could't read properly - try scan QR code again...");
                        break;
                    default:
                        ESP_LOGE(dpp_tag, "General failure. Error code - %s", esp_err_to_name(dpp_failure->failure_reason));
                        break;
                }
            } else {
                xEventGroupSetBits(s_dpp_event_group, DPP_AUTH_FAIL_BIT);
            }

            break;
        default:
            break;
        }
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(wifi_tag, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_dpp_event_group, DPP_CONNECTED_BIT);
    }
    
}

static void event_handler_second(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                if (s_retry_num < WIFI_MAX_RETRY_NUM) {
                    esp_wifi_connect();
                    s_retry_num++;
                    ESP_LOGW(wifi_tag, "Disconnect event, retry to connect to the AP");
                } else {
                    xEventGroupSetBits(s_dpp_event_group, DPP_CONNECT_FAIL_BIT);
                }
                break;
            case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(wifi_tag, "Successfully connected to the AP ssid : %s ", s_dpp_wifi_config.sta.ssid);
                break;
            default:
                break;
        }
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(wifi_tag, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_dpp_event_group, DPP_CONNECTED_BIT);
    }
}

static esp_err_t read_nvs_conf(uint8_t *ssid, uint8_t *passwd)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open("wifi_cred", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW("NVS", "Could not open wifis NVS! %s", esp_err_to_name(err));
        // nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    size_t required_size;
    nvs_get_str(nvs_handle, "ssid", NULL, &required_size);
    err = nvs_get_str(nvs_handle, "ssid", (char *)ssid, &required_size);

    if (err != ESP_OK)
    {
        ESP_LOGW("NVS", "Could not read SSID from memory: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    nvs_get_str(nvs_handle, "passwd", NULL, &required_size);
    err = nvs_get_str(nvs_handle, "passwd", (char *)passwd, &required_size);

    if (err != ESP_OK)
    {
        ESP_LOGW("NVS", "Could not read passwd from memory: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

static esp_err_t write_nvs_conf(uint8_t *_ssid, uint8_t *_passwd)
{
    uint8_t ssid[32] = {0};
    uint8_t passwd[64] = {0};

    
    memcpy(ssid, _ssid, sizeof(ssid));
    memcpy(passwd, _passwd, sizeof(passwd));

    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open("wifi_cred", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW("NVS", "Could not open wifis NVS! %s", esp_err_to_name(err));
        // nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    err = nvs_set_str(nvs_handle, "ssid", (char *)ssid);

    if (err != ESP_OK)
    {
        ESP_LOGW("NVS", "Could not write SSID to memory: %s", esp_err_to_name(err));
        nvs_close(nvs_handle); 
        return ESP_FAIL;
    }

    err = nvs_set_str(nvs_handle, "passwd", (char *)passwd);

    if (err != ESP_OK)
    {
        ESP_LOGW("NVS", "Could not write passwd to memory: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW("NVS", "Could not commit NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

static esp_err_t dpp_enrollee_bootstrap(void)
{
    esp_err_t ret;
    size_t pkey_len = strlen(EXAMPLE_DPP_BOOTSTRAPPING_KEY);
    char *key = NULL;

    if (pkey_len) {
        // Currently only NIST P-256 curve is supported, add prefix/postfix accordingly
        char prefix[] = "30310201010420";  // 7 bytes
        char postfix[] = "a00a06082a8648ce3d030107";  // 12 bytes

        if (pkey_len != CURVE_SEC256R1_PKEY_HEX_DIGITS) {
            ESP_LOGE(dpp_tag, "Invalid key length! Private key needs to be 32 bytes (or 64 hex digits) long");
            return ESP_FAIL;
        } 
        
        key = malloc(sizeof(prefix) + pkey_len + sizeof(postfix));

        if (!key) {
            ESP_LOGE(dpp_tag, "Failed to allocate for bootstrapping key");
            return ESP_ERR_NO_MEM;
        }

        snprintf(key, sizeof(prefix) + pkey_len + sizeof(postfix), "%s%s%s", prefix, EXAMPLE_DPP_BOOTSTRAPPING_KEY, postfix);
    }
    ret = esp_supp_dpp_bootstrap_gen(EXAMPLE_DPP_LISTEN_CHANNEL_LIST, DPP_BOOTSTRAP_QR_CODE,
                                    key, EXAMPLE_DPP_DEVICE_INFO);

    if (key)
        free(key);
    return ret;
    
}

void wifi_init_sta()
{
    bool tried_connect = false;
    s_dpp_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler_second, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler_second, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    //                                  First try - check NVS for last WiFi credentials

    uint8_t *ssid = malloc(sizeof(uint8_t[32]));
    uint8_t *passwd = malloc(sizeof(uint8_t[64]));

    memset(ssid, 0, sizeof(uint8_t[32]));
    memset(passwd, 0, sizeof(uint8_t[64]));

    wifi_config_t wifi_config = {0};


    if (read_nvs_conf(ssid, passwd) != ESP_FAIL && strlen((char *)ssid) != 0 && strlen((char *)passwd) != 0 )
    {
        
        ESP_LOGI("NVS", "Reading from NVS...%s", (char *)ssid);

        memcpy(wifi_config.sta.ssid, ssid, sizeof(uint8_t[32]));
        memcpy(wifi_config.sta.password, passwd, sizeof(uint8_t[64]));

        
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    else
    {
        ESP_LOGW("NVS", "Couln't read from NVS.");
        xEventGroupSetBits(s_dpp_event_group, WIFI_NVS_FAIL_BIT);
    }
    
   
    EventBits_t bits = xEventGroupWaitBits(s_dpp_event_group,
                                           DPP_CONNECTED_BIT | DPP_CONNECT_FAIL_BIT | WIFI_NVS_FAIL_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           portMAX_DELAY);

    bool wifi_connected = false;

    if (bits & DPP_CONNECTED_BIT) 
    {
        wifi_connected = true;
    } 
    else if (bits & DPP_CONNECT_FAIL_BIT)
    {
        ESP_LOGE(wifi_tag, "Failed to connect to SSID:%s", s_dpp_wifi_config.sta.ssid);
    } 
    else if (bits & WIFI_NVS_FAIL_BIT) 
    {
        ESP_LOGI(dpp_tag, "Initializing DPP...");
    } 

    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler_second));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler_second));

    if (wifi_connected) 
    {
        vEventGroupDelete(s_dpp_event_group);
        free(ssid);
        free(passwd);
        ssid = NULL;
        passwd = NULL;

        // ESP_ERROR_CHECK(xSemaphoreGive(_wifiMutex));
        return;
    }
    if (tried_connect) ESP_ERROR_CHECK(esp_wifi_stop());

    vTaskDelay(pdMS_TO_TICKS(10));
    s_retry_num = 0;

    //                                  Second try - start Easy Connect
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_supp_dpp_init(NULL));
    ESP_ERROR_CHECK(dpp_enrollee_bootstrap());
    ESP_ERROR_CHECK(esp_wifi_start());


    bits = xEventGroupWaitBits(s_dpp_event_group,
                                DPP_CONNECTED_BIT | DPP_CONNECT_FAIL_BIT | DPP_AUTH_FAIL_BIT,
                                pdTRUE,
                                pdFALSE,
                                portMAX_DELAY);
        
    if (bits & DPP_CONNECTED_BIT) 
    {        
        ESP_LOGI(dpp_tag, "Saving configuration: %s", esp_err_to_name(write_nvs_conf(ssid, passwd)));

        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } 
    else if (bits & DPP_CONNECT_FAIL_BIT)
    {
        ESP_LOGI(wifi_tag, "Failed to connect to SSID:%s", s_dpp_wifi_config.sta.ssid);
    } 
    else if (bits & DPP_AUTH_FAIL_BIT) 
    {
        ESP_LOGI(dpp_tag, "DPP Authentication failed after %d retries", s_retry_num);
    }
    else 
    {
        ESP_LOGE(dpp_tag, "UNEXPECTED EVENT");
    }

    esp_supp_dpp_deinit();
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    vEventGroupDelete(s_dpp_event_group);
    
    if (ssid) free(ssid);
    if (passwd) free(passwd);
    ssid = NULL;
    passwd = NULL;
}