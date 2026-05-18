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
esp_err_t load_config();
void save_config();
void send_logs(void *pvParameters);
void send_ble_data(void *pvParameters);
void heap_stats(void *pvParameters);

const char *tag = "WIN_ROLL";
const gpio_num_t pins[4] = {GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_10};
const char *topic_roller_state_pub = "smart_home/window_roller/state";          // Roller position (ACK) 
const char *topic_motor_sub = "smart_home/window_roller/motor";                 // Motor position (command)
const char *topic_force_ota_sub = "smart_home/window_roller/update";            // Msg "true" will try to force OTA update
const char *topic_logs_pub = "smart_home/window_roller/logs";                   // Logs (optional for debugging)
const char *topic_plant1_pub = "smart_home/window_roller/plants/plant1";        // Received and retransmitted data from BLE sensor
const char *topic_plant2_pub = "smart_home/window_roller/plants/plant2";        // As above
const char *topic_battery_pub = "smart_home/window_roller/plants/battery";      // As above
const char *topic_set_motor_sub = "smart_home/window_roller/set_motor";         // Roller position override ("up", "half", "down")
const char *topic_ram_log_sub = "smart_home/window_roller/ram_logging";         // Switch for RAM stats - turned on by default
const char *topic_steps_pub = "smart_home/window_roller/steps";                 // Motor steps (shared once every 20 sec)
const int ROLLER_UP = 0;
const int ROLLER_HALF = 40000;
const int ROLLER_DOWN = 86000;
static bool cancel_rollback = false;
volatile int target_step = 0;
int seq_index = 0;
int log_index = 0;
static bool logging_ram = true;


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
TaskHandle_t ram_task_handle;

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

    esp_mqtt_topic_t topics[4];

    topics[0].filter = strdup(topic_motor_sub);
    topics[0].qos = 1;
    topics[1].filter = strdup(topic_force_ota_sub);
    topics[1].qos = 1;
    topics[2].filter = strdup(topic_set_motor_sub);
    topics[2].qos = 1;
    topics[3].filter = strdup(topic_ram_log_sub);
    topics[3].qos = 1;
    
    ret = mqtt_start(topics, 4, (char *)&ca_crt_start);

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
    blockUpdate = xSemaphoreCreateMutex();
    xSemaphoreGive(blockUpdate);
    
    input_ota_conf_t config = {
        .URL = strdup("https://192.168.0.100/window_roller/latest.bin"),
        .cert = (char *)ca_crt_start,
        .update_interval_h = atoi(OTA_UPDATES_INTERVAL),
        ._blockUpdate = blockUpdate,
        .logs_queue = logs_queue
    };

    LogsMsg_t log_msg = {0};
    
    ret = https_ota_init(&config);
    if (ret != ESP_OK) 
    {
        ESP_LOGE(tag, "OTA init error");
        snprintf(log_msg.msg, 64, "OTA init: Error");
    }
    else
    {
        snprintf(log_msg.msg, 64, "OTA init: OK");
    }
    xQueueSend(logs_queue, &log_msg, 0);    

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

    ret = load_config();
    if (ret != ESP_OK)
    {
        ESP_LOGE(tag, "Could not read config!");
    }    

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

    xTaskCreate(heap_stats, "RAM stats publishing", 4096, (void *)logs_queue, 2, &ram_task_handle);

    if (!logging_ram) vTaskSuspend(ram_task_handle);
    

    /*
    TODO:
    [X] OTA
    [X] Steps counter - logic 
    [X] Stepper motor driver
    [X] OTA new feature -> blocking update while motor is working and program on update
    [ ] logs - in one, simple function
    [X] Receiving timedate from local server and add timestamp to logs - done on local python script
    [X] BLE listening
    [X] Heap stats
    
   */

}


void pubInfo()
{
    esp_err_t ret;
    payload_t data = {0};

    data.topic = strdup(topic_roller_state_pub);

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
            esp_timer_start_periodic(logs_timer, 2000000);  // 20 sec

            if (xSemaphoreTake(motor_sem, portMAX_DELAY) == pdPASS)
            {
                LogsMsg_t log_msg = {0};
                snprintf(log_msg.msg, 64, "Motor state: in desired position");
                log_msg.msg_len = strlen(log_msg.msg);
                
                xQueueSend(logs_queue, &log_msg, 0);
                ESP_LOGI(tag, "Motor state: in desired position");

                esp_timer_stop(logs_timer);

                pubInfo();

                save_config();

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
    LogsMsg_t log_msg = {0};
    
    snprintf(log_msg.msg, 64, "Motor steps: %ld", motor.steps);
    log_msg.msg_len = strlen(log_msg.msg);
                
    xQueueSend(logs_queue, &log_msg, 0);
    ESP_LOGI(tag, "Motor steps: %ld", motor.steps);

    mqtt_log(&log_msg, topic_steps_pub);
}

void process_input(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    payload_t *data = malloc(sizeof(payload_t));
    data->msg = NULL;
    data->topic = NULL;
    for (;;)
    {
        if (data->msg) free(data->msg);   // #TODO moze wywalic
        if (data->topic) free(data->topic);

        data = mqtt_get(); // blocks code

        ESP_LOGI(tag, "Got topic:msg - %.*s:%.*s", data->topic_len, data->topic, data->msg_len, data->msg);
        
        if (strcmp(data->topic, topic_motor_sub) == 0)
        {
            if (strcmp(data->msg, "up") == 0) 
            {
                ESP_LOGI(tag, "Going up");

                if (xQueueSend(queue, &ROLLER_UP, 100) != pdPASS)
                {
                    ESP_LOGW(tag, "Queue full - waiting for pending operations...");
                }
            }
            else if (strcmp(data->msg, "half") == 0)
            {
                ESP_LOGI(tag, "Going half-way");

                if (xQueueSend(queue, &ROLLER_HALF, 100) != pdPASS)
                {
                    ESP_LOGW(tag, "Queue full - waiting for pending operations...");
                }
                
            }
            else if (strcmp(data->msg, "down") == 0)
            {
                ESP_LOGI(tag, "Going down");

                if (xQueueSend(queue, &ROLLER_DOWN, 100) != pdPASS)
                {
                    ESP_LOGW(tag, "Queue full - waiting for pending operations...");
                }
            }
            else continue;
        }
        else if (strcmp(data->topic, topic_force_ota_sub) == 0)
        {
            if (strcmp(data->msg, "true") == 0 && xSemaphoreTake(blockUpdate, 0) == pdPASS)
            {
                xSemaphoreGive(blockUpdate);
                ESP_LOGW(tag, "Trying to update software manually...");
                force_ota_update();
            }
        }
        else if (strcmp(data->topic, topic_set_motor_sub) == 0)
        {
            if (strcmp(data->msg, "up") == 0) motor.steps = ROLLER_UP;
            else if (strcmp(data->msg, "half") == 0) motor.steps = ROLLER_HALF;
            else if (strcmp(data->msg, "down") == 0) motor.steps = ROLLER_DOWN;
            else continue;
            target_step = motor.steps;
            save_config();
        }
        else if (strcmp(data->topic, topic_ram_log_sub) == 0)
        {
            if (strcmp(data->msg, "true") == 0)
            {
                LogsMsg_t log_msg = {0};
    
                snprintf(log_msg.msg, 64, "FREE RAM: Logging turned ON");
                log_msg.msg_len = strlen(log_msg.msg);
                            
                xQueueSend(logs_queue, &log_msg, 0);

                vTaskResume(ram_task_handle);
            } 
            else if (strcmp(data->msg, "false") == 0) 
            {
                LogsMsg_t log_msg = {0};
    
                snprintf(log_msg.msg, 64, "FREE RAM: Logging turned OFF");
                log_msg.msg_len = strlen(log_msg.msg);
                            
                xQueueSend(logs_queue, &log_msg, 0);

                vTaskSuspend(ram_task_handle);
            }
            else continue;
            
            save_config();
        }

        
    }    
}

/*
    motor_data:
        uint32_t steps;
    ram_logging:
        bool state;
*/

void save_config() 
{
    nvs_handle_t handle;
    esp_err_t err;

    eTaskState state = eTaskGetState(ram_task_handle);
    int8_t running = state == eSuspended ? 0 : 1;

    ESP_LOGI(tag, "Saving log_state: %d", running);

    err = nvs_open("motor_data", NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(tag, "Error while opening NVS namespace: %s\n", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u32(handle, "steps", motor.steps);
    if (err != ESP_OK) printf("Error while saving!\n");

    err = nvs_open("ram_logging", NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(tag, "Error while opening NVS namespace: %s\n", esp_err_to_name(err));
        return;
    }
    err = nvs_set_i8(handle, "state", running);
    if (err != ESP_OK) printf("Error while saving!\n");
    
    err = nvs_commit(handle);
    if (err != ESP_OK) ESP_LOGE(tag, "Could not commit save to NVS!\n");

    nvs_close(handle);
}

esp_err_t load_config() {
    nvs_handle_t handle_motor, handle_ram;
    uint32_t steps = 0; 
    esp_err_t ret;
    int8_t state = 0;

    if (nvs_open("motor_data", NVS_READONLY, &handle_motor) == ESP_OK)
    {
        ret = nvs_get_u32(handle_motor, "steps", &steps);
        nvs_close(handle_motor);
    }
    else 
    {
        ESP_LOGE(tag, "NVS: ERROR - problem with opening \"motor_data\" namespace");
        return ESP_FAIL;
    }
    
    if (ret != ESP_OK)
    {
        ESP_LOGE(tag, "NVS: ERROR - problem with reading \"motor_data/steps\" attr");
        return ret;
    } 

    motor.steps = steps;

    if (nvs_open("ram_logging", NVS_READONLY, &handle_ram) == ESP_OK)
    {
        ret = nvs_get_i8(handle_ram, "state", &state);
        nvs_close(handle_ram);
    }
    else 
    {
        ESP_LOGE(tag, "NVS: ERROR - problem with opening \"ram_logging\" namespace");
        return ESP_FAIL;
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(tag, "NVS: ERROR - problem with reading \"ram_logging/state\" attr");
        return ret;
    } 

    ESP_LOGI(tag, "Reading log_state: %d", state);

    logging_ram = state == 0 ? false : true;

    return ESP_OK;
}

void send_logs(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    LogsMsg_t data;
    payload_t payload;
    esp_err_t ret;

    payload.topic = strdup(topic_logs_pub);

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
    mqtt_data[0].topic = strdup(topic_plant1_pub);
    mqtt_data[1].topic = strdup(topic_plant2_pub);
    mqtt_data[2].topic = strdup(topic_battery_pub);

    for (;;)
    {
        if (xQueueReceive(queue, &data, portMAX_DELAY) == pdPASS)
        {
            snprintf(mqtt_data[0].msg, 5, "%u%%", data.hum1);  // "XXX%" => 4 chars + '\0'
            mqtt_data[0].msg_len = 5;

            snprintf(mqtt_data[1].msg, 5, "%u%%", data.hum2);  // "XXX%" => 4 chars + '\0'
            mqtt_data[1].msg_len = 5;
            
            snprintf(mqtt_data[2].msg, 9, "%u mV", (unsigned int)data.bat);  // "XXXXX mV" => max 8 chars + '\0'
            mqtt_data[2].msg_len = 9;
            
            for (int i=0; i<3; i++)
            {
                ret = mqtt_pub(&mqtt_data[i]);

                ESP_LOGI(tag, "Payload %d sent status: %s", i, esp_err_to_name(ret));

                memset(mqtt_data[i].msg, 0, 5);
            }

            if (data.brownout == true)
            {
                LogsMsg_t msg = {0};
                snprintf(msg.msg, 64, "SENSOR: BROWNOUT DETECTED!");
                msg.msg_len = strlen(msg.msg);

                xQueueSend(logs_queue, &msg, 0);
            }
            
        }
    }
}

void heap_stats(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t) pvParameters;
    uint32_t free_ram, min_free_ram;
    size_t largest_block;

    LogsMsg_t log_msg = {0};
    
    for (;;)
    {
        if (xSemaphoreTake(blockUpdate, 200) == pdPASS)
        {
            memset(log_msg.msg, 0, 64);

            free_ram = esp_get_free_heap_size();
            min_free_ram = esp_get_minimum_free_heap_size();
            largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

            snprintf(log_msg.msg, 64, "Free RAM: %lub; %zub; %lub", free_ram, largest_block, min_free_ram);

            xQueueSend(queue, &log_msg, 0);

            xSemaphoreGive(blockUpdate);
        }  
        vTaskDelay(pdMS_TO_TICKS(1000 * 60)); // once every 1min
    }
}