#include "freertos/event_groups.h"

extern EventGroupHandle_t global_event_group;

#define IS_NTP_SET_BIT BIT0
#define WIFI_CONNECTED_BIT BIT0