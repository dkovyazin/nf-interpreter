//-----------------------------------------------------------------------------
//
//                   ** WARNING! **
//    This file was generated automatically by a tool.
//    Re-running the tool will overwrite this file.
//    You should copy this file to a custom location
//    before adding any customization in the copy to
//    prevent loss of your changes when the tool is
//    re-run.
//
//-----------------------------------------------------------------------------

#include "interoplib.h"
#include "interoplib_interoplib_WiFi.h"

#include "interoplib_config.h"
#include <esp_wifi.h>

#define LOG_TAG "interoplib WiFi"

using namespace interoplib::interoplib;

static wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT)
		ESP_LOGI(LOG_TAG, "WIFI_EVENT: %d", (int)event_id);
	else if (event_base == IP_EVENT)
		ESP_LOGI(LOG_TAG, "IP_EVENT: %d", (int)event_id);
}

void WiFi::NativeSetup( const char* ssid, const char* password, HRESULT &hr )
{
    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());

	/* Initialize WiFi */
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

	/* Register WiFi and IP event handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_ap();
    assert(sta_netif);

    wifi_config_t wifi_ap_config;
    memset(&wifi_ap_config, 0, sizeof(wifi_ap_config));

    memcpy(wifi_ap_config.ap.ssid, (const void*)ssid, sizeof(wifi_ap_config.ap.ssid));
    memcpy(wifi_ap_config.ap.password, (const void*)password, sizeof(wifi_ap_config.ap.password));

    wifi_ap_config.ap.ssid_len = sizeof(ssid);
    wifi_ap_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_ap_config.ap.max_connection = WIFI_AP_MAX_CONN;
    wifi_ap_config.ap.pmf_cfg.required = false;

    if (sizeof(password) == 0)
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    else
        wifi_ap_config.ap.authmode = WIFI_AUTH_MODE;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    ESP_LOGI(LOG_TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d", *ssid, *password, WIFI_AP_CHANNEL);

	/* Start WiFi */
	hr = esp_wifi_start();
}

void WiFi::NativeConnect( const char* ssid, const char* password, HRESULT &hr )
{
    ////////////////////////////////
    // implementation starts here //

    hr = ESP_OK;

    // implementation ends here   //
    ////////////////////////////////
}

void WiFi::NativeStop(  HRESULT &hr )
{
    ////////////////////////////////
    // implementation starts here //

    hr = esp_wifi_stop();

    // implementation ends here   //
    ////////////////////////////////
}
