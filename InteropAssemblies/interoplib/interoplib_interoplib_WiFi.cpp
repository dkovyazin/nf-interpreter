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
#include "lwip/inet.h"
#include <lwip/sockets.h>

#define LOG_TAG "interoplib WiFi"

// Static IP configuration for the AP interface
#define AP_STATIC_IP_ADDR       "192.168.4.1"
#define AP_STATIC_NETMASK_ADDR  "255.255.255.0"
#define AP_STATIC_GW_ADDR       "192.168.4.1" // AP itself is the gateway

// DHCP Server Configuration
#define DHCP_LEASE_START_IP     "192.168.4.100"
#define DHCP_LEASE_END_IP       "192.168.4.150"
#define DHCP_LEASE_TIME_MIN     120 // Lease time in minutes
#define DNS_SERVER_IP           "192.168.4.1" // AP itself as DNS or use 8.8.8.8 for Google DNS

using namespace interoplib::interoplib;

static wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT)
		ESP_LOGI(LOG_TAG, "WIFI_EVENT: %d", (int)event_id);
	else if (event_base == IP_EVENT)
		ESP_LOGI(LOG_TAG, "IP_EVENT: %d", (int)event_id);
}

static int ssize(const char* s)
{
    for (int i = 0; ; i++)
        if (s[i] == 0)
            return i;

    return 0;
}

void WiFi::NativeSetupAP( const char* ssid, const char* password, HRESULT &hr )
{
    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_ap();
    assert(sta_netif);

    // ESP_LOGI(LOG_TAG, "Stopping DHCP server...");
    // ESP_ERROR_CHECK(esp_netif_dhcps_stop(sta_netif));
//
    // ESP_LOGI(LOG_TAG, "Configuring static IP for AP interface...");
    // esp_netif_ip_info_t ip_info;
    // memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
    // inet_pton(AF_INET, AP_STATIC_IP_ADDR, &ip_info.ip);
    // inet_pton(AF_INET, AP_STATIC_GW_ADDR, &ip_info.gw);
    // inet_pton(AF_INET, AP_STATIC_NETMASK_ADDR, &ip_info.netmask);
    // ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip_info));
    // ESP_LOGI(LOG_TAG, "AP Static IP configured: IP=" IPSTR ", GW=" IPSTR ", Mask=" IPSTR,
    //         IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));
//
    // // 3. Configure DHCP server options
    // ESP_LOGI(LOG_TAG, "Configuring DHCP server options...");
//
    // // Set IP address lease pool
    // dhcps_lease_t lease_opt;
    // memset(&lease_opt, 0, sizeof(dhcps_lease_t));
    // inet_pton(AF_INET, DHCP_LEASE_START_IP, &lease_opt.start_ip);
    // inet_pton(AF_INET, DHCP_LEASE_END_IP, &lease_opt.end_ip);
    // esp_err_t opt_ret = esp_netif_dhcps_option(sta_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease_opt, sizeof(lease_opt));
    // if (opt_ret == ESP_OK) {
    //     ESP_LOGI(LOG_TAG, "DHCP IP Pool configured: %s - %s", DHCP_LEASE_START_IP, DHCP_LEASE_END_IP);
    // } else {
    //     ESP_LOGE(LOG_TAG, "Failed to set DHCP IP Pool (err=0x%x)", opt_ret);
    // }
//
    // // Set IP address lease time (in minutes, converted to seconds for LwIP)
    // // Note: LwIP DHCP server expects lease time in seconds. The ESP_NETIF_IP_ADDRESS_LEASE_TIME option takes minutes.
    // // Let's check the esp_netif_dhcps_option documentation or source for expected unit.
    // // According to esp_netif_lwip.c, it takes seconds for ESP_NETIF_IP_ADDRESS_LEASE_TIME
    // // However, older comments and Kconfig might mention minutes. Let's assume seconds based on LwIP.
    // // The header `esp_netif_types.h` says "The IP address lease time (in seconds)"
    // uint32_t lease_time_sec = DHCP_LEASE_TIME_MIN * 60;
    // opt_ret = esp_netif_dhcps_option(sta_netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lease_time_sec, sizeof(lease_time_sec));
    // if (opt_ret == ESP_OK) {
    //     ESP_LOGI(LOG_TAG, "DHCP Lease Time configured: %d minutes (%d seconds)", DHCP_LEASE_TIME_MIN, lease_time_sec);
    // } else {
    //     ESP_LOGE(LOG_TAG, "Failed to set DHCP Lease Time (err=0x%x)", opt_ret);
    // }
//
    // // Set DNS Server (Router option is usually AP's IP, set by default)
    // esp_netif_dns_info_t dns_info;
    // memset(&dns_info, 0, sizeof(esp_netif_dns_info_t));
    // inet_pton(AF_INET, DNS_SERVER_IP, &dns_info.ip.u_addr.ip4);
    // dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    // // Option ESP_NETIF_DOMAIN_NAME_SERVER configures DHCP option 6 (DNS)
    // opt_ret = esp_netif_dhcps_option(sta_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dns_info, sizeof(dns_info));
    //  if (opt_ret == ESP_OK) {
    //     ESP_LOGI(LOG_TAG, "DHCP DNS Server configured: %s", DNS_SERVER_IP);
    // } else {
    //     ESP_LOGE(LOG_TAG, "Failed to set DHCP DNS Server (err=0x%x)", opt_ret);
    // }
//
    // // 4. Start DHCP server
    // ESP_LOGI(LOG_TAG, "Starting DHCP server...");
    // ESP_ERROR_CHECK(esp_netif_dhcps_start(sta_netif));
    // ESP_LOGI(LOG_TAG, "DHCP server started.");

	/* Initialize WiFi */
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

	/* Register WiFi and IP event handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_ap_config;
    memset(&wifi_ap_config, 0, sizeof(wifi_ap_config));

    memcpy(wifi_ap_config.ap.ssid, (const void*)ssid, sizeof(wifi_ap_config.ap.ssid));
    memcpy(wifi_ap_config.ap.password, (const void*)password, sizeof(wifi_ap_config.ap.password));

    wifi_ap_config.ap.ssid_len = ssize(ssid);
    wifi_ap_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_ap_config.ap.max_connection = WIFI_AP_MAX_CONN;
    wifi_ap_config.ap.pmf_cfg.required = false;

    if (ssize(password) == 0)
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
