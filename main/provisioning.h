#ifndef CO2STATION_PROVISIONING_H
#define CO2STATION_PROVISIONING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "dgx_screen.h"

typedef enum
{
    AppWifiProvisioningIdle,
    AppWifiProvisioningActive,
    AppWifiProvisioningFinishedNoChange,
    AppWifiProvisioningFinishedRestart,
    AppWifiProvisioningFinishedError,
} app_wifi_provisioning_state_t;

/*
 * Start station mode only when valid Wi-Fi credentials are already stored.
 *
 * Returns ESP_OK both when station mode starts successfully and when the device
 * is simply still unprovisioned.
 */
esp_err_t app_wifi_start_station_if_provisioned(void);
bool      app_wifi_is_provisioned(void);
bool      app_wifi_get_ip_text(char *buffer, size_t buffer_size);
bool      app_wifi_get_rssi_dbm(int8_t *rssi_dbm);

/*
 * Start a non-blocking BLE provisioning session and draw its QR code on the
 * supplied screen.
 *
 * The session remains active until app_wifi_provisioning_step() reports a
 * terminal state. If the session times out without a successful credential
 * update, the previous Wi-Fi config is restored automatically.
 */
esp_err_t app_wifi_provisioning_begin(dgx_screen_t *screen, uint32_t timeout_ms);

/*
 * Advance the active provisioning session from the main loop.
 *
 * While the session is waiting for user action this returns
 * AppWifiProvisioningActive. On completion it returns one of the terminal
 * states so the caller can either redraw the normal UI or restart the device.
 */
app_wifi_provisioning_state_t app_wifi_provisioning_step(void);

#endif