#include "app_webserver.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "app_settings.h"
#include "str.h"
#include "tzones.h"

typedef esp_err_t (*web_chunk_handler_t)(httpd_req_t *req, const app_settings_t *settings);

typedef struct
{
    const char          *const_html;
    web_chunk_handler_t  handler;
} web_template_chunk_t;

static const char  *TAG           = "CO2Station WEB";
static httpd_handle_t s_http_server = NULL;

static int hex_to_int(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

static void url_decode_inplace(char *text)
{
    char *read_ptr  = text;
    char *write_ptr = text;

    while (*read_ptr != '\0') {
        if (*read_ptr == '+' ) {
            *write_ptr++ = ' ';
            ++read_ptr;
        } else if (*read_ptr == '%' && read_ptr[1] != '\0' && read_ptr[2] != '\0') {
            int high = hex_to_int(read_ptr[1]);
            int low  = hex_to_int(read_ptr[2]);

            if (high >= 0 && low >= 0) {
                *write_ptr++ = (char)((high << 4) | low);
                read_ptr += 3;
            } else {
                /* Malformed %xy: pass through the three bytes verbatim. */
                *write_ptr++ = *read_ptr++;
                *write_ptr++ = *read_ptr++;
                *write_ptr++ = *read_ptr++;
            }
        } else {
            *write_ptr++ = *read_ptr++;
        }
    }

    *write_ptr = '\0';
}

static bool parse_float(const char *text, float *value)
{
    char *end_ptr;
    float parsed;

    if (text == NULL || *text == '\0') {
        return false;
    }

    parsed = strtof(text, &end_ptr);
    if (end_ptr == text || *end_ptr != '\0') {
        return false;
    }

    *value = parsed;
    return true;
}

static void copy_text_field(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t i = 0;
    for (; i < dst_len - 1 && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void apply_timezone_setting(const char *timezone)
{
    const char *timezone_name = (timezone != NULL && timezone[0] != '\0') ? timezone : "Etc/UTC";

    if (setupTZ(timezone_name)) {
        ESP_LOGI(TAG, "Timezone set to %s", timezone_name);
        return;
    }

    ESP_LOGW(TAG, "Unknown timezone '%s', falling back to Etc/UTC", timezone_name);
    (void)setupTZ("Etc/UTC");
}

static bool settings_equal(const app_settings_t *lhs, const app_settings_t *rhs)
{
    return strcmp(lhs->mqtt_address, rhs->mqtt_address) == 0 &&
           strcmp(lhs->mqtt_username, rhs->mqtt_username) == 0 &&
           strcmp(lhs->mqtt_password, rhs->mqtt_password) == 0 && strcmp(lhs->device_name, rhs->device_name) == 0 &&
           strcmp(lhs->host_name, rhs->host_name) == 0 && strcmp(lhs->timezone, rhs->timezone) == 0 &&
           lhs->co2_offset == rhs->co2_offset && lhs->temperature_offset == rhs->temperature_offset &&
           lhs->humidity_offset == rhs->humidity_offset;
}

static void update_settings_from_form(app_settings_t *settings, char *body, bool *cancel)
{
    char *save_ptr;

    for (char *token = strtok_r(body, "&", &save_ptr); token != NULL; token = strtok_r(NULL, "&", &save_ptr)) {
        char *equals = strchr(token, '=');
        char *key;
        char *value;
        float  parsed_value;

        if (equals == NULL) {
            continue;
        }

        *equals = '\0';
        key     = token;
        value   = equals + 1;
        url_decode_inplace(key);
        url_decode_inplace(value);

        if (strcmp(key, "cancel") == 0 && strcmp(value, "cancel") == 0) {
            if (cancel != NULL) {
                *cancel = true;
            }
        } else if (strcmp(key, "mqtt_address") == 0) {
            copy_text_field(settings->mqtt_address, sizeof(settings->mqtt_address), value);
        } else if (strcmp(key, "mqtt_username") == 0) {
            copy_text_field(settings->mqtt_username, sizeof(settings->mqtt_username), value);
        } else if (strcmp(key, "mqtt_password") == 0) {
            copy_text_field(settings->mqtt_password, sizeof(settings->mqtt_password), value);
        } else if (strcmp(key, "device_name") == 0) {
            copy_text_field(settings->device_name, sizeof(settings->device_name), value);
        } else if (strcmp(key, "host_name") == 0) {
            copy_text_field(settings->host_name, sizeof(settings->host_name), value);
        } else if (strcmp(key, "timezone") == 0) {
            copy_text_field(settings->timezone, sizeof(settings->timezone), value);
        } else if (strcmp(key, "co2_offset") == 0 && parse_float(value, &parsed_value)) {
            settings->co2_offset = parsed_value;
        } else if (strcmp(key, "temperature_offset") == 0 && parse_float(value, &parsed_value)) {
            settings->temperature_offset = parsed_value;
        } else if (strcmp(key, "humidity_offset") == 0 && parse_float(value, &parsed_value)) {
            settings->humidity_offset = parsed_value;
        }
    }
}

static esp_err_t send_escaped_chunk(httpd_req_t *req, const char *text)
{
    const char *cursor = text;
    const char *flush  = text;
    esp_err_t   ret;

    if (text == NULL) {
        return ESP_OK;
    }

    while (*cursor != '\0') {
        const char *replacement     = NULL;
        size_t      replacement_len = 0;

        switch (*cursor) {
        case '&':
            replacement     = "&amp;";
            replacement_len = 5;
            break;
        case '<':
            replacement     = "&lt;";
            replacement_len = 4;
            break;
        case '>':
            replacement     = "&gt;";
            replacement_len = 4;
            break;
        case '"':
            replacement     = "&quot;";
            replacement_len = 6;
            break;
        case '\'':
            replacement     = "&#39;";
            replacement_len = 5;
            break;
        default:
            break;
        }

        if (replacement != NULL) {
            if (cursor > flush) {
                ret = httpd_resp_send_chunk(req, flush, (ssize_t)(cursor - flush));
                if (ret != ESP_OK) {
                    return ret;
                }
            }
            ret = httpd_resp_send_chunk(req, replacement, (ssize_t)replacement_len);
            if (ret != ESP_OK) {
                return ret;
            }
            flush = cursor + 1;
        }
        ++cursor;
    }

    if (cursor > flush) {
        return httpd_resp_send_chunk(req, flush, (ssize_t)(cursor - flush));
    }
    return ESP_OK;
}

static esp_err_t send_float_chunk(httpd_req_t *req, float value)
{
    char buffer[32];
    int  length = snprintf(buffer, sizeof(buffer), "%.3f", value);

    if (length <= 0) {
        return ESP_OK;
    }
    if ((size_t)length >= sizeof(buffer)) {
        length = (int)sizeof(buffer) - 1;
    }
    return httpd_resp_send_chunk(req, buffer, length);
}

static esp_err_t http_data_mqtt_address(httpd_req_t *req, const app_settings_t *settings)
{
    return send_escaped_chunk(req, settings->mqtt_address);
}

static esp_err_t http_data_mqtt_username(httpd_req_t *req, const app_settings_t *settings)
{
    return send_escaped_chunk(req, settings->mqtt_username);
}

static esp_err_t http_data_mqtt_password(httpd_req_t *req, const app_settings_t *settings)
{
    return send_escaped_chunk(req, settings->mqtt_password);
}

static esp_err_t http_data_device_name(httpd_req_t *req, const app_settings_t *settings)
{
    return send_escaped_chunk(req, settings->device_name);
}

static esp_err_t http_data_host_name(httpd_req_t *req, const app_settings_t *settings)
{
    return send_escaped_chunk(req, settings->host_name);
}

static esp_err_t http_data_timezone_options(httpd_req_t *req, const app_settings_t *settings)
{
    char        option[160];
    const char *selected_timezone = (settings != NULL && settings->timezone[0] != '\0') ? settings->timezone : "Etc/UTC";

    for (size_t idx = 0; idx < TimeZonesNumer; ++idx) {
        int length = snprintf(option, sizeof(option), "<option value=\"%s\"%s>%s</option>", TimeZones[idx].name,
                              strcmp(selected_timezone, TimeZones[idx].name) == 0 ? " selected" : "",
                              TimeZones[idx].name);

        if (length <= 0) {
            continue;
        }
        if ((size_t)length >= sizeof(option)) {
            ESP_LOGW(TAG, "Timezone option truncated for %s", TimeZones[idx].name);
            continue;
        }

        esp_err_t ret = httpd_resp_send_chunk(req, option, length);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t http_data_co2_offset(httpd_req_t *req, const app_settings_t *settings)
{
    return send_float_chunk(req, settings->co2_offset);
}

static esp_err_t http_data_temperature_offset(httpd_req_t *req, const app_settings_t *settings)
{
    return send_float_chunk(req, settings->temperature_offset);
}

static esp_err_t http_data_humidity_offset(httpd_req_t *req, const app_settings_t *settings)
{
    return send_float_chunk(req, settings->humidity_offset);
}

#include "app_webserver_template.h"

static esp_err_t send_status_page(httpd_req_t *req, const char *title, const char *message)
{
    char page[512];
    int  length = snprintf(page, sizeof(page),
                           "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title></head>"
                           "<body><p>%s</p><p><a href=\"/\">Back to settings</a></p></body></html>",
                           title != NULL ? title : "", message != NULL ? message : "");

    if (length <= 0) {
        return ESP_FAIL;
    }
    if ((size_t)length >= sizeof(page)) {
        length = (int)sizeof(page) - 1;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, length);
}

static esp_err_t redirect_to(httpd_req_t *req, const char *path)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", path);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    app_settings_t settings;
    esp_err_t      ret;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    app_settings_get_snapshot(&settings);

    for (size_t idx = 0; idx < sizeof(s_index_template) / sizeof(s_index_template[0]); ++idx) {
        ret = httpd_resp_send_chunk(req, s_index_template[idx].const_html, HTTPD_RESP_USE_STRLEN);
        if (ret != ESP_OK) {
            (void)httpd_resp_send_chunk(req, NULL, 0);
            return ret;
        }

        if (s_index_template[idx].handler != NULL) {
            ret = s_index_template[idx].handler(req, &settings);
            if (ret != ESP_OK) {
                (void)httpd_resp_send_chunk(req, NULL, 0);
                return ret;
            }
        }
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t settings_save_handler(httpd_req_t *req)
{
    app_settings_t current_settings;
    app_settings_t updated_settings;
    str_t         *body;
    char          *body_data;
    bool           cancel = false;
    int            received_total = 0;
    esp_err_t      ret;

    if (req->content_len <= 0 || req->content_len > 2048) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
    }

    body = str_new_ln((size_t)req->content_len);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    body_data = str_c(body);

    while (received_total < req->content_len) {
        int received = httpd_req_recv(req, body_data + received_total, req->content_len - received_total);

        if (received <= 0) {
            str_destroy(&body);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request body");
        }
        received_total += received;
    }

    app_settings_get_snapshot(&current_settings);
    updated_settings = current_settings;
    update_settings_from_form(&updated_settings, body_data, &cancel);
    str_destroy(&body);

    if (cancel) {
        return redirect_to(req, "/nochanges");
    }

    /* Compare the same way the store will see it: app_settings_update()
     * normalizes its input, so we must normalize before checking equality to
     * avoid spurious "changed" detections when e.g. a user blanks a field
     * that has a forced default. */
    app_settings_normalize(&updated_settings);
    if (settings_equal(&current_settings, &updated_settings)) {
        return redirect_to(req, "/nochanges");
    }

    ret = app_settings_update(&updated_settings);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save settings: %s", esp_err_to_name(ret));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
    }

    apply_timezone_setting(updated_settings.timezone);

    return redirect_to(req, "/thankyou");
}

static esp_err_t settings_thankyou_handler(httpd_req_t *req)
{
    return send_status_page(req, "Configured", "Settings saved successfully.");
}

static esp_err_t settings_nochanges_handler(httpd_req_t *req)
{
    return send_status_page(req, "No changes", "Configuration was not changed.");
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_type(req, "image/x-icon");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_start(void)
{
    static const httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = settings_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t root_post_uri = {
        .uri      = "/",
        .method   = HTTP_POST,
        .handler  = settings_save_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t thankyou_uri = {
        .uri      = "/thankyou",
        .method   = HTTP_GET,
        .handler  = settings_thankyou_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t nochanges_uri = {
        .uri      = "/nochanges",
        .method   = HTTP_GET,
        .handler  = settings_nochanges_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t favicon_uri = {
        .uri      = "/favicon.ico",
        .method   = HTTP_GET,
        .handler  = favicon_handler,
        .user_ctx = NULL,
    };
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t      ret;

    if (s_http_server != NULL) {
        return ESP_OK;
    }

    ret = app_settings_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = httpd_start(&s_http_server, &config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = httpd_register_uri_handler(s_http_server, &root_uri);
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_http_server, &thankyou_uri);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_http_server, &nochanges_uri);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_http_server, &favicon_uri);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_http_server, &root_post_uri);
    }
    if (ret != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Webserver started on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t web_stop(void)
{
    if (s_http_server == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_http_server);
    if (ret == ESP_OK) {
        s_http_server = NULL;
        ESP_LOGI(TAG, "Webserver stopped");
    }
    return ret;
}

bool web_is_running(void)
{
    return s_http_server != NULL;
}