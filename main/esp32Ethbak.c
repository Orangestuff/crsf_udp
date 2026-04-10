#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "iot_usbh_ecm.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"

static const char *TAG = "USB_ETH_INIT";

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "   USB ETHERNET LINK IS UP!     ");
    ESP_LOGI(TAG, "================================");
    // IP2STR and IPSTR are handy macros that format the raw bytes into a standard "192.168.X.X" string
    ESP_LOGI(TAG, "IP Address:  " IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "Subnet Mask: " IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "Gateway:     " IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "================================");
}


// USB Daemon Task
static void usb_host_lib_daemon_task(void *arg) {
    while (1) {
        usb_host_lib_handle_events(portMAX_DELAY, NULL);
    }
}

void app_main(void)
{
    //  TCP/IP event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Register callback function to listen for the "Got IP" event
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // Install the USB Host Library
    ESP_LOGI(TAG, "Installing USB Host Library...");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false, 
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    
    // Start the USB Host Daemon task
    xTaskCreatePinnedToCore(usb_host_lib_daemon_task, "usb_daemon", 16384, NULL, 5, NULL, 0);

    // Configure the USB ECM Ethernet Driver
    ESP_LOGI(TAG, "Configuring USB ECM adapter...");
    
    // Explicitly initialize the struct instead of using a macro.
    // Setting match_id_list to NULL allows it to auto-detect any standard adapter.
    iot_usbh_ecm_config_t ecm_config = {
        .match_id_list = NULL 
    };
    
    iot_eth_driver_t *eth_driver = NULL;

    // Create the driver instance
    ESP_ERROR_CHECK(iot_eth_new_usb_ecm(&ecm_config, &eth_driver));

    // Attach to the ESP-NETIF stack (Allows DHCP to work)
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, iot_eth_new_netif_glue(eth_driver)));

    // Start the Ethernet driver
    ESP_LOGI(TAG, "Starting Ethernet Driver. Waiting for USB adapter to be plugged in...");
    ESP_ERROR_CHECK(iot_eth_start(eth_driver));
    // --- NEW: STATIC IP CONFIGURATION ---
    /*
    esp_netif_dhcpc_stop(eth_netif);

    // Create a struct to hold our manual IP settings
    esp_netif_ip_info_t ip_info;
    
    // Convert standard strings into the raw bytes the network stack needs
    ip_info.ip.addr = esp_ip4addr_aton("192.168.5.10");      // The ESP32's new IP
    ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0"); // Standard subnet
    ip_info.gw.addr = esp_ip4addr_aton("192.168.5.1");       // Gateway (Your PC)

    // Force the IP into the network interface
    esp_netif_set_ip_info(eth_netif, &ip_info);
    */
    
}