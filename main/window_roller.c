#include <stdio.h>
#include "easy_connect.h"
#include <driver/gpio.h>
#include "ota.h"
#include "custom_mqtt.h"

#ifdef CONFIG_OTA_UPDATES_INTERVAL
#define OTA_UPDATES_INTERVAL CONFIG_OTA_UPDATES_INTERVAL
#else
#define OTA_UPDATES_INTERVAL 6
#endif
#define GPIO_1  5
#define GPIO_2  6
#define GPIO_3  7
#define GPIO_4  10
#define GPIO_LED 8
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_1) | (1ULL<<GPIO_2) | \
                              (1ULL<<GPIO_3) | (1ULL<<GPIO_4) | (1ULL<<GPIO_LED))
#define DELAY 2

typedef struct {
    uint16_t steps;
} motor_t;

void process_input(void *pvParameters);
void runMotor(void *pvParameters);
void pubInfo();

const char *tag = "WIN_ROLL";
const gpio_num_t pins[4] = {GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_10};
const char *mqtt_topic_pub = "window_roller/state";
const char *mqtt_topic_sub = "window_roller/motor";
const int ROLLER_UP = 0;
const int ROLLER_HALF = 40000;
const int ROLLER_DOWN = 86000;

static QueueHandle_t motorQueue;
static motor_t motor;

#define GPIO_LED 8
#define GPIO_CONF_LED (1ULL<<GPIO_LED)

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    wifi_init_sta();

    gpio_config_t gpio_conf = {
        .pin_bit_mask = GPIO_OUTPUT_PIN_SEL,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };

    gpio_config(&gpio_conf);

    gpio_set_level(GPIO_LED, 1);

    ESP_LOGI("led", "led: %s", esp_err_to_name(gpio_set_level(GPIO_LED, 1)));

    // powinno dzialac do tego momentu

    esp_mqtt_topic_t topics[1];

    topics[0].filter = strdup(mqtt_topic_sub);
    topics[0].qos = 0;

    extern const uint8_t ca_crt_start[] asm("_binary_ca_cert_pem_start");

    input_ota_conf_t config = {
        .URL = strdup("https://192.168.0.100/window_roller/latest.bin"),
        .cert = (char *)ca_crt_start,
        .update_interval_h = atoi(OTA_UPDATES_INTERVAL),
    };

    ret = https_ota_init(&config);

    if (ret != ESP_OK) ESP_LOGE(tag, "OTA init error");

    ret = mqtt_start(topics, 1, (char *)&ca_crt_start);

    if (ret != ESP_OK) ESP_LOGE(tag, "mqtt_start() ERROR: %s", esp_err_to_name(ret));
    else ESP_LOGI(tag, "mqtt started successfuly!");
    
    for (int i=0; i<1; i++)
    {
        free((void *)topics[i].filter);
        topics[i].filter = NULL;
    }

    motor.steps = 0;
    motorQueue = xQueueCreate(5, sizeof(int));
    
    xTaskCreate(runMotor, "Motor driver", 4096, motorQueue, 2, NULL);


    /*
    TODO:
    [ ] Stepper motor driver
    [ ] Steps counter - logic 
    [ ] OTA
    [ ] logs
    [ ] Light-sleep
    [ ] Stack and Heap statistics
            uxTaskGetStackHighWaterMark(task)
    [ ] functions to test new soft version (app rollback)
    [ ] 
    
    esp_ota_mark_app_valid_cancel_rollback();
   */

}


void pubInfo()
{
    esp_err_t ret;
    payload_t data;
    data.topic = malloc(sizeof(char) * 32);
    snprintf(data.topic, 32, mqtt_topic_pub);
    data.topic_len = 32;
    data.msg = malloc(sizeof(char) * 32);
    data.msg_len = 32;
    if (motor.steps < ROLLER_UP)
    {
        snprintf(data.msg, 32, "Fully opened");
        ret = mqtt_pub(data);
    }
    else if (motor.steps > (ROLLER_HALF - 10) || (motor.steps < ROLLER_HALF + 10))
    {
        snprintf(data.msg, 32, "Half opened");
        ret = mqtt_pub(data);
    }
    else if (motor.steps > ROLLER_DOWN)
    {
        snprintf(data.msg, 32, "Fully closed");
        ret = mqtt_pub(data);
    }
    if (ret == ESP_ERR_INVALID_ARG)
    {
        ESP_LOGE(tag, "Could not publicate data - damaged payload");
    }
    else if (ret == ESP_FAIL)
    {
        ESP_LOGE(tag, "Could not publicate data - general error");
    }
}
void runMotor(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    int goal;
 
    static const int sequence[9][2] = {
        {GPIO_1, -1},
        {GPIO_1, GPIO_2},
        {GPIO_2, -1},
        {GPIO_2, GPIO_3},
        {GPIO_3, -1},
        {GPIO_3, GPIO_4},
        {GPIO_4, -1},
        {GPIO_4, GPIO_1},
        {GPIO_1, -1}
    };
    for (;;)
    {
        if (xQueueReceive(queue, &goal, portMAX_DELAY) != pdTRUE) continue;
 
        if (goal > motor.steps)
        {
            while (goal > motor.steps)
            {
                for (int i=1; i<9; i++)
                {
                    gpio_set_level(sequence[i-1][0], 0);
                    if (sequence[i-1][1] != -1) gpio_set_level(sequence[i-1][1], 0);
                    gpio_set_level(sequence[i][0], 1);
                    if (sequence[i][1] != -1) gpio_set_level(sequence[i][1], 1);
                    vTaskDelay(pdMS_TO_TICKS(DELAY));
                }
                motor.steps += 8;
            }    
        }
        else if (goal < motor.steps)
        {
            while (goal < motor.steps)
            {
                for (int i=8; i>0; i--)
                {
                    gpio_set_level(sequence[i-1][0], 0);
                    if (sequence[i-1][1] != -1) gpio_set_level(sequence[i-1][1], 0);
                    gpio_set_level(sequence[i][0], 1);
                    if (sequence[i][1] != -1) gpio_set_level(sequence[i][1], 1);
                    vTaskDelay(pdMS_TO_TICKS(DELAY));
                }
                motor.steps -= 8;
            } 
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(5, 0);
        gpio_set_level(6, 0);
        gpio_set_level(7, 0);
        gpio_set_level(10, 0);
        pubInfo();
    }
}
void process_input(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    payload_t *data;
    for (;;)
    {
        data = mqtt_get(); // blocks code
     
        ESP_LOGI(tag, "Got topic:msg - %.s:%.s", data->topic, data->topic_len, data->msg, data->msg_len);
        if (strcmp(data->topic, mqtt_topic_sub) != 0)
        {
            if (strcmp(data->msg, "up")) 
            {
                ESP_ERROR_CHECK(xQueueSend(queue, &ROLLER_UP, 1000));
            }
            else if (strcmp(data->msg, "half"))
            {
                ESP_ERROR_CHECK(xQueueSend(queue, &ROLLER_HALF, 1000));
            }
            else if (strcmp(data->msg, "down"))
            {
                ESP_ERROR_CHECK(xQueueSend(queue, &ROLLER_DOWN, 1000));
            }
            else
            {
                ESP_LOGW(tag, "Unknown msg");
            }
        }
        free(data->msg);
        free(data->topic);
    }
}



