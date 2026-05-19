#include <stdio.h>
#include <string.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <mqtt_client.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <esp_log.h>

typedef struct {
    char *topic;
    int topic_len;
    int msg_len;
    char *msg;
    int retain;
    int qos;
} payload_t;

typedef struct {
    char msg[64];
    int msg_len;
} LogsMsg_t;

/**
 * @brief Initializes MQTTS client
 * @param _topics Array with topics to subscribe
 * @param size Arrays length
 * @param ca_crt_start TLS certificate
 */
esp_err_t mqtt_start(esp_mqtt_topic_t *_topics, int size, char *ca_crt_start);

/**
 * @brief Listens incoming data on subscribed topics
 * @returns Received data
 * @note It will wait untill data arrival
 */
payload_t* mqtt_get();

/**
 * @brief Publicates data
 * @param payload struct which detailed configuration, like QoS, retain flag, topic and message
 * @returns ESP_OK - message sent, ESP_FAIL - message not delivered, 
 * ESP_ERR_INVALID_ARG - topic is empty
 */
esp_err_t mqtt_pub(payload_t *payload);

/**
 * @brief Shorter version of `mqtt_pub()`
 * @param payload struct with message only
 * @param topic optional topic 
 * @returns ESP_OK - message sent, ESP_FAIL - message not delivered, 
 * ESP_ERR_INVALID_ARG - topic is empty
 * @note QoS is set on 0 and will not be retained
 */
esp_err_t mqtt_log(LogsMsg_t *payload, const char *topic);