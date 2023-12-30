#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_spiffs.h>

#include "global_event_group.h"

static const char *TAG = "Webserver";

extern const unsigned char html_index_html_start[] asm("_binary_index_html_start");
extern const unsigned char html_index_html_end[] asm("_binary_index_html_end");

#define SCRATCH_BUFSIZE 1024

struct file_server_data
{
  /* Scratch buffer for temporary storage during file transfer */
  char scratch[SCRATCH_BUFSIZE];
};
static struct file_server_data *server_data = NULL;

static void replace_placeholder(char *buffer, size_t buffer_size, const char *placeholder, const char *replacement)
{
  char *pos = strstr(buffer, placeholder);
  if (pos)
  {
    size_t len_before = pos - buffer;
    size_t len_placeholder = strlen(placeholder);
    size_t len_replacement = strlen(replacement);
    size_t len_after = strlen(pos + len_placeholder);

    if (len_before + len_replacement + len_after < buffer_size)
    {
      memmove(pos + len_replacement, pos + len_placeholder, len_after + 1); // +1 for null terminator
      memcpy(pos, replacement, len_replacement);
    }
  }
}

static esp_err_t get_index_page(httpd_req_t *req)
{
  size_t index_html_size = html_index_html_end - html_index_html_start;
  char *html_page = malloc(index_html_size + 1);
  if (!html_page)
  {
    return ESP_ERR_NO_MEM;
  }

  memcpy(html_page, html_index_html_start, index_html_size);
  html_page[index_html_size] = '\0';

  // Ensure response_data buffer is large enough for the formatted content
  size_t response_data_size = index_html_size + 500 + LOG_BUFFER_SIZE;
  char *response_data = malloc(response_data_size);
  if (!response_data)
  {
    // Handle malloc failure
    free(html_page);
    return ESP_ERR_NO_MEM;
  }

  char inside_temperature_string[20];
  if (global_inside_temperature == TEMPERATURE_ERROR_CODE)
  {
    strcpy(inside_temperature_string, "N/A");
  }
  else
  {
    sprintf(inside_temperature_string, "%.2f°C", global_inside_temperature);
  }

  char outside_temperature_string[20];
  if (global_outside_temperature == TEMPERATURE_ERROR_CODE)
  {
    strcpy(outside_temperature_string, "N/A");
  }
  else
  {
    sprintf(outside_temperature_string, "%.2f°C", global_outside_temperature);
  }

  // Convert integer values to strings before using them in replace_placeholder
  char light_level_string[20];
  sprintf(light_level_string, "%d", global_light_level_index + 1);
  char light_levels_amount_string[20];
  sprintf(light_levels_amount_string, "%d", LIGHT_LEVELS_AMOUNT);

  strcpy(response_data, html_page);

  replace_placeholder(response_data, response_data_size, "{INSIDE_TEMPERATURE}", inside_temperature_string);
  replace_placeholder(response_data, response_data_size, "{OUTSIDE_TEMPERATURE}", outside_temperature_string);
  replace_placeholder(response_data, response_data_size, "{LIGHT_LEVEL}", light_level_string);
  replace_placeholder(response_data, response_data_size, "{LIGHT_LEVELS_AMOUNT}", light_levels_amount_string);
  replace_placeholder(response_data, response_data_size, "{FIRMWARE_VERSION}", global_running_firmware_version);
  replace_placeholder(response_data, response_data_size, "{LOGS}", global_log_buffer);

  esp_err_t response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);

  free(html_page);
  free(response_data);

  return response;
}

static esp_err_t get_favicon(httpd_req_t *req)
{
  extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
  extern const unsigned char favicon_ico_end[] asm("_binary_favicon_ico_end");
  const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
  httpd_resp_set_type(req, "image/x-icon");
  httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);

  return ESP_OK;
}

static esp_err_t download_logs_file(httpd_req_t *req)
{
  FILE *logs_file = fopen("/spiffs/logs.txt", "r");

  if (!logs_file)
  {
    ESP_LOGE(TAG, "Failed to open file");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "File opened successfully");

  httpd_resp_set_type(req, "text/html");

  fseek(logs_file, 0, SEEK_END);
  long file_size = ftell(logs_file);
  fseek(logs_file, 0, SEEK_SET);
  if (file_size == 0)
  {
    ESP_LOGI(TAG, "Logs file is empty");
  }
  else
  {
    ESP_LOGI(TAG, "Logs file size: %ld bytes", file_size);
  }

  ESP_LOGI(TAG, "user_ctx address: %p", req->user_ctx);
  ESP_LOGI(TAG, "req address: %p", req);

  char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
  size_t chunksize;
  do
  {
    /* Read file in chunks into the scratch buffer */
    chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, logs_file);

    ESP_LOGI(TAG, "Read file part of size: %d", chunksize);

    if (chunksize > 0)
    {
      /* Send the buffer contents as HTTP response chunk */
      if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK)
      {
        ESP_LOGE(TAG, "File sending failed!");
        /* Abort sending file */
        httpd_resp_sendstr_chunk(req, NULL);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
        return ESP_FAIL;
      }
    }
    /* Keep looping till the whole file is sent */
  } while (chunksize != 0);

  fclose(logs_file);

  ESP_LOGI(TAG, "File sending complete");
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
  if (server_data)
  {
    ESP_LOGE(TAG, "File server already started");
    return ESP_ERR_INVALID_STATE;
  }

  /* Allocate memory for server data */
  server_data = calloc(1, sizeof(struct file_server_data));
  if (!server_data)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for server data");
    return ESP_ERR_NO_MEM;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t server = NULL;

  httpd_uri_t uri_get_index = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = get_index_page,
      .user_ctx = server_data,
  };

  httpd_uri_t uri_download_logs = {
      .uri = "/logs",
      .method = HTTP_GET,
      .handler = download_logs_file,
      .user_ctx = server_data,
  };

  httpd_uri_t uri_get_favicon = {
      .uri = "/favicon.ico",
      .method = HTTP_GET,
      .handler = get_favicon,
      .user_ctx = server_data,
  };

  if (httpd_start(&server, &config) == ESP_OK)
  {
    httpd_register_uri_handler(server, &uri_get_index);
    httpd_register_uri_handler(server, &uri_download_logs);
    httpd_register_uri_handler(server, &uri_get_favicon);
  }

  return server;
}

void webserver_task(void *pvParameter)
{
  ESP_LOGI(TAG, "Waiting for Wi-Fi");
  xEventGroupWaitBits(global_event_group, IS_WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

  ESP_LOGI(TAG, "Init start");
  start_webserver();
  ESP_LOGI(TAG, "Init done");

  while (1)
  {
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}