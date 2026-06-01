#ifndef CO2STATION_APP_MQTT_H
#define CO2STATION_APP_MQTT_H

#include <stdbool.h>
#include <stdint.h>

#include "app_settings.h"

void app_mqtt_publish_measurement(const app_settings_t *settings, uint16_t co2_ppm, int16_t temperature_deci_c,
                                  uint16_t humidity_deci_rh, bool has_rssi_dbm, int8_t rssi_dbm);

#endif