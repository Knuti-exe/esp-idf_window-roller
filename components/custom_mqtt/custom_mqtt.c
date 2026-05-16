#include "custom_mqtt.h"

static const char *tag = "MQTT";
QueueHandle_t ReceivedQueue;
static esp_mqtt_client_handle_t mqtt_client;
static esp_mqtt_topic_t *topics;
static int topics_size;

esp_err_t mqtt_pub(payload_t *buffer)
{
    int msg_id;

    if (buffer->topic)
    {
        msg_id = esp_mqtt_client_publish(mqtt_client, buffer->topic, buffer->msg, buffer->msg_len, buffer->qos, buffer->retain);

        if (msg_id >= 0) return ESP_OK;
        else return ESP_FAIL;
    }
    else return ESP_ERR_INVALID_ARG;
}

payload_t* mqtt_get()
{
    payload_t *buffer = malloc(sizeof(payload_t));
    memset(buffer, 0, sizeof(payload_t));

    xQueueReceive(ReceivedQueue, buffer, portMAX_DELAY);

    return buffer;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    // int msg_id;

    switch((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            int ret;
            ret = esp_mqtt_client_subscribe_multiple(client, topics, topics_size);

            if (ret == -1) ESP_LOGE(tag, "Error - multiple subscriptions");

            for (int i=0; i<topics_size; i++)
            {
                free((void *)topics[i].filter);
            }

            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(tag, "Mqtt disconnected.");

            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(tag, "Msg subscribed successfuly!");

            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(tag, "Msg unsubscribed.");

            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(tag, "Msg sent successfuly!");

            break;
        case MQTT_EVENT_DATA:
            // got msg: msg id, topic, data, length
            // Warning: for large data, msg is fragmented and have to track with `currern_data_offset` and `total_data_len`
            payload_t data = {
                .msg_len = event->data_len,
                .topic_len = event->topic_len,
            };
            data.msg = malloc(event->data_len + 1);       // +1 for '\0' end of string char
            data.topic = malloc(event->topic_len + 1);

            strncpy(data.msg, event->data, event->data_len);
            strncpy(data.topic, event->topic, event->topic_len);

            data.msg[data.msg_len] = '\0';
            data.topic[data.topic_len] = '\0';

            if (xQueueSend(ReceivedQueue, &data, pdMS_TO_TICKS(10)) != pdPASS) 
            {
                ESP_LOGE(tag, "Error: queue full - cannot receive data!");
                free(data.msg);
                free(data.topic);
            }

            ESP_LOGI(tag, "GOT DATA!");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(tag, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(tag, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
                ESP_LOGE(tag, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
                ESP_LOGE(tag, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
                        strerror(event->error_handle->esp_transport_sock_errno));
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(tag, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
            } else {
                ESP_LOGE(tag, "Unknown error type: 0x%x", event->error_handle->error_type);
            }
            break;
        default:
            break;
    }   
}

esp_err_t mqtt_start(esp_mqtt_topic_t *_topics, int size, char *ca_crt_start)
{
    esp_err_t ret;

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address = {
            .uri = "mqtts://192.168.0.100:8883",
        },
        .broker.verification = {
            .certificate = strdup(ca_crt_start),
            .certificate_len = 0,
        },
        .credentials = {
            .username = "custom",
            .authentication.password = "broker#123"
        },
        .network.reconnect_timeout_ms = 1000,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1, // TODO mqtt 5
         
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_client) 
    {
        ESP_LOGE(tag, "Error occured while MQTT client init.");
        return ESP_FAIL;
    }

    ret = esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(tag, "Could not register event to event loop");
        return ret;
    }
    
    ret = esp_mqtt_client_start(mqtt_client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(tag, "Could not start mqtt client");
        return ret;
    }

    ReceivedQueue = xQueueCreate(20, sizeof(payload_t));
    topics = malloc(size * sizeof(esp_mqtt_topic_t));
    topics_size = size;

    for (int i=0; i<size; i++) {
        // char *pTopic = malloc(sizeof(_topics[i]));
        // sprintf(pTopic, _topics[i].filter);
        // topics[i].filter = pTopic;

        topics[i].filter = strdup(_topics[i].filter);

        topics[i].qos = _topics[i].qos;        
    }

    return ESP_OK;
}

esp_err_t mqtt_log(LogsMsg_t *payload, const char *topic)
{
    payload_t msg = {0};
    msg.topic = strdup(topic);
    msg.qos = 0;
    msg.retain = 0;
    msg.topic_len = strlen(topic);
    msg.msg = malloc(strlen(payload->msg));
    // memcpy(msg.msg, payload->msg, sizeof(payload->msg));
    msg.msg = strdup(payload->msg);
    msg.msg_len = payload->msg_len;

    esp_err_t ret = mqtt_pub(&msg);

    free(msg.topic);
    free(msg.msg);
    return ret;
}