#pragma once

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

#define queue_log(...) queue_log_fun((fun_args){__VA_ARGS__})

typedef struct {
    volatile uint32_t steps;
} motor_t;

typedef struct {
    char *msg;
    int lvl;
} fun_args;

/**
 * @brief Processing input from subscribed topics
 */
void process_input(void *pvParameters);

/**
 * @brief One half-step for stepper motor
 */
void runMotor(void *pvParameters);

/**
 * @brief Stepper motor timer function
 */
void motor_timer_callback(void* arg);

/**
 * @brief Timer function which logs `motor.state` every 20 secs
 */
void log_timer_callback(void* arg);

/**
 * @brief Loads `motor.steps` and `heap_stats()` logging state.
 * @returns ESP_OK - loaded data successfuly,
 * ESP_FAIL - Error while opening NVS namespace
 */
esp_err_t load_config();

/**
 * @brief Saves `motor.steps` and `heap_stats()` logging state.
 */
void save_config();

/**
 * @brief Asynchronous queuing task, which sends logs to broker.
 */
void send_logs(void *pvParameters);

/**
 * @brief Sends received BLE data via MQTT protocol.
 */
void send_ble_data(void *pvParameters);

/**
 * @brief Logs heap statistics (actual free heap, largest free block, historical minimum).
*/
void heap_stats(void *pvParameters);

/**
 * @brief Packs and sends log message to `logs_queue`, which will be processed in `send_logs()` 
 *
 * @param msg   message to be send. Cannot be empty.
 * @param lvl   Message tag (0 - INFO;  1 - OK;  2 - WARNING;  3 - ERROR)
 */
void queue_log_fun(fun_args args);

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
static const char *log_lvl[] = {"[INFO]", "[OK]", "[WARNING]", "[ERROR]"};

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
