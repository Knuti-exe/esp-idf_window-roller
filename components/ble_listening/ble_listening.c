#include "ble_listening.h"

QueueHandle_t ble_data_queue = NULL;
static bool ble_running = false;
// static const uint8_t plants_mac[6] = {0xac, 0xa7, 0x04, 0xb9, 0x1e, 0xc6};
static const uint8_t plants_mac_reverse[6] = {0xc6, 0x1e, 0xb9, 0x04, 0xa7, 0xac};
static uint8_t last_msg_id = 0xab;

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg) 
{    
    if (event->type == BLE_GAP_EVENT_DISC) 
    {
        struct ble_gap_disc_desc *disc = &event->disc;
        
        if (memcmp(disc->addr.val, plants_mac_reverse, 6) == 0)  // PLANTS
        {
            struct ble_hs_adv_fields fields;
            uint8_t temp_last_msg_id = 0;
            
            int rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
            
            ble_payload_t payload = {
                .bat = 0x0000,
                .hum1 = 0xff,
                .hum2 = 0xff,
                .brownout = false
            };

            if (rc == 0 && fields.mfg_data != NULL && fields.mfg_data_len > 6)
            {
                int len = fields.mfg_data_len;

                temp_last_msg_id = fields.mfg_data[len - 1];
                
                payload.brownout = fields.mfg_data[len - 2] == 0x01 ? true : false;
                payload.bat = (fields.mfg_data[len - 4] << 8) | fields.mfg_data[len - 3];
                payload.hum2 = fields.mfg_data[len - 5];
                payload.hum1 = fields.mfg_data[len - 6];
            }

            if (temp_last_msg_id != last_msg_id) xQueueSend(ble_data_queue, &payload, 0);

            last_msg_id = temp_last_msg_id;
        }
    }
    
    return 0;
}

static void start_ble_scan(void) 
{
    struct ble_gap_disc_params scan_params;    
    memset(&scan_params, 0, sizeof(scan_params));
    
    scan_params.itvl = 80;   // Interval (50ms / 0.625ms)
    scan_params.window = 48; // Window (30ms / 0.625ms)
    
    scan_params.passive = 1; 
    scan_params.limited = 0;
    
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, 
                          &scan_params, ble_gap_event_handler, NULL);
                          
    if (rc != 0) 
    {
        ESP_LOGE("BLE", "BLE scanner did not start!");    
    }
}

static void ble_on_sync(void) 
{
    start_ble_scan(); 
}

void ble_host_task(void *param) 
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void init_ble_scanner(QueueHandle_t _ble_queue)
{
    ble_running = true;

    ble_data_queue = _ble_queue;

    nimble_port_init();

    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
}

void suspend_ble() 
{
    if (ble_running) 
    {
        ble_gap_disc_cancel(); 
        nimble_port_stop();
        nimble_port_deinit();
        ble_running = false;
    }
}

void resume_ble() 
{
    if (!ble_running) 
    {
        init_ble_scanner(ble_data_queue);
        ble_running = true;
    }
}