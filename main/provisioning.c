#include "provisioning.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "qrcode.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#include "app_fonts.h"
#include "app_settings.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"

enum
{
    APP_WIFI_CONNECTED_EVENT   = BIT0,
    APP_WIFI_PROVISIONED_EVENT = BIT1,
};

#define APP_PROVISIONING_POP       "CO21234"
#define APP_PROVISIONING_PREFIX    "CO2-"
#define APP_PROVISIONING_TRANSPORT "ble"
#define APP_WIFI_STA_MAX_RETRY     10

typedef struct
{
    bool          active;
    bool          had_previous_config;
    bool          received_credentials;
    bool          provisioning_succeeded;
    bool          provisioning_failed;
    bool          credentials_changed;
    uint32_t      timeout_ms;
    int64_t       deadline_us;
    wifi_config_t previous_config;
} app_wifi_provisioning_session_t;

static const char                     *TAG                    = "CO2Station WIFI";
static EventGroupHandle_t              s_wifi_event_group     = NULL;
static dgx_screen_t                   *s_qr_screen            = NULL;
static esp_netif_t                    *s_wifi_netif           = NULL;
static bool                            s_wifi_initialized     = false;
static bool                            s_wifi_is_provisioned  = false;
static app_wifi_provisioning_session_t s_provisioning_session = {0};
static uint8_t                         s_sta_retry_count      = 0;

#if defined(DEBUG_APP_HEAP)
/* Called around BLE provisioning transitions when DEBUG_APP_HEAP is enabled to snapshot heap usage. */
static void log_provisioning_heap(const char *phase)
{
    ESP_LOGI(TAG, "Provisioning %s: free heap=%u bytes, min free heap=%u bytes", phase,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}
#else
#define log_provisioning_heap(...) ((void)0)
#endif

static bool app_wifi_has_sta_config(const wifi_config_t *config)
{
    for (size_t idx = 0; idx < sizeof(config->sta.ssid); ++idx) {
        if (config->sta.ssid[idx] != 0) {
            return true;
        }
    }

    return false;
}

static bool app_wifi_sta_config_changed(const wifi_sta_config_t *current, const wifi_sta_config_t *incoming)
{
    return memcmp(current->ssid, incoming->ssid, sizeof(current->ssid)) != 0 ||
           memcmp(current->password, incoming->password, sizeof(current->password)) != 0 ||
           current->bssid_set != incoming->bssid_set ||
           memcmp(current->bssid, incoming->bssid, sizeof(current->bssid)) != 0 ||
           current->channel != incoming->channel;
}

static void app_draw_centered_text(dgx_screen_t *screen, int baseline_y, const char *text, dgx_font_t *font,
                                   uint32_t color)
{
    int16_t raw_ycorner;
    int16_t raw_height;
    int     width = dgx_font_string_bounds(text, font, &raw_ycorner, &raw_height);
    int     x     = (screen->width - width) / 2;

    dgx_font_string_utf8_screen(screen, x, baseline_y, text, color, DgxOutputNormal, 1, font, NULL, NULL);
}

static void app_draw_qrcode(esp_qrcode_handle_t code)
{
    const int qrsize = esp_qrcode_get_size(code);
    const int top    = 42;
    const int bottom = 18;
    const int area_w = s_qr_screen->width - 20;
    const int area_h = s_qr_screen->height - top - bottom;
    const int area   = area_w < area_h ? area_w : area_h;
    int       scale  = area / qrsize;

    if (scale < 1) {
        scale = 1;
    }

    const int qr_pixels = qrsize * scale;
    const int x_border  = (s_qr_screen->width - qr_pixels) / 2;
    const int y_border  = top + ((s_qr_screen->height - top - bottom) - qr_pixels) / 2 - 2 * scale;

    dgx_fill_rectangle(s_qr_screen, 0, 0, s_qr_screen->width, s_qr_screen->height, DGX_WHITE(DGX_RGB_16));
    app_draw_centered_text(s_qr_screen, 16, "BLE Wi-Fi setup", app_font_provisioning_title(), 0);
    app_draw_centered_text(s_qr_screen, 31, "Espressif BLE app", app_font_provisioning_text(), 0);
    app_draw_centered_text(s_qr_screen, s_qr_screen->height - app_font_provisioning_text()->yAdvance - 2 * scale,
                           "Scan within 60 seconds", app_font_provisioning_text(), 0);

    for (int x = 0; x < qrsize; ++x) {
        for (int y = 0; y < qrsize; ++y) {
            if (!esp_qrcode_get_module(code, x, y)) {
                continue;
            }
            dgx_fill_rectangle(s_qr_screen, x_border + x * scale, y_border + y * scale, scale, scale, 0);
        }
    }
}

static esp_err_t app_wifi_stop_station(void)
{
    esp_err_t ret = esp_wifi_stop();

    if (ret == ESP_OK || ret == ESP_ERR_WIFI_NOT_INIT || ret == ESP_ERR_WIFI_NOT_STARTED) {
        return ESP_OK;
    }

    return ret;
}

static esp_err_t app_wifi_start_station(void)
{
    app_settings_t settings;
    esp_err_t      ret = esp_wifi_set_mode(WIFI_MODE_STA);

    if (ret != ESP_OK) {
        return ret;
    }

    if (s_wifi_netif != NULL) {
        app_settings_get_snapshot(&settings);
        ret = esp_netif_set_hostname(s_wifi_netif, settings.host_name);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        return ret;
    }

    return ESP_OK;
}

static esp_err_t app_wifi_restore_previous_config(void)
{
    esp_err_t ret;

    if (!s_provisioning_session.had_previous_config) {
        return ESP_OK;
    }

    ret = app_wifi_stop_station();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &s_provisioning_session.previous_config);
    if (ret != ESP_OK) {
        return ret;
    }

    return app_wifi_start_station();
}

static app_wifi_provisioning_state_t app_wifi_finish_provisioning(app_wifi_provisioning_state_t result,
                                                                  bool                          restore_previous_config)
{
    log_provisioning_heap("finish:entry");

    if (s_provisioning_session.active) {
        wifi_prov_mgr_deinit();
        log_provisioning_heap("finish:after manager deinit");
    }

    if (restore_previous_config && app_wifi_restore_previous_config() != ESP_OK) {
        result = AppWifiProvisioningFinishedError;
    }
    if (restore_previous_config) {
        log_provisioning_heap("finish:after station restore");
    }

    s_wifi_is_provisioned = s_provisioning_session.provisioning_succeeded ||
                            (restore_previous_config && result != AppWifiProvisioningFinishedError &&
                             s_provisioning_session.had_previous_config);
    s_qr_screen = NULL;
    memset(&s_provisioning_session, 0, sizeof(s_provisioning_session));
    log_provisioning_heap("finish:done");
    return result;
}

static void app_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            log_provisioning_heap("event:start");
            break;
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received Wi-Fi credentials for SSID '%s'", (const char *)wifi_sta_cfg->ssid);
            s_provisioning_session.received_credentials = true;
            s_provisioning_session.credentials_changed =
                !s_provisioning_session.had_previous_config ||
                app_wifi_sta_config_changed(&s_provisioning_session.previous_config.sta, wifi_sta_cfg);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning failed: %s",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "authentication failed" : "AP not found");
            /* Auto-stop is disabled; let the step loop tear the session down. */
            s_provisioning_session.provisioning_failed = true;
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            s_provisioning_session.provisioning_succeeded = true;
            xEventGroupSetBits(s_wifi_event_group, APP_WIFI_PROVISIONED_EVENT);
            log_provisioning_heap("event:cred success");
            break;
        case WIFI_PROV_END:
            log_provisioning_heap("event:end");
            break;
        default:
            break;
        }
        return;
    }

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            s_sta_retry_count = 0;
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_sta_retry_count = 0;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_sta_retry_count < APP_WIFI_STA_MAX_RETRY) {
                ++s_sta_retry_count;
                ESP_LOGI(TAG, "Wi-Fi disconnected, retry %u/%u", s_sta_retry_count, APP_WIFI_STA_MAX_RETRY);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Wi-Fi disconnected, giving up after %u retries", APP_WIFI_STA_MAX_RETRY);
            }
            break;
        default:
            break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected with IP address " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, APP_WIFI_CONNECTED_EVENT | APP_WIFI_PROVISIONED_EVENT);
    }
}

static esp_err_t app_wifi_init_base(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = app_settings_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_wifi_event_handler, NULL));

    if (s_wifi_netif == NULL) {
        s_wifi_netif = esp_netif_create_default_wifi_sta();
        if (s_wifi_netif == NULL) {
            return ESP_FAIL;
        }
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));

    s_wifi_initialized = true;
    return ESP_OK;
}

static void app_wifi_get_service_name(char *buffer, size_t buffer_size)
{
    uint8_t mac[6] = {0};

    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(buffer, buffer_size, APP_PROVISIONING_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

esp_err_t app_wifi_start_station_if_provisioned(void)
{
    esp_err_t ret         = app_wifi_init_base();
    bool      provisioned = false;

    if (ret != ESP_OK) {
        return ret;
    }

    log_provisioning_heap("status probe:entry");

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        /* This path only probes stored credentials. Releasing BTDM here would
         * permanently disable BLE provisioning for the rest of the boot. */
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };

    ret = wifi_prov_mgr_init(config);
    if (ret != ESP_OK) {
        return ret;
    }
    log_provisioning_heap("status probe:after manager init");

    ret = wifi_prov_mgr_is_provisioned(&provisioned);
    log_provisioning_heap("status probe:after query");
    wifi_prov_mgr_deinit();
    log_provisioning_heap("status probe:after manager deinit");
    if (ret != ESP_OK) {
        return ret;
    }

    s_wifi_is_provisioned = provisioned;

    if (!provisioned) {
        ESP_LOGI(TAG, "Wi-Fi not provisioned, waiting for touch-triggered setup");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Wi-Fi already provisioned, starting station mode");
    return app_wifi_start_station();
}

bool app_wifi_is_provisioned(void)
{
    return s_wifi_is_provisioned;
}

bool app_wifi_get_ip_text(char *buffer, size_t buffer_size)
{
    esp_netif_ip_info_t ip_info;
    int                 length;

    if (buffer == NULL || buffer_size == 0 || s_wifi_netif == NULL) {
        return false;
    }

    if (esp_netif_get_ip_info(s_wifi_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    length = snprintf(buffer, buffer_size, IPSTR, IP2STR(&ip_info.ip));
    return length > 0 && (size_t)length < buffer_size;
}

bool app_wifi_get_rssi_dbm(int8_t *rssi_dbm)
{
    wifi_ap_record_t ap_info;

    if (rssi_dbm == NULL) {
        return false;
    }

    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return false;
    }

    *rssi_dbm = ap_info.rssi;
    return true;
}

esp_err_t app_wifi_provisioning_begin(dgx_screen_t *screen, uint32_t timeout_ms)
{
    char      service_name[sizeof(APP_PROVISIONING_PREFIX "XXXXXX")];
    esp_err_t ret = app_wifi_init_base();

    if (ret != ESP_OK) {
        return ret;
    }

    log_provisioning_heap("begin:entry");

    if (s_provisioning_session.active) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_provisioning_session, 0, sizeof(s_provisioning_session));

    ret = esp_wifi_get_config(WIFI_IF_STA, &s_provisioning_session.previous_config);
    if (ret != ESP_OK) {
        return ret;
    }

    s_provisioning_session.had_previous_config = app_wifi_has_sta_config(&s_provisioning_session.previous_config);

    ret = app_wifi_stop_station();
    if (ret != ESP_OK) {
        return ret;
    }
    log_provisioning_heap("begin:after station stop");

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        /* The app may need to re-enter provisioning again later in the same
         * boot, so do not use the FREE_BTDM handler here because BT memory
         * release is irreversible. Releasing classic BT is still safe. */
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT,
    };

    ret = wifi_prov_mgr_init(config);
    if (ret != ESP_OK) {
        goto restore;
    }
    log_provisioning_heap("begin:after manager init");

    ret = wifi_prov_mgr_disable_auto_stop(1000);
    if (ret != ESP_OK) {
        goto deinit_and_restore;
    }
    log_provisioning_heap("begin:after auto-stop config");

    ret = wifi_prov_mgr_reset_provisioning();
    if (ret != ESP_OK) {
        goto deinit_and_restore;
    }
    log_provisioning_heap("begin:after reset");

    app_wifi_get_service_name(service_name, sizeof(service_name));
    xEventGroupClearBits(s_wifi_event_group, APP_WIFI_CONNECTED_EVENT | APP_WIFI_PROVISIONED_EVENT);

    ret = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, APP_PROVISIONING_POP, service_name, NULL);
    if (ret != ESP_OK) {
        goto deinit_and_restore;
    }
    log_provisioning_heap("begin:after BLE start");

    s_provisioning_session.active      = true;
    s_provisioning_session.timeout_ms  = timeout_ms;
    s_provisioning_session.deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    if (screen != NULL) {
        char                payload[160];
        esp_qrcode_config_t qr_config = ESP_QRCODE_CONFIG_DEFAULT();
        int                 written =
            snprintf(payload, sizeof(payload), "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}",
                     service_name, APP_PROVISIONING_POP, APP_PROVISIONING_TRANSPORT);

        if (written > 0 && (size_t)written < sizeof(payload)) {
            s_qr_screen            = screen;
            qr_config.display_func = app_draw_qrcode;
            esp_qrcode_generate(&qr_config, payload);
            log_provisioning_heap("begin:after qr");
        } else {
            ESP_LOGW(TAG, "QR payload truncated, skipping QR display");
        }
    }

    log_provisioning_heap("begin:ready");

    return ESP_OK;

deinit_and_restore:
    wifi_prov_mgr_deinit();
    log_provisioning_heap("begin:error after manager deinit");
restore:
    if (s_provisioning_session.had_previous_config) {
        (void)app_wifi_restore_previous_config();
        log_provisioning_heap("begin:error after station restore");
    }
    memset(&s_provisioning_session, 0, sizeof(s_provisioning_session));
    return ret;
}

app_wifi_provisioning_state_t app_wifi_provisioning_step(void)
{
    if (!s_provisioning_session.active) {
        return AppWifiProvisioningIdle;
    }

    if (s_provisioning_session.provisioning_succeeded) {
        return app_wifi_finish_provisioning(s_provisioning_session.credentials_changed
                                                ? AppWifiProvisioningFinishedRestart
                                                : AppWifiProvisioningFinishedNoChange,
                                            false);
    }

    if (s_provisioning_session.provisioning_failed) {
        ESP_LOGI(TAG, "Provisioning aborted by credential failure");
        if (s_provisioning_session.had_previous_config) {
            return app_wifi_finish_provisioning(AppWifiProvisioningFinishedNoChange, true);
        }
        return app_wifi_finish_provisioning(AppWifiProvisioningFinishedError, false);
    }

    if (esp_timer_get_time() < s_provisioning_session.deadline_us) {
        return AppWifiProvisioningActive;
    }

    ESP_LOGI(TAG, "Provisioning timed out after %lu ms, returning to normal mode",
             (unsigned long)s_provisioning_session.timeout_ms);
    if (s_provisioning_session.had_previous_config) {
        return app_wifi_finish_provisioning(AppWifiProvisioningFinishedNoChange, true);
    }
    return app_wifi_finish_provisioning(AppWifiProvisioningFinishedError, false);
}