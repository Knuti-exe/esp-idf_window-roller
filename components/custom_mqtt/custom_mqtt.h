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

esp_err_t mqtt_start(esp_mqtt_topic_t *_topics, int size, char *ca_crt_start);
payload_t* mqtt_get();
esp_err_t mqtt_pub(payload_t *payload);
esp_err_t mqtt_log(LogsMsg_t *payload, const char *topic);