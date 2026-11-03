// lib/aws/app_aws.c -- cloud sync using Firebase Realtime Database (REST API)
// Same public API as the AWS version, so main.c and app_hw do not change.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "app_aws.h"

#define TAG "appaws"

// FIREBASE_URL comes from platformio.ini build_flags, e.g.
//   "https://your-project-default-rtdb.firebaseio.com"
// FIREBASE_AUTH is optional (database secret / token). Leave empty if the
// database rules allow access.
#ifndef FIREBASE_AUTH
#define FIREBASE_AUTH ""
#endif

#define POLL_MS 1000

static SemaphoreHandle_t aws_guard;
static fan_state_changed_f fan_state_changed_cb = NULL;
static int last_desired = -1;

static char resp_buf[64];
static int resp_len = 0;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0)
    {
        int n = evt->data_len;
        if (resp_len + n >= (int)sizeof(resp_buf))
            n = sizeof(resp_buf) - 1 - resp_len;
        if (n > 0)
        {
            memcpy(resp_buf + resp_len, evt->data, n);
            resp_len += n;
            resp_buf[resp_len] = '\0';
        }
    }
    return ESP_OK;
}

static void build_url(char *out, size_t len, const char *path)
{
    if (strlen(FIREBASE_AUTH) > 0)
        snprintf(out, len, "%s%s.json?auth=%s", FIREBASE_URL, path, FIREBASE_AUTH);
    else
        snprintf(out, len, "%s%s.json", FIREBASE_URL, path);
}

// returns HTTP status, or -1 on failure
static int firebase_request(const char *path, esp_http_client_method_t method, const char *body)
{
    char url[256];
    build_url(url, sizeof(url), path);
    resp_len = 0;
    resp_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL)
        return -1;

    if (body != NULL)
    {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, strlen(body));
    }

    int status = -1;
    if (esp_http_client_perform(c) == ESP_OK)
        status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return status;
}

void appaws_init(fan_state_changed_f cb)
{
    fan_state_changed_cb = cb;
    aws_guard = xSemaphoreCreateMutex();
}

// Task: poll /fan/desired and apply changes to the relays
void appaws_connect(void *param)
{
    ESP_LOGI(TAG, "connected, watching %s", FAN_PATH_DESIRED);

    while (1)
    {
        if (xSemaphoreTake(aws_guard, 100) == pdTRUE)
        {
            int status = firebase_request(FAN_PATH_DESIRED, HTTP_METHOD_GET, NULL);
            if (status == 200)
            {
                if (strcmp(resp_buf, "null") != 0 && resp_len > 0)
                {
                    int val = atoi(resp_buf);
                    if (val != last_desired)
                    {
                        last_desired = val;
                        ESP_LOGI(TAG, "desired powerlevel = %d", val);
                        if (fan_state_changed_cb)
                            fan_state_changed_cb((uint8_t)val);   // -> apphw_set_state()
                    }
                }
            }
            else
            {
                ESP_LOGW(TAG, "poll failed: %d", status);
            }
            xSemaphoreGive(aws_guard);
        }
        vTaskDelay(POLL_MS / portTICK_PERIOD_MS);
    }
}

// Called by the hardware library when a physical button is pressed
void appaws_publish(uint8_t val)
{
    char body[8];
    snprintf(body, sizeof(body), "%d", val);

    if (xSemaphoreTake(aws_guard, portMAX_DELAY) == pdTRUE)
    {
        int s1 = firebase_request(FAN_PATH_REPORTED, HTTP_METHOD_PUT, body);
        // also update desired so the poll loop doesn't undo the button press
        last_desired = val;
        int s2 = firebase_request(FAN_PATH_DESIRED, HTTP_METHOD_PUT, body);
        xSemaphoreGive(aws_guard);

        if (s1 != 200 || s2 != 200)
            ESP_LOGE(TAG, "publish failed: %d / %d", s1, s2);
    }
}