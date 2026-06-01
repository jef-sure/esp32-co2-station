#ifndef CO2STATION_APP_WEBSERVER_H
#define CO2STATION_APP_WEBSERVER_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t web_start(void);
esp_err_t web_stop(void);
bool      web_is_running(void);

#endif