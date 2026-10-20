#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"
#include "aws_iot_shadow_interface.h"

#include "app_aws.h"

#define TAG "appaws"

// Cryptographic files embedded via board_build.embed_txtfiles
extern const uint8_t private_pem_key_start[]     asm("_binary_private_pem_key_start");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t aws_root_ca_pem_start[]     asm("_binary_AmazonRootCA1_pem_start");

static AWS_IoT_Client aws_client;
static SemaphoreHandle_t aws_guard;   // shadow functions are not thread-safe
static fan_state_changed_f fan_state_changed_cb = NULL;
static uint8_t desired_state = 0;

static const char *endpoint_address = AWS_ENDPOINT;  // from build_flags
static const char *thing_name = AWS_THING_NAME;
static const char *client_id = AWS_THING_NAME;

void appaws_init(fan_state_changed_f cb)
{
    fan_state_changed_cb = cb;
    aws_guard = xSemaphoreCreateMutex();
}

static void disconnected_handler(AWS_IoT_Client *pClient, void *data)
{
    ESP_LOGW(TAG, "MQTT disconnected");
}

// Runs when a voice command changes the desired powerlevel
static void fan_powerlevel_change_requested(const char *pJsonString,
                                            uint32_t JsonStringDataLen,
                                            jsonStruct_t *pContext)
{
    if (pContext != NULL)
    {
        uint8_t val = *(uint8_t *)(pContext->pData);
        ESP_LOGI(TAG, "delta received, powerlevel = %d", val);
        desired_state = val;
        if (fan_state_changed_cb)
            fan_state_changed_cb(val);   // calls apphw_set_state()
    }
}

void appaws_connect(void *param)
{
    memset((void *)&aws_client, 0, sizeof(aws_client));

    ShadowInitParameters_t sp = ShadowInitParametersDefault;
    sp.pHost = (char *)endpoint_address;
    sp.port = AWS_IOT_MQTT_PORT;
    sp.pClientCRT = (const char *)certificate_pem_crt_start;
    sp.pClientKey = (const char *)private_pem_key_start;
    sp.pRootCA = (const char *)aws_root_ca_pem_start;
    sp.disconnectHandler = disconnected_handler;
    aws_iot_shadow_init(&aws_client, &sp);

    ShadowConnectParameters_t scp = ShadowConnectParametersDefault;
    scp.pMyThingName = thing_name;
    scp.pMqttClientId = client_id;
    scp.mqttClientIdLen = (uint16_t)strlen(client_id);

    while (aws_iot_shadow_connect(&aws_client, &scp) != SUCCESS)
    {
        ESP_LOGW(TAG, "trying to connect");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    uint8_t fan_powerlevel = 0;
    jsonStruct_t fan_controller;
    fan_controller.cb = fan_powerlevel_change_requested;
    fan_controller.pData = &fan_powerlevel;
    fan_controller.pKey = "powerlevel";
    fan_controller.type = SHADOW_JSON_UINT8;
    fan_controller.dataLength = sizeof(uint8_t);

    if (aws_iot_shadow_register_delta(&aws_client, &fan_controller) == SUCCESS)
    {
        ESP_LOGI(TAG, "shadow delta registered");
    }

    IoT_Error_t err = SUCCESS;
    while (1)
    {
        if (xSemaphoreTake(aws_guard, 100) == pdTRUE)
        {
            err = aws_iot_shadow_yield(&aws_client, 250);
            xSemaphoreGive(aws_guard);
        }
        if (err != SUCCESS)
        {
            ESP_LOGE(TAG, "yield failed: %d", err);
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// Called by the hardware library when a button is pressed
void appaws_publish(uint8_t val)
{
    jsonStruct_t temp_json = {
        .cb = NULL,
        .pKey = "powerlevel",
        .pData = &val,
        .type = SHADOW_JSON_UINT8,
        .dataLength = sizeof(val)};

    char jsondoc_buffer[200];
    aws_iot_shadow_init_json_document(jsondoc_buffer, sizeof(jsondoc_buffer));
    aws_iot_shadow_add_reported(jsondoc_buffer, sizeof(jsondoc_buffer), 1, &temp_json);

    // Update desired too, so AWS doesn't send a delta that undoes the button press
    if (desired_state != val)
    {
        aws_iot_shadow_add_desired(jsondoc_buffer, sizeof(jsondoc_buffer), 1, &temp_json);
        desired_state = val;
    }
    aws_iot_finalize_json_document(jsondoc_buffer, sizeof(jsondoc_buffer));

    IoT_Error_t err = SUCCESS;
    if (xSemaphoreTake(aws_guard, portMAX_DELAY) == pdTRUE)
    {
        err = aws_iot_shadow_update(&aws_client, thing_name, jsondoc_buffer, NULL, NULL, 4, true);
        xSemaphoreGive(aws_guard);
    }
    if (err != SUCCESS)
    {
        ESP_LOGE(TAG, "publish failed: %d", err);
    }
}