#include <stdio.h>
#include "easy_connect.h"
#include <driver/gpio.h>
#include "ota.h"
#include "custom_mqtt.h"
#include <esp_sntp.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <esp_system.h>

#ifdef CONFIG_OTA_UPDATES_INTERVAL
#define OTA_UPDATES_INTERVAL CONFIG_OTA_UPDATES_INTERVAL
#else
#define OTA_UPDATES_INTERVAL 6
#endif

#define GPIO_1  5
#define GPIO_2  6
#define GPIO_3  7
#define GPIO_4  10
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_1) | (1ULL<<GPIO_2) | \
                              (1ULL<<GPIO_3) | (1ULL<<GPIO_4))
#define DELAY 2

typedef struct {
    volatile uint32_t steps;
} motor_t;

void process_input(void *pvParameters);
void runMotor(void *pvParameters);
void pubInfo();
void motor_timer_callback(void* arg);
void log_timer_callback(void* arg);
uint32_t load_motor_steps();
void save_motor_steps(uint32_t steps);

const char *tag = "WIN_ROLL";
const gpio_num_t pins[4] = {GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_10};
const char *mqtt_topic_pub = "window_roller/state";
const char *mqtt_topic_sub = "window_roller/motor";
const char *mqtt_update_sub = "window_roller/update";       // CANNOT BE RETAINED MESSAGE
const int ROLLER_UP = 0;
const int ROLLER_HALF = 40000;
const int ROLLER_DOWN = 86000;
static bool cancel_rollback = false;
volatile int target_step = 0;
int seq_index = 0;
int log_index = 0;

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

static QueueHandle_t motorQueue;
static motor_t motor;
esp_timer_handle_t motor_timer;
const esp_timer_create_args_t timer_args = {
    .callback = &motor_timer_callback,
    .name = "motor_timer"
};
esp_timer_handle_t logs_timer;
const esp_timer_create_args_t log_timer_args = {
    .callback = &log_timer_callback,
    .name = "log_timer"
};
static SemaphoreHandle_t motor_sem = NULL;
static SemaphoreHandle_t blockUpdate = NULL;

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    wifi_init_sta();
    // ESP_LOGI(tag, "Wifi connection: %s", esp_err_to_name(easy_connect()));

    gpio_config_t gpio_conf = {
        .pin_bit_mask = GPIO_OUTPUT_PIN_SEL,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };

    gpio_config(&gpio_conf);

    motor.steps = load_motor_steps();

    esp_mqtt_topic_t topics[2];

    topics[0].filter = strdup(mqtt_topic_sub);
    topics[0].qos = 0;
    topics[1].filter = strdup(mqtt_update_sub);
    topics[1].qos = 1;

    blockUpdate = xSemaphoreCreateMutex();
    extern const uint8_t ca_crt_start[] asm("_binary_ca_cert_pem_start");

    input_ota_conf_t config = {
        .URL = strdup("https://192.168.0.100/window_roller/latest.bin"),
        .cert = (char *)ca_crt_start,
        .update_interval_h = atoi(OTA_UPDATES_INTERVAL),
        ._blockUpdate = blockUpdate,
    };

    ret = https_ota_init(&config);

    if (ret != ESP_OK) ESP_LOGE(tag, "OTA init error");

    ret = mqtt_start(topics, 2, (char *)&ca_crt_start);

    if (ret != ESP_OK) ESP_LOGE(tag, "mqtt_start() ERROR: %s", esp_err_to_name(ret));
    else ESP_LOGI(tag, "mqtt started successfuly!");
    
    for (int i=0; i<2; i++)
    {
        free((void *)topics[i].filter);
        topics[i].filter = NULL;
    }

    motor.steps = 0;
    motorQueue = xQueueCreate(10, sizeof(int));
    
    xTaskCreate(runMotor, "Motor driver", 4096, (void *)motorQueue, 2, NULL);
    xTaskCreate(process_input, "Listen & process data", 4096, (void *)motorQueue, 3, NULL);

    motor_sem = xSemaphoreCreateBinary();
    esp_timer_create(&timer_args, &motor_timer);
    esp_timer_create(&log_timer_args, &logs_timer);

    xSemaphoreGive(blockUpdate);

    /*
    TODO:
    [X] OTA
    [X] Steps counter - logic 
    [X] Stepper motor driver
    [ ] OTA new feature -> blocking update while motor is working and program on update
    [ ] logs
    [ ] BLE listening
    [ ] Stack and Heap statistics
            uxTaskGetStackHighWaterMark(task)
    
   */

}


void pubInfo()
{
    esp_err_t ret;
    payload_t data = {0};

    data.topic = strdup(mqtt_topic_pub);
    data.topic_len = strlen(mqtt_topic_pub);

    if (motor.steps < ROLLER_UP + 10) data.msg = strdup("Fully opened");
    else if (motor.steps > (ROLLER_HALF - 10) 
        && (motor.steps < ROLLER_HALF + 10)) data.msg = strdup("half opened");
    else if (motor.steps > ROLLER_DOWN - 10) data.msg = strdup("Fully closed");
    else
    {   
        int len = snprintf(NULL, 0, "Unknown pos: %ld", motor.steps);
        data.msg = malloc(len + 1);       

        snprintf(data.msg, len + 1, "Unknown pos: %ld", motor.steps);
    }
    data.msg_len = strlen(data.msg);

    ret = mqtt_pub(&data);

    if (ret == ESP_ERR_INVALID_ARG) ESP_LOGE(tag, "Could not publicate data - damaged payload");
    else if (ret == ESP_FAIL) ESP_LOGE(tag, "Could not publicate data - general error");

    free(data.topic);
    free(data.msg);
    data.topic = NULL;
    data.msg = NULL;
}

void runMotor(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    int goal;
    
    for (;;)
    {
        if (xQueueReceive(queue, &goal, portMAX_DELAY) != pdTRUE) continue;
        
        ESP_LOGI(tag, "Running motor... (steps: %ld; goal: %d)", motor.steps, goal);

        target_step = goal;

        if (xSemaphoreTake(blockUpdate, portMAX_DELAY) == pdPASS)
        {
            xQueueReset(motor_sem);  // same as => xSemaphoreTake(motor_sem, 0);

            esp_timer_start_periodic(motor_timer, 2000);
            esp_timer_start_periodic(logs_timer, 1000000);

            if (xSemaphoreTake(motor_sem, portMAX_DELAY) == pdPASS)
            {
                ESP_LOGI(tag, "Motor on position!");

                esp_timer_stop(logs_timer);

                pubInfo();

                save_motor_steps(motor.steps);

                if (!cancel_rollback)
                {
                    cancel_rollback = true;
                    ota_cancel_rollback();
                }

                if (xSemaphoreGive(blockUpdate) == pdFAIL) ESP_LOGE(tag, "Could not relase blockUpdate semaphore");
            }   
        }
    }
}

void motor_timer_callback(void* arg) 
{
    if (target_step == motor.steps)
    {
        esp_timer_stop((esp_timer_handle_t)arg);
        xSemaphoreGive(motor_sem);
        return;
    }
    gpio_set_level(sequence[seq_index][0], 0);
    if (sequence[seq_index][1] != -1) gpio_set_level(sequence[seq_index][1], 0);

    if (motor.steps < target_step) 
    {
        seq_index = (seq_index + 1) % 8;
        motor.steps++;
    }
    else if (motor.steps > target_step) 
    {
        seq_index = (seq_index - 1 + 8) % 8;
        motor.steps--;
    }

    gpio_set_level(sequence[seq_index][0], 1);
    if (sequence[seq_index][1] != -1) gpio_set_level(sequence[seq_index][1], 1);
}

void log_timer_callback(void* arg) 
{    
    printf("Step: %ld\n", motor.steps); // #TODO send that via MQTT (outside that function)
}

void process_input(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    payload_t *data;
    for (;;)
    {
        data = mqtt_get(); // blocks code

        ESP_LOGI(tag, "Got topic:msg - %.*s:%.*s", data->topic_len, data->topic, data->msg_len, data->msg);
        
        if (strcmp(data->topic, mqtt_topic_sub) == 0)
        {
            if (strcmp(data->msg, "up") == 0) 
            {
                ESP_LOGI(tag, "Going up");

                if (xQueueSend(queue, &ROLLER_UP, 1000) != pdPASS)
                {
                    ESP_LOGW(tag, "Queue full - waiting for pending operations...");
                }
            }
            else if (strcmp(data->msg, "half") == 0)
            {
                ESP_LOGI(tag, "Going half-way");

                if (xQueueSend(queue, &ROLLER_HALF, 1000) != pdPASS)
                {
                    ESP_LOGW(tag, "Queue full - waiting for pending operations...");
                }
                
            }
            else if (strcmp(data->msg, "down") == 0)
            {
                ESP_LOGI(tag, "Going down");

                if (xQueueSend(queue, &ROLLER_DOWN, 1000) != pdPASS)
                {
                    ESP_LOGW(tag, "Queue full - waiting for pending operations...");
                }
            }
            else
            {
                ESP_LOGW(tag, "Unknown msg");
            }
        }
        else if (strcmp(data->topic, mqtt_update_sub) == 0)
        {
            if (strcmp(data->msg, "true") == 0 && xSemaphoreTake(blockUpdate, 0) == pdPASS)
            {
                xSemaphoreGive(blockUpdate);
                ESP_LOGW(tag, "Trying to update software manually...");
                force_ota_update();
            }
        }

        free(data->msg);
        free(data->topic);
    }    
}

void save_motor_steps(uint32_t steps) 
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open("motor_data", NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(tag, "Error while opening NVS namespace: %s\n", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u32(handle, "steps", steps);
    if (err != ESP_OK) printf("Error while saving!\n");

    err = nvs_commit(handle);
    if (err != ESP_OK) ESP_LOGE(tag, "Could not commit save to NVS!\n");

    nvs_close(handle);
}

uint32_t load_motor_steps() {
    nvs_handle_t handle;
    uint32_t steps = 0; 

    if (nvs_open("motor_data", NVS_READONLY, &handle) == ESP_OK)
    {
        nvs_get_u32(handle, "steps", &steps);
        nvs_close(handle);
    }
    return steps;
}