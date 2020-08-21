/* iot_device definition for BeerIoTTapSensor
   
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/timer.h"

#include "nvs.h"
#include "nvs_flash.h"

//Include AWS libraries
#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"
#include "aws_iot_shadow_interface.h"

#include "sdkconfig.h"

static const char *TAG = "mqtt";
static const char *TAG_SENSOR = "sensors";

#define GPIO_INPUT_IO_0 16
#define GPIO_INPUT_IO_1 17
#define GPIO_INPUT_IO_2 5
#define GPIO_INPUT_IO_3 27
#define GPIO_INPUT_IO_4 26
#define GPIO_INPUT_IO_5 25

#define SENSOR_INPUT_COUNT 6


#define GPIO_INPUT_PIN_SELECT (1ULL<<GPIO_INPUT_IO_0 | 1ULL<<GPIO_INPUT_IO_1 | 1ULL<<GPIO_INPUT_IO_2 | 1ULL<<GPIO_INPUT_IO_3 | 1ULL<<GPIO_INPUT_IO_4 | 1ULL<<GPIO_INPUT_IO_5 )
#define ESP_INTR_FLAG_DEFAULT 0

#define MAX_LENGTH_OF_UPDATE_JSON_BUFFER 200


#define HOST_ADDRESS_SIZE 255

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

/**
 * @brief Default MQTT HOST URL is pulled from the aws_iot_config.h
 */
static char HostAddress[HOST_ADDRESS_SIZE] = AWS_IOT_MQTT_HOST;

uint32_t port = AWS_IOT_MQTT_PORT;


/**
 * @brief event group definition for wifi events
 */

static EventGroupHandle_t s_wifi_event_group;


extern const uint8_t aws_root_ca_pem_start[] asm("_binary_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");



static xQueueHandle gpio_evt_queue = NULL;
static xQueueHandle aws_message_evt_queue = NULL;

static SemaphoreHandle_t xPourCountMutex;

xTimerHandle timerHndl1Sec;



typedef struct {
    uint32_t pourCount;
    uint8_t tapId;
} pourEventFinished_t;

static uint32_t evt_count[SENSOR_INPUT_COUNT] = {0};

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = 0;
    switch ((uint32_t)arg) {
        case GPIO_INPUT_IO_0:
            gpio_num = 0;
            break;
        case GPIO_INPUT_IO_1:
            gpio_num = 1;
            break;
        case GPIO_INPUT_IO_2:
            gpio_num = 2;
            break;
        case GPIO_INPUT_IO_3:
            gpio_num = 3;
            break;
        case GPIO_INPUT_IO_4:
            gpio_num = 4;
            break;
        case GPIO_INPUT_IO_5:
            gpio_num = 5;
            break;
        default:
            return;
    }
    
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_example(void* arg)
{
    uint32_t io_num;
    xPourCountMutex = xSemaphoreCreateMutex();

    if (xPourCountMutex == NULL) {
        ESP_LOGE(TAG_SENSOR, "Error creating sensor Mutex. Aborting...");
        abort();
    }
    for(;;)
    {
        //If the mutex can be taken, increment the count, otherwise wait for the semaphore
        
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY))
        {
            if( xSemaphoreTake( xPourCountMutex, portMAX_DELAY) == pdTRUE){
                evt_count[io_num]++;
                printf("GPIO[%d] pulse count: %d\n", io_num, evt_count[io_num]);
            }
            xSemaphoreGive( xPourCountMutex );
        }
        
    }
}

static void event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

void init_wifi()
{
    
    tcpip_adapter_init();
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

    wifi_config_t wifi_config = {
            .sta = {
                .ssid = CONFIG_ESP_WIFI_SSID,
                .password = CONFIG_ESP_WIFI_PASSOWRD
            },
    };

    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );

}

void setup_pin_inputs(){
     /* Configure the IOMUX register for pad BLINK_GPIO (some pads are
       muxed to GPIO on reset already, but some default to other
       functions and need to be switched to GPIO. Consult the
       Technical Reference for a list of pads and their default
       functions.)
    */
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_PIN_INTR_POSEDGE;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SELECT;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = 1;

    gpio_config(&io_conf);

    gpio_evt_queue = xQueueCreate(10000, sizeof(uint32_t));

    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

    gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);
    gpio_isr_handler_add(GPIO_INPUT_IO_1, gpio_isr_handler, (void*) GPIO_INPUT_IO_1);
    gpio_isr_handler_add(GPIO_INPUT_IO_2, gpio_isr_handler, (void*) GPIO_INPUT_IO_2);
    gpio_isr_handler_add(GPIO_INPUT_IO_3, gpio_isr_handler, (void*) GPIO_INPUT_IO_3);
    gpio_isr_handler_add(GPIO_INPUT_IO_4, gpio_isr_handler, (void*) GPIO_INPUT_IO_4);
    gpio_isr_handler_add(GPIO_INPUT_IO_5, gpio_isr_handler, (void*) GPIO_INPUT_IO_5);

}

void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;

    if(NULL == pClient) {
        return;
    }

    if(aws_iot_is_autoreconnect_enabled(pClient)) {
        ESP_LOGI(TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    } else {
        ESP_LOGW(TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
        rc = aws_iot_mqtt_attempt_reconnect(pClient);
        if(NETWORK_RECONNECTED == rc) {
            ESP_LOGW(TAG, "Manual Reconnect Successful");
        } else {
            ESP_LOGW(TAG, "Manual Reconnect Failed - %d", rc);
        }
    }
}

void aws_iot_task(void *param) {

    IoT_Error_t rc = FAILURE;
    ESP_LOGI(TAG, "Address of keys %s, %s", (const char *)aws_root_ca_pem_start, (const char *)certificate_pem_crt_start);

    char topic[100] = {0};
    char cPayload[100] = {0};
    ESP_LOGI(TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    //initialize the mqtt client
    AWS_IoT_Client mqttClient;

    IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
    IoT_Client_Connect_Params connectParms = iotClientConnectParamsDefault;

    IoT_Publish_Message_Params paramsQOS1;

    mqttInitParams.enableAutoReconnect = false;
    mqttInitParams.pHostURL = HostAddress;
    mqttInitParams.port = port;

    mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
    mqttInitParams.pDeviceCertLocation = (const char *)certificate_pem_crt_start;
    mqttInitParams.pDevicePrivateKeyLocation = (const char *)private_pem_key_start;
    
    mqttInitParams.mqttCommandTimeout_ms = 20000;
    mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    mqttInitParams.isSSLHostnameVerify = true;
    mqttInitParams.disconnectHandler = disconnectCallbackHandler;
    mqttInitParams.disconnectHandlerData = NULL;

    ESP_LOGI(TAG, "MQTT Init");

    rc = aws_iot_mqtt_init(&mqttClient, &mqttInitParams);
    if (SUCCESS != rc) {
        ESP_LOGE(TAG, "aws_iot_mqtt_init returned error %d, aborting...", rc);
        abort();
    }
    
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        false, true, portMAX_DELAY);

    
    connectParms.keepAliveIntervalInSec = 10;
    connectParms.isCleanSession = true;
    connectParms.MQTTVersion = MQTT_3_1_1;
    
    connectParms.pClientID = CONFIG_AWS_IOT_CLIENT_ID;
    connectParms.clientIDLen = (uint16_t) strlen(CONFIG_AWS_IOT_CLIENT_ID);
    connectParms.isWillMsgPresent = false;

    ESP_LOGI(TAG, "Connecting to AWS...");
    do {
        rc = aws_iot_mqtt_connect(&mqttClient, &connectParms);
        if (SUCCESS != rc) {
            ESP_LOGE(TAG, "Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
            vTaskDelay(1000 / portTICK_RATE_MS);
        }
    } while(SUCCESS != rc);

    rc = aws_iot_mqtt_autoreconnect_set_status(&mqttClient, true);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "Unable to set Auto Reconnect to true - %d, aborting...", rc);
        abort();
    }

    paramsQOS1.qos = QOS1;
    paramsQOS1.payload = (void *) cPayload;
    paramsQOS1.isRetained = 0;

    //Loop and publish change in flowCount and which pin is active
    while(NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc) {
        rc = aws_iot_mqtt_yield(&mqttClient, 100);
        if(NETWORK_ATTEMPTING_RECONNECT == rc ) {
            continue;
        }
        ESP_LOGI(TAG, "=======================================================================================");
        pourEventFinished_t event;
        //For each message publish a count on the corrisponding tap topic
        if (xQueueReceive(aws_message_evt_queue, &event, 100)) {
            
            sprintf(topic, "tapSensor/tap_%02d", event.tapId);
            sprintf(cPayload, "{ \"count\": %d, \"TapId\": \"tap_%02d\" }", event.pourCount, event.tapId);
            ESP_LOGI(TAG, "logging tap change for: %s", topic);
            paramsQOS1.payloadLen = strlen(cPayload);
            rc = aws_iot_mqtt_publish(&mqttClient, topic, strlen(topic), &paramsQOS1);
            if (SUCCESS != rc && MQTT_REQUEST_TIMEOUT_ERROR != rc && NETWORK_ATTEMPTING_RECONNECT != rc && NETWORK_RECONNECTED != rc) {
                ESP_LOGE(TAG, "Error(%d) publishing to MQTT topic: %s, payload: %s", rc, topic, cPayload );
            }
            if (rc == MQTT_REQUEST_TIMEOUT_ERROR) {
                ESP_LOGW(TAG, "Tap update not recieved.  TapId: %d PourCount: %d", event.tapId, event.pourCount);
                rc = SUCCESS;
            }
            

        }

    }
}

void aws_message_create_task(void *param) {
    int i = 0;
    for(;;) {
        
        vTaskDelay(10000 / portTICK_PERIOD_MS);
        ESP_LOGI("AWS_MSG", "checking for input change");
        //Lock the mutex for evt_count as we need to reset values to 0 after queueing message
        if ( xPourCountMutex != NULL && xSemaphoreTake( xPourCountMutex, (TickType_t) 10) == pdTRUE) {
            
            for (i = 0; i < SENSOR_INPUT_COUNT; i++) {           
                //If the tap count has poured anything in the last second, take the value and queue it to send as an aws message
                if (evt_count[i] > 0){
                    ESP_LOGI("AWS_MSG", "Change on tap: %d", i);
                    pourEventFinished_t event;
                    event.pourCount = evt_count[i];
                    event.tapId = i;
                    
                    xQueueSend(aws_message_evt_queue, &event, (TickType_t)10);
                    evt_count[i] = 0;
                }
            }
            xSemaphoreGive(xPourCountMutex);
        }
    }
}

void app_main() {
   
    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    setup_pin_inputs();


    aws_message_evt_queue = xQueueCreate(10000, sizeof(pourEventFinished_t));

    init_wifi();
    
    xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", 9216, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&aws_message_create_task, "1Sec timer publish", 20480, NULL, 10, NULL, 0);
}
