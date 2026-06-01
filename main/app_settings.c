#include "app_settings.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

enum
{
    APP_SETTINGS_SCHEMA_VERSION    = 4,
    APP_SETTINGS_SCHEMA_VERSION_V3 = 3,
    APP_SETTINGS_SCHEMA_VERSION_V2 = 2,
    APP_SETTINGS_SCHEMA_VERSION_V1 = 1,
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
    float co2_coefficient;
    float temperature_offset;
    float temperature_coefficient;
    float humidity_offset;
    float humidity_coefficient;
} app_settings_v3_t;

typedef struct
{
    char  mqtt_address[APP_SETTINGS_MQTT_ADDRESS_LEN];
    char  mqtt_username[APP_SETTINGS_MQTT_USERNAME_LEN];
    char  mqtt_password[APP_SETTINGS_MQTT_PASSWORD_LEN];
    char  device_name[APP_SETTINGS_DEVICE_NAME_LEN];
    char  host_name[APP_SETTINGS_HOST_NAME_LEN];
    float co2_offset;
    float co2_coefficient;
    float temperature_offset;
    float temperature_coefficient;
    float humidity_offset;
    float humidity_coefficient;
} app_settings_v2_t;

typedef struct
{
    char  mqtt_address[APP_SETTINGS_MQTT_ADDRESS_LEN];
    char  device_name[APP_SETTINGS_DEVICE_NAME_LEN];
    char  host_name[APP_SETTINGS_HOST_NAME_LEN];
    float co2_offset;
    float co2_coefficient;
    float temperature_offset;
    float temperature_coefficient;
    float humidity_offset;
    float humidity_coefficient;
} app_settings_v1_t;

typedef struct
{
    uint32_t       schema_version;
    app_settings_t settings;
} app_settings_storage_t;

typedef struct
{
    uint32_t          schema_version;
    app_settings_v3_t settings;
} app_settings_storage_v3_t;

typedef struct
{
    uint32_t          schema_version;
    app_settings_v2_t settings;
} app_settings_storage_v2_t;

typedef struct
{
    uint32_t         schema_version;
    app_settings_v1_t settings;
} app_settings_storage_v1_t;

typedef union
{
    app_settings_storage_t    v2;
    app_settings_storage_v3_t v3_legacy;
    app_settings_storage_v2_t v2_legacy;
    app_settings_storage_v1_t v1;
} app_settings_storage_any_t;

static const char      *APP_SETTINGS_NAMESPACE = "appcfg";
static const char      *APP_SETTINGS_KEY       = "settings";
static SemaphoreHandle_t s_settings_mutex      = NULL;
static app_settings_t    s_settings            = {0};
static bool              s_settings_initialized = false;

static void app_settings_set_defaults(app_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    memcpy(settings->device_name, "CO2Station", sizeof("CO2Station"));
    memcpy(settings->host_name, "co2station", sizeof("co2station"));
    memcpy(settings->timezone, "Etc/UTC", sizeof("Etc/UTC"));
}

static void app_settings_import_v3(app_settings_t *dst, const app_settings_v3_t *src)
{
    app_settings_set_defaults(dst);

    memcpy(dst->mqtt_address, src->mqtt_address, sizeof(src->mqtt_address));
    memcpy(dst->mqtt_username, src->mqtt_username, sizeof(src->mqtt_username));
    memcpy(dst->mqtt_password, src->mqtt_password, sizeof(src->mqtt_password));
    memcpy(dst->device_name, src->device_name, sizeof(src->device_name));
    memcpy(dst->host_name, src->host_name, sizeof(src->host_name));
    memcpy(dst->timezone, src->timezone, sizeof(src->timezone));
    dst->co2_offset         = src->co2_offset;
    dst->temperature_offset = src->temperature_offset;
    dst->humidity_offset    = src->humidity_offset;
}

static void app_settings_import_v2(app_settings_t *dst, const app_settings_v2_t *src)
{
    app_settings_set_defaults(dst);

    memcpy(dst->mqtt_address, src->mqtt_address, sizeof(src->mqtt_address));
    memcpy(dst->mqtt_username, src->mqtt_username, sizeof(src->mqtt_username));
    memcpy(dst->mqtt_password, src->mqtt_password, sizeof(src->mqtt_password));
    memcpy(dst->device_name, src->device_name, sizeof(src->device_name));
    memcpy(dst->host_name, src->host_name, sizeof(src->host_name));
    dst->co2_offset         = src->co2_offset;
    dst->temperature_offset = src->temperature_offset;
    dst->humidity_offset    = src->humidity_offset;
}

static void app_settings_import_v1(app_settings_t *dst, const app_settings_v1_t *src)
{
    app_settings_set_defaults(dst);

    memcpy(dst->mqtt_address, src->mqtt_address, sizeof(src->mqtt_address));
    memcpy(dst->device_name, src->device_name, sizeof(src->device_name));
    memcpy(dst->host_name, src->host_name, sizeof(src->host_name));
    dst->co2_offset         = src->co2_offset;
    dst->temperature_offset = src->temperature_offset;
    dst->humidity_offset    = src->humidity_offset;
}

void app_settings_normalize(app_settings_t *settings)
{
    settings->mqtt_address[APP_SETTINGS_MQTT_ADDRESS_LEN - 1]   = '\0';
    settings->mqtt_username[APP_SETTINGS_MQTT_USERNAME_LEN - 1] = '\0';
    settings->mqtt_password[APP_SETTINGS_MQTT_PASSWORD_LEN - 1] = '\0';
    settings->device_name[APP_SETTINGS_DEVICE_NAME_LEN - 1]     = '\0';
    settings->host_name[APP_SETTINGS_HOST_NAME_LEN - 1]         = '\0';
    settings->timezone[APP_SETTINGS_TIMEZONE_LEN - 1]           = '\0';

    if (settings->device_name[0] == '\0') {
        memcpy(settings->device_name, "CO2Station", sizeof("CO2Station"));
    }
    if (settings->host_name[0] == '\0') {
        memcpy(settings->host_name, "co2station", sizeof("co2station"));
    }
    if (settings->timezone[0] == '\0') {
        memcpy(settings->timezone, "Etc/UTC", sizeof("Etc/UTC"));
    }
}

static esp_err_t app_settings_save(const app_settings_t *settings)
{
    app_settings_storage_t storage = {
        .schema_version = APP_SETTINGS_SCHEMA_VERSION,
        .settings       = *settings,
    };
    nvs_handle_t handle;
    esp_err_t    ret = nvs_open(APP_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(handle, APP_SETTINGS_KEY, &storage, sizeof(storage));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t app_settings_init(void)
{
    app_settings_storage_any_t storage = {0};
    size_t                     size    = sizeof(storage);
    nvs_handle_t               handle;
    esp_err_t                  ret;

    if (s_settings_initialized) {
        return ESP_OK;
    }

    if (s_settings_mutex == NULL) {
        s_settings_mutex = xSemaphoreCreateMutex();
        if (s_settings_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    app_settings_set_defaults(&s_settings);

    ret = nvs_open(APP_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_blob(handle, APP_SETTINGS_KEY, &storage, &size);
    if (ret == ESP_OK && size == sizeof(storage.v2) && storage.v2.schema_version == APP_SETTINGS_SCHEMA_VERSION) {
        s_settings = storage.v2.settings;
        app_settings_normalize(&s_settings);
    } else if (ret == ESP_OK && size == sizeof(storage.v3_legacy) &&
               storage.v3_legacy.schema_version == APP_SETTINGS_SCHEMA_VERSION_V3) {
        app_settings_import_v3(&s_settings, &storage.v3_legacy.settings);
        app_settings_normalize(&s_settings);
        ret = app_settings_save(&s_settings);
        if (ret != ESP_OK) {
            nvs_close(handle);
            return ret;
        }
    } else if (ret == ESP_OK && size == sizeof(storage.v2_legacy) &&
               storage.v2_legacy.schema_version == APP_SETTINGS_SCHEMA_VERSION_V2) {
        app_settings_import_v2(&s_settings, &storage.v2_legacy.settings);
        app_settings_normalize(&s_settings);
        ret = app_settings_save(&s_settings);
        if (ret != ESP_OK) {
            nvs_close(handle);
            return ret;
        }
    } else if (ret == ESP_OK && size == sizeof(storage.v1) && storage.v1.schema_version == APP_SETTINGS_SCHEMA_VERSION_V1) {
        app_settings_import_v1(&s_settings, &storage.v1.settings);
        app_settings_normalize(&s_settings);
        ret = app_settings_save(&s_settings);
        if (ret != ESP_OK) {
            nvs_close(handle);
            return ret;
        }
    } else {
        app_settings_normalize(&s_settings);
        ret = app_settings_save(&s_settings);
        if (ret != ESP_OK) {
            nvs_close(handle);
            return ret;
        }
    }

    nvs_close(handle);
    s_settings_initialized = true;
    return ESP_OK;
}

void app_settings_get_snapshot(app_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    if (!s_settings_initialized || s_settings_mutex == NULL) {
        app_settings_set_defaults(settings);
        return;
    }

    if (xSemaphoreTake(s_settings_mutex, portMAX_DELAY) == pdTRUE) {
        *settings = s_settings;
        xSemaphoreGive(s_settings_mutex);
    }
}

esp_err_t app_settings_update(const app_settings_t *settings)
{
    app_settings_t normalized;
    esp_err_t      ret;

    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_settings_initialized || s_settings_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    normalized = *settings;
    app_settings_normalize(&normalized);

    if (xSemaphoreTake(s_settings_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ret = app_settings_save(&normalized);
    if (ret == ESP_OK) {
        s_settings = normalized;
    }

    xSemaphoreGive(s_settings_mutex);
    return ret;
}