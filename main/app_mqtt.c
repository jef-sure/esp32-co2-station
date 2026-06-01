#include "app_mqtt.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

enum
{
    APP_MQTT_URI_LEN                = APP_SETTINGS_MQTT_ADDRESS_LEN + 8,
    APP_MQTT_TOPIC_LEN              = APP_SETTINGS_HOST_NAME_LEN + 32,
    APP_MQTT_AVAILABILITY_TOPIC_LEN = APP_SETTINGS_HOST_NAME_LEN + 24,
    APP_MQTT_DISCOVERY_TOPIC_LEN    = APP_SETTINGS_HOST_NAME_LEN + 64,
    APP_MQTT_DISCOVERY_PAYLOAD_LEN  = 768,
    APP_MQTT_PAYLOAD_LEN            = 128,
    APP_MQTT_RETRY_DELAY_US         = 10000000,
};

typedef struct
{
    const char *object_id;
    const char *name;
    const char *device_class;
    const char *unit;
    const char *value_key;
} app_mqtt_sensor_desc_t;

static const app_mqtt_sensor_desc_t s_discovery_sensors[] = {
    {
     .object_id    = "co2",
     .name         = "CO2",
     .device_class = "carbon_dioxide",
     .unit         = "ppm",
     .value_key    = "co2_ppm",
     },
    {
     .object_id    = "temperature",
     .name         = "Temperature",
     .device_class = "temperature",
     .unit         = "°C",
     .value_key    = "temperature_c",
     },
    {
     .object_id    = "humidity",
     .name         = "Humidity",
     .device_class = "humidity",
     .unit         = "%",
     .value_key    = "humidity_rh",
     },
    {
     .object_id    = "wifi_rssi",
     .name         = "Wi-Fi RSSI",
     .device_class = "signal_strength",
     .unit         = "dBm",
     .value_key    = "rssi_dbm",
     },
};

static const char              *TAG = "CO2Station MQTT";
static esp_mqtt_client_handle_t s_client;
static bool                     s_connected;
static char                     s_broker_uri[APP_MQTT_URI_LEN];
static char                     s_measurement_topic[APP_MQTT_TOPIC_LEN];
static char                     s_availability_topic[APP_MQTT_AVAILABILITY_TOPIC_LEN];
static char                     s_node_id[APP_SETTINGS_HOST_NAME_LEN];
static char                     s_device_name[APP_SETTINGS_DEVICE_NAME_LEN];
static char                     s_mqtt_username[APP_SETTINGS_MQTT_USERNAME_LEN];
static char                     s_mqtt_password[APP_SETTINGS_MQTT_PASSWORD_LEN];
static int64_t                  s_next_start_retry_us;

static void app_mqtt_copy_text(const char *src, char *dst, size_t dst_len)
{
    if (dst_len == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_len, "%s", src);
}

static void app_mqtt_copy_trimmed(const char *src, char *dst, size_t dst_len)
{
    if (dst_len == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    while (*src != '\0' && isspace((unsigned char)*src)) {
        ++src;
    }

    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) {
        --len;
    }

    if (len >= dst_len) {
        len = dst_len - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void app_mqtt_format_uri(const char *address, char *uri, size_t uri_len)
{
    char trimmed[APP_SETTINGS_MQTT_ADDRESS_LEN];

    app_mqtt_copy_trimmed(address, trimmed, sizeof(trimmed));
    if (trimmed[0] == '\0') {
        uri[0] = '\0';
        return;
    }

    if (strstr(trimmed, "://") != NULL) {
        snprintf(uri, uri_len, "%s", trimmed);
        return;
    }

    snprintf(uri, uri_len, "mqtt://%s", trimmed);
}

static void app_mqtt_format_topic(const app_settings_t *settings, char *topic, size_t topic_len)
{
    const char *host_name = (settings != NULL && settings->host_name[0] != '\0') ? settings->host_name : "co2station";
    snprintf(topic, topic_len, "devices/%s/measurements", host_name);
}

static void app_mqtt_format_availability_topic(const app_settings_t *settings, char *topic, size_t topic_len)
{
    const char *host_name = (settings != NULL && settings->host_name[0] != '\0') ? settings->host_name : "co2station";
    snprintf(topic, topic_len, "devices/%s/status", host_name);
}

static int app_mqtt_publish_common(const char *topic, const char *payload, int len, int qos, int retain, bool store)
{
    if (s_client == NULL || topic == NULL || topic[0] == '\0' || payload == NULL) {
        return -1;
    }

    return esp_mqtt_client_enqueue(s_client, topic, payload, len, qos, retain, store);
}

static void app_mqtt_publish_availability(const char *state)
{
    if (state == NULL || s_availability_topic[0] == '\0') {
        return;
    }

    int msg_id = app_mqtt_publish_common(s_availability_topic, state, 0, 1, 1, true);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to publish availability to %s", s_availability_topic);
    }
}

static void app_mqtt_publish_autodiscovery(void)
{
    char config_topic[APP_MQTT_DISCOVERY_TOPIC_LEN];
    char payload[APP_MQTT_DISCOVERY_PAYLOAD_LEN];

    for (size_t i = 0; i < sizeof(s_discovery_sensors) / sizeof(s_discovery_sensors[0]); ++i) {
        const app_mqtt_sensor_desc_t *sensor = &s_discovery_sensors[i];

        int topic_len = snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/%s/config", s_node_id,
                                 sensor->object_id);
        if (topic_len <= 0 || topic_len >= (int)sizeof(config_topic)) {
            ESP_LOGW(TAG, "Discovery topic truncated for %s", sensor->object_id);
            continue;
        }

        int payload_len = snprintf(
            payload, sizeof(payload),
            "{\"name\":\"%s\",\"object_id\":\"%s_%s\",\"unique_id\":\"%s_%s\","
            "\"state_topic\":\"%s\",\"availability_topic\":\"%s\","
            "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
            "\"device_class\":\"%s\",\"state_class\":\"measurement\","
            "\"unit_of_measurement\":\"%s\",\"value_template\":\"{{ value_json.%s }}\","
            "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"manufacturer\":\"Anton Petrusevich\","
            "\"model\":\"ESP32-C3 SCD41 GC9A01 v1.0\"}}",
            sensor->name, s_node_id, sensor->object_id, s_node_id, sensor->object_id, s_measurement_topic,
            s_availability_topic, sensor->device_class, sensor->unit, sensor->value_key, s_node_id, s_device_name);
        if (payload_len <= 0 || payload_len >= (int)sizeof(payload)) {
            ESP_LOGW(TAG, "Discovery payload truncated for %s", sensor->object_id);
            continue;
        }

        int msg_id = app_mqtt_publish_common(config_topic, payload, payload_len, 1, 1, true);
        if (msg_id < 0) {
            ESP_LOGW(TAG, "Failed to publish discovery config for %s", sensor->object_id);
        }
    }
}

static void app_mqtt_dispose_client(void)
{
    if (s_client != NULL) {
        (void)esp_mqtt_client_stop(s_client);
        (void)esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
}

static void app_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Connected to %s", s_broker_uri);
        app_mqtt_publish_autodiscovery();
        app_mqtt_publish_availability("online");
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Disconnected from %s", s_broker_uri);
        break;
    case MQTT_EVENT_ERROR:
        s_connected = false;
        if (event != NULL && event->error_handle != NULL) {
            ESP_LOGW(TAG, "MQTT error: type=%d transport_err=%s sock_errno=%d", (int)event->error_handle->error_type,
                     esp_err_to_name(event->error_handle->esp_tls_last_esp_err),
                     event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGW(TAG, "MQTT error");
        }
        break;
    default:
        break;
    }
}

static esp_err_t app_mqtt_start_client(const char *broker_uri)
{
    esp_mqtt_client_config_t config = {
        .broker.address.uri                  = broker_uri,
        .credentials.username                = s_mqtt_username[0] != '\0' ? s_mqtt_username : NULL,
        .credentials.client_id               = s_node_id[0] != '\0' ? s_node_id : NULL,
        .credentials.authentication.password = s_mqtt_password[0] != '\0' ? s_mqtt_password : NULL,
        .session.last_will.topic             = s_availability_topic[0] != '\0' ? s_availability_topic : NULL,
        .session.last_will.msg               = "offline",
        .session.last_will.msg_len           = 7,
        .session.last_will.qos               = 1,
        .session.last_will.retain            = 1,
    };
    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, app_mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        app_mqtt_dispose_client();
        return err;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        app_mqtt_dispose_client();
        return err;
    }

    ESP_LOGI(TAG, "Starting client for %s", broker_uri);
    return ESP_OK;
}

static bool app_mqtt_sync_client(const app_settings_t *settings)
{
    char    broker_uri[APP_MQTT_URI_LEN];
    char    measurement_topic[APP_MQTT_TOPIC_LEN];
    char    availability_topic[APP_MQTT_AVAILABILITY_TOPIC_LEN];
    char    node_id[APP_SETTINGS_HOST_NAME_LEN];
    char    device_name[APP_SETTINGS_DEVICE_NAME_LEN];
    char    mqtt_username[APP_SETTINGS_MQTT_USERNAME_LEN];
    char    mqtt_password[APP_SETTINGS_MQTT_PASSWORD_LEN];
    int64_t now = esp_timer_get_time();

    if (settings == NULL) {
        return false;
    }

    app_mqtt_format_uri(settings->mqtt_address, broker_uri, sizeof(broker_uri));
    app_mqtt_format_topic(settings, measurement_topic, sizeof(measurement_topic));
    app_mqtt_format_availability_topic(settings, availability_topic, sizeof(availability_topic));
    app_mqtt_copy_text(settings->host_name[0] != '\0' ? settings->host_name : "co2station", node_id, sizeof(node_id));
    app_mqtt_copy_text(settings->device_name[0] != '\0' ? settings->device_name : "CO2Station", device_name,
                       sizeof(device_name));
    app_mqtt_copy_text(settings->mqtt_username, mqtt_username, sizeof(mqtt_username));
    app_mqtt_copy_text(settings->mqtt_password, mqtt_password, sizeof(mqtt_password));

    if (broker_uri[0] == '\0') {
        if (s_client != NULL || s_broker_uri[0] != '\0') {
            ESP_LOGI(TAG, "MQTT disabled");
        }
        app_mqtt_dispose_client();
        s_broker_uri[0]         = '\0';
        s_measurement_topic[0]  = '\0';
        s_availability_topic[0] = '\0';
        s_node_id[0]            = '\0';
        s_device_name[0]        = '\0';
        s_mqtt_username[0]      = '\0';
        s_mqtt_password[0]      = '\0';
        s_next_start_retry_us   = 0;
        return false;
    }

    if (strcmp(s_broker_uri, broker_uri) != 0 || strcmp(s_measurement_topic, measurement_topic) != 0 ||
        strcmp(s_availability_topic, availability_topic) != 0 || strcmp(s_node_id, node_id) != 0 ||
        strcmp(s_device_name, device_name) != 0 || strcmp(s_mqtt_username, mqtt_username) != 0 ||
        strcmp(s_mqtt_password, mqtt_password) != 0) {
        app_mqtt_dispose_client();
        snprintf(s_broker_uri, sizeof(s_broker_uri), "%s", broker_uri);
        snprintf(s_measurement_topic, sizeof(s_measurement_topic), "%s", measurement_topic);
        snprintf(s_availability_topic, sizeof(s_availability_topic), "%s", availability_topic);
        snprintf(s_node_id, sizeof(s_node_id), "%s", node_id);
        snprintf(s_device_name, sizeof(s_device_name), "%s", device_name);
        snprintf(s_mqtt_username, sizeof(s_mqtt_username), "%s", mqtt_username);
        snprintf(s_mqtt_password, sizeof(s_mqtt_password), "%s", mqtt_password);
        s_next_start_retry_us = 0;
    }

    if (s_client != NULL) {
        return true;
    }

    if (now < s_next_start_retry_us) {
        return false;
    }

    esp_err_t err = app_mqtt_start_client(s_broker_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client for %s: %s", s_broker_uri, esp_err_to_name(err));
        app_mqtt_dispose_client();
        s_next_start_retry_us = now + APP_MQTT_RETRY_DELAY_US;
        return false;
    }

    return true;
}

void app_mqtt_publish_measurement(const app_settings_t *settings, uint16_t co2_ppm, int16_t temperature_deci_c,
                                  uint16_t humidity_deci_rh, bool has_rssi_dbm, int8_t rssi_dbm)
{
    char payload[APP_MQTT_PAYLOAD_LEN];
    int  abs_temp = temperature_deci_c < 0 ? -(int)temperature_deci_c : (int)temperature_deci_c;

    if (!app_mqtt_sync_client(settings) || !s_connected || s_measurement_topic[0] == '\0') {
        return;
    }

    int payload_len;
    if (has_rssi_dbm) {
        payload_len = snprintf(payload, sizeof(payload),
                               "{\"co2_ppm\":%u,\"temperature_c\":%s%d.%d,\"humidity_rh\":%u.%u,\"rssi_dbm\":%d}",
                               (unsigned)co2_ppm, temperature_deci_c < 0 ? "-" : "", abs_temp / 10, abs_temp % 10,
                               humidity_deci_rh / 10, humidity_deci_rh % 10, (int)rssi_dbm);
    } else {
        payload_len =
            snprintf(payload, sizeof(payload), "{\"co2_ppm\":%u,\"temperature_c\":%s%d.%d,\"humidity_rh\":%u.%u}",
                     (unsigned)co2_ppm, temperature_deci_c < 0 ? "-" : "", abs_temp / 10, abs_temp % 10,
                     humidity_deci_rh / 10, humidity_deci_rh % 10);
    }
    if (payload_len <= 0 || payload_len >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "Measurement payload truncated, skipping publish");
        return;
    }

    int msg_id = app_mqtt_publish_common(s_measurement_topic, payload, payload_len, 0, 0, true);
    if (msg_id == -2) {
        ESP_LOGW(TAG, "MQTT outbox full, dropping measurement publish to %s", s_measurement_topic);
    } else if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to enqueue measurement publish to %s", s_measurement_topic);
    }
}