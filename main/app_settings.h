#ifndef CO2STATION_APP_SETTINGS_H
#define CO2STATION_APP_SETTINGS_H

#include "esp_err.h"

enum
{
    APP_SETTINGS_MQTT_ADDRESS_LEN  = 128,
    APP_SETTINGS_MQTT_USERNAME_LEN = 64,
    APP_SETTINGS_MQTT_PASSWORD_LEN = 64,
    APP_SETTINGS_DEVICE_NAME_LEN   = 64,
    APP_SETTINGS_HOST_NAME_LEN     = 64,
    APP_SETTINGS_TIMEZONE_LEN      = 64,
};

typedef struct
{
    char  mqtt_address[APP_SETTINGS_MQTT_ADDRESS_LEN];
    char  mqtt_username[APP_SETTINGS_MQTT_USERNAME_LEN];
    char  mqtt_password[APP_SETTINGS_MQTT_PASSWORD_LEN];
    char  device_name[APP_SETTINGS_DEVICE_NAME_LEN];
    char  host_name[APP_SETTINGS_HOST_NAME_LEN];
    char  timezone[APP_SETTINGS_TIMEZONE_LEN];
    float co2_offset;
    float temperature_offset;
    float humidity_offset;
} app_settings_t;

esp_err_t app_settings_init(void);
void      app_settings_get_snapshot(app_settings_t *settings);
void      app_settings_normalize(app_settings_t *settings);
esp_err_t app_settings_update(const app_settings_t *settings);

#endif