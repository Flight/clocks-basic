#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <string.h>
#include <cJSON.h>

#include "sdkconfig.h"
#include "global_event_group.h"

#include "temperature_from_api.h"

float global_outside_temperature;

static const char *TAG = "Weather API";

static const char *WEATHER_API_URL = "https://api.weatherapi.com/v1/";
static const char *WEATHER_API_KEY = CONFIG_WEATHER_API_KEY;
static const char *WEATHER_CITY = CONFIG_WEATHER_CITY;

static const uint8_t REFRESH_INTERVAL_MINS = 5;
static const uint8_t RETRY_INTERVAL_SECS = 10;
static const uint8_t MAX_RETRIES = 10;

extern const char api_weatherapi_com_pem_start[] asm("_binary_api_weatherapi_com_pem_start");
extern const char api_weatherapi_com_pem_end[] asm("_binary_api_weatherapi_com_pem_end");

#define MAX_HTTP_OUTPUT_BUFFER 2048
#define TEMPERATURE_ERROR_CODE -1000

static uint8_t retry_count = 0;
static float temperature_from_json = TEMPERATURE_ERROR_CODE;

static float get_temperature_from_json(char *json_string)
{
  float temperature = TEMPERATURE_ERROR_CODE;

  ESP_LOGI(TAG, "JSON string: %s", json_string);

  cJSON *response_json = cJSON_Parse(json_string);
  if (response_json != NULL)
  {
    cJSON *current = cJSON_GetObjectItem(response_json, "current");
    if (current != NULL)
    {
      cJSON *temperature_json = cJSON_GetObjectItem(current, "temp_c");
      if (temperature_json != NULL && cJSON_IsNumber(temperature_json))
      {
        float temperature = temperature_json->valuedouble;
        return temperature;
      }
      else
      {
        ESP_LOGE(TAG, "Failed to get 'temp_c' or it's not a number");
      }
    }
    else
    {
      ESP_LOGE(TAG, "Failed to get 'current' from JSON response");
    }
    cJSON_Delete(response_json);
  }
  else
  {
    ESP_LOGE(TAG, "Failed to parse JSON response");
  }

  return temperature;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
  static char *output_buffer; // Buffer to store response of http request from event handler
  static int output_len;      // Stores number of bytes read

  switch (evt->event_id)
  {
  case HTTP_EVENT_ERROR:
    ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
    break;

  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
    break;

  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
    break;

  case HTTP_EVENT_ON_HEADER:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
    break;

  case HTTP_EVENT_ON_DATA:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    // Clean the buffer in case of a new request
    if (output_len == 0 && evt->user_data)
    {
      memset(evt->user_data, 0, MAX_HTTP_OUTPUT_BUFFER);
    }

    int copy_len = 0;
    if (evt->user_data)
    {
      copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
      if (copy_len)
      {
        memcpy(evt->user_data + output_len, evt->data, copy_len);
      }
    }
    else
    {
      if (esp_http_client_is_chunked_response(evt->client))
      {
        ESP_LOGI(TAG, "The data is chunked");
        // If chunked, reallocate buffer to accumulate more data
        output_buffer = realloc(output_buffer, output_len + evt->data_len + 1);
        if (!output_buffer)
        {
          ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
          return ESP_FAIL;
        }
        copy_len = evt->data_len;
      }
      else
      {
        ESP_LOGI(TAG, "The data is not chunked");
        int content_len = esp_http_client_get_content_length(evt->client);
        if (output_buffer == NULL)
        {
          output_buffer = (char *)calloc(content_len + 1, sizeof(char));
          output_len = 0;
          if (output_buffer == NULL)
          {
            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
            return ESP_FAIL;
          }
        }
        copy_len = MIN(evt->data_len, (content_len - output_len));
      }
      if (copy_len)
      {
        memcpy(output_buffer + output_len, evt->data, copy_len);
      }
    }
    output_len += copy_len;
    break;

  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
    if (output_buffer != NULL)
    {
      temperature_from_json = get_temperature_from_json(output_buffer);
      if (temperature_from_json == TEMPERATURE_ERROR_CODE || strlen(output_buffer) == 0)
      {
        ESP_LOGE(TAG, "Can't get temperature");
      }
      else
      {
        global_outside_temperature = temperature_from_json;
        ESP_LOGI(TAG, "Temperature is %f", global_outside_temperature);
        xEventGroupSetBits(global_event_group, IS_OUTSIDE_TEMPERATURE_READING_DONE_BIT);
      }

      free(output_buffer);
      output_buffer = NULL;
    }
    else
    {
      ESP_LOGE(TAG, "Output buffer is 0!");
    }
    output_len = 0;
    break;

  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
    int mbedtls_err = 0;
    esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
    if (err != 0)
    {
      ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
      ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
    }
    if (output_buffer != NULL)
    {
      free(output_buffer);
      output_buffer = NULL;
    }
    output_len = 0;
    break;

  case HTTP_EVENT_REDIRECT:
    ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
    break;
  }

  return ESP_OK;
}

void temperature_from_api_task(void *pvParameter)
{
  char full_url[256];
  sprintf(full_url, "%scurrent.json?key=%s&q=%s&aqi=no", WEATHER_API_URL, WEATHER_API_KEY, WEATHER_CITY);

  esp_http_client_config_t config = {
      .url = full_url,
      .event_handler = _http_event_handler,
      .cert_pem = api_weatherapi_com_pem_start,
      .timeout_ms = 20000,
      .buffer_size = 4096};

  while (true)
  {
    xEventGroupClearBits(global_event_group, IS_OUTSIDE_TEMPERATURE_READING_DONE_BIT);

    ESP_LOGI(TAG, "Waiting for Wi-Fi");
    xEventGroupWaitBits(global_event_group, IS_WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    esp_http_client_handle_t client = esp_http_client_init(&config);

    while (retry_count < MAX_RETRIES)
    {
      esp_http_client_perform(client);

      if (temperature_from_json != TEMPERATURE_ERROR_CODE)
      {
        break;
      }

      ESP_LOGW(TAG, "HTTP GET request failed (attempt %d)", retry_count + 1);
      vTaskDelay(1000 * RETRY_INTERVAL_SECS / portTICK_PERIOD_MS);
      retry_count++;
    }

    esp_http_client_cleanup(client);
    retry_count = 0;
    vTaskDelay(1000 * 60 * REFRESH_INTERVAL_MINS / portTICK_PERIOD_MS);
  }
}