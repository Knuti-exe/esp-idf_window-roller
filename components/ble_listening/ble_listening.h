#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

typedef struct {
    uint16_t bat;
    uint8_t hum1;
    uint8_t hum2;
    bool brownout;
} ble_payload_t;

/**
 * @brief Initializes NimBLE stack
 * @param ble_queue BLE Queue, where received data will be inserted
 */
void init_ble_scanner(QueueHandle_t ble_queue);

/**
 * @brief Stops BLE listening
 */
void suspend_ble();

/**
 * @brief Resumes BLE listening
 */
void resume_ble();