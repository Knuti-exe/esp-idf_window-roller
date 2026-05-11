#include <stdio.h>
#include "easy_connect.h"
#include <driver/gpio.h>
#include "ota.h"
#include "custom_mqtt.h"
#include <esp_sntp.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include "ble_listening.h"
#include "esp_heap_caps.h"

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
#define QUEUE_LENGTH 20

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
void send_logs(void *pvParameters);
void send_ble_data(void *pvParameters);
void heap_stats(void *pvParameters);

const char *tag = "WIN_ROLL";
const gpio_num_t pins[4] = {GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_10};
const char *mqtt_topic_pub = "smart_home/window_roller/state";
const char *mqtt_topic_sub = "smart_home/window_roller/motor";
const char *mqtt_update_sub = "smart_home/window_roller/update";       // CANNOT BE RETAINED MESSAGE
const char *mqtt_logs_topic = "smart_home/window_roller/logs";
const char *mqtt_plants_plant1 = "smart_home/window_roller/plants/plant1";
const char *mqtt_plants_plant2 = "smart_home/window_roller/plants/plant2";
const char *mqtt_plants_battery = "smart_home/window_roller/plants/battery";
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
static StaticQueue_t static_logs_queue;
QueueHandle_t logs_queue;
LogsMsg_t xPayloadStorage[QUEUE_LENGTH];
QueueHandle_t ble_queue;

void app_main(void)
{
    //                              WiFi configuration
    /************************************************************************ */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    wifi_init_sta();

    //                              MQTT configuration
    /************************************************************************ */
    extern const uint8_t ca_crt_start[] asm("_binary_ca_cert_pem_start");   // same certificate for MQTT and HTTPS

    esp_mqtt_topic_t topics[2];

    topics[0].filter = strdup(mqtt_topic_sub);
    topics[0].qos = 0;
    topics[1].filter = strdup(mqtt_update_sub);
    topics[1].qos = 1;

    ret = mqtt_start(topics, 2, (char *)&ca_crt_start);

    if (ret != ESP_OK) ESP_LOGE(tag, "mqtt_start() ERROR: %s", esp_err_to_name(ret));
    else ESP_LOGI(tag, "mqtt started successfuly!");
    
    for (int i=0; i<2; i++)
    {
        free((void *)topics[i].filter);
        topics[i].filter = NULL;
    }
    motorQueue = xQueueCreate(10, sizeof(int));

    xTaskCreate(process_input, "Listen & process data", 4096, (void *)motorQueue, 3, NULL);

    logs_queue = xQueueCreateStatic(20, sizeof(LogsMsg_t), (uint8_t *)xPayloadStorage, &static_logs_queue);
    xTaskCreate(send_logs, "Sends loggs", 4096, (void *)logs_queue, 5, NULL);
    

    //                              HTTPS OTA configuration
    /************************************************************************ */
    input_ota_conf_t config = {
        .URL = strdup("https://192.168.0.100/window_roller/latest.bin"),
        .cert = (char *)ca_crt_start,
        .update_interval_h = atoi(OTA_UPDATES_INTERVAL),
        ._blockUpdate = blockUpdate,
    };

    ret = https_ota_init(&config);
    if (ret != ESP_OK) ESP_LOGE(tag, "OTA init error");

    blockUpdate = xSemaphoreCreateMutex();
    xSemaphoreGive(blockUpdate);

    //                              Motor driver configuration
    /************************************************************************ */
    gpio_config_t gpio_conf = {
        .pin_bit_mask = GPIO_OUTPUT_PIN_SEL,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };

    gpio_config(&gpio_conf);

    motor.steps = load_motor_steps();
    
    xTaskCreate(runMotor, "Motor driver", 4096, (void *)motorQueue, 2, NULL);

    motor_sem = xSemaphoreCreateBinary();
    esp_timer_create(&timer_args, &motor_timer);
    esp_timer_create(&log_timer_args, &logs_timer);

    
    //                              BLE scanner configuration
    /************************************************************************ */
    ble_queue = xQueueCreate(5, sizeof(ble_payload_t));
    init_ble_scanner(ble_queue);

    xTaskCreate(send_ble_data, "BLE receiver", 4096, (void *)ble_queue, 2, NULL);

    //                              Version publish
    /************************************************************************ */
    payload_t data = {0};

    char *ver_topic = "smart_home/window_roller/version";

    data.topic = strdup(ver_topic);
    data.retain = 1;
    
    char *ver_msg = getRunningVer();
    data.msg_len = strlen(ver_msg);
    data.msg = ver_msg;

    ret = mqtt_pub(&data);
    ESP_LOGI(tag, "Version publicated: %s", esp_err_to_name(ret));

    free(data.topic);
    free(ver_msg);
    data.msg = NULL;

    //                              Logs
    /************************************************************************ */

    xTaskCreate(heap_stats, "RAM stats publishing", 4096, (void *)logs_queue, 2, NULL);

    /*
    TODO:
    [X] OTA
    [X] Steps counter - logic 
    [X] Stepper motor driver
    [X] OTA new feature -> blocking update while motor is working and program on update
    [ ] logs
    [ ] Receiving timedate from local server and add timestamp to logs
    [X] BLE listening
    [X] Heap stats
    
   */

}


void pubInfo()
{
    esp_err_t ret;
    payload_t data = {0};

    data.topic = strdup(mqtt_topic_pub);

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

                for (int i=0; i<8; i++) 
                {
                    gpio_set_level(sequence[i][0], 0);
                    if (sequence[i][1] != -1) gpio_set_level(sequence[i][1], 0);
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

void send_logs(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    LogsMsg_t data;
    payload_t payload;
    esp_err_t ret;

    payload.topic = strdup(mqtt_logs_topic);

    for (;;)
    {
        if (xQueueReceive(queue, &data, portMAX_DELAY) == pdPASS)
        {
            payload.msg = data.msg;
            payload.msg_len = data.msg_len;

            ret = mqtt_pub(&payload);

            if (ret != ESP_OK)
            {
                ESP_LOGE(tag, "Error while publicating logs!");
            }

            payload.msg = NULL;
        }
    }
}

void send_ble_data(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    ble_payload_t data;
    payload_t mqtt_data[3];
    esp_err_t ret;

    for (int i=0; i<3; i++)
    {
        mqtt_data[i].retain = 1;
        mqtt_data[i].qos = 0;
        mqtt_data[i].msg = malloc(5);
        memset(mqtt_data[i].msg, 0, 5);
    }
    mqtt_data[0].topic = strdup(mqtt_plants_plant1);
    mqtt_data[1].topic = strdup(mqtt_plants_plant2);
    mqtt_data[2].topic = strdup(mqtt_plants_battery);

    for (;;)
    {
        if (xQueueReceive(queue, &data, portMAX_DELAY) == pdPASS)
        {
            snprintf(mqtt_data[0].msg, 5, "%u%%", data.hum1);  // "XXX%" => 4 chars + '\0'
            mqtt_data[0].msg_len = 5;

            snprintf(mqtt_data[1].msg, 5, "%u%%", data.hum2);  // "XXX%" => 4 chars + '\0'
            mqtt_data[1].msg_len = 5;
            
            snprintf(mqtt_data[2].msg, 5, "%u%%", data.bat);  // "XXX%" => 4 chars + '\0'
            mqtt_data[2].msg_len = 5;
            
            for (int i=0; i<3; i++)
            {
                ret = mqtt_pub(&mqtt_data[i]);

                ESP_LOGI(tag, "Payload %d sent status: %s", i, esp_err_to_name(ret));

                memset(mqtt_data[i].msg, 0, 5);
            }
        }
    }
}

void heap_stats(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    uint32_t free_ram, min_free_ram;
    size_t largest_block;

    LogsMsg_t logs[3] = {0};

    for (int i=0; i<3; i++)
    {
        memset(logs[i].msg, 0, 64);
    }
    
    for (;;)
    {
        if (xSemaphoreTake(blockUpdate, 200) == pdPASS)
        {
            for (int i=0; i<3; i++)
            {
                memset(logs[i].msg, 0, 64);
            }       

            free_ram = esp_get_free_heap_size();
            min_free_ram = esp_get_minimum_free_heap_size();
            largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

            logs[0].msg_len = snprintf(logs[0].msg, 64, "Actual free RAM:       %lu bytes", free_ram);
            logs[1].msg_len = snprintf(logs[1].msg, 64, "Largest block:         %zu bytes", largest_block);
            logs[2].msg_len = snprintf(logs[2].msg, 64, "Lowest free RAM:       %lu bytes", min_free_ram);

            xQueueSend(queue, &logs[0], 0);
            xQueueSend(queue, &logs[1], 0);
            xQueueSend(queue, &logs[2], 0);

            xSemaphoreGive(blockUpdate);
        }
        
        
        vTaskDelay(pdMS_TO_TICKS(1000 * 60)); // once every 1min
    }
}