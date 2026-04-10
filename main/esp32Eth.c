#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"

// Raw LwIP Headers
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "netif/etharp.h"

// CherryUSB Header
#include "usbh_core.h"

// --- THE BULLETPROOF C TRICK ---
// Bypass the missing header by declaring the external CherryUSB transmit function manually
extern int usbh_net_tx(uint8_t *buffer, uint32_t buflen);

static const char *TAG = "CHERRY_RTL";
static struct netif rtl_netif;
static bool netif_initialized = false;

// ==============================================================
// 1. THE BRIDGE: LwIP -> USB (Transmit)
// ==============================================================
static err_t rtl_netif_output(struct netif *netif, struct pbuf *p) {
    static uint8_t tx_buf[1514];
    pbuf_copy_partial(p, tx_buf, p->tot_len, 0);
    
    // Push the raw IP packets directly into the CherryUSB pipeline
    usbh_net_tx(tx_buf, p->tot_len); 
    return ERR_OK;
}

// ==============================================================
// 2. THE BRIDGE: USB -> LwIP (Receive)
// ==============================================================
void usbh_net_rx_cb(uint8_t *buf, uint32_t len) {
    if (!netif_initialized) return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (p != NULL) {
        pbuf_take(p, buf, len);
        if (tcpip_input(p, &rtl_netif) != ERR_OK) {
            pbuf_free(p);
        }
    }
}

// ==============================================================
// 3. HARDWARE STATE: The Link Callback
// ==============================================================
void usbh_net_linkstatus_cb(bool link_up) {
    if (!netif_initialized) {
        ip4_addr_t ipaddr = {0}, netmask = {0}, gw = {0};

        rtl_netif.name[0] = 'r';
        rtl_netif.name[1] = 't';
        rtl_netif.output = etharp_output;
        rtl_netif.linkoutput = rtl_netif_output;
        rtl_netif.mtu = 1500;
        rtl_netif.flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;

        // Generate a safe, local MAC address
        rtl_netif.hwaddr[0] = 0x02; 
        rtl_netif.hwaddr[1] = 0x00;
        rtl_netif.hwaddr[2] = 0x00;
        rtl_netif.hwaddr[3] = 0x12;
        rtl_netif.hwaddr[4] = 0x34;
        rtl_netif.hwaddr[5] = 0x56;
        rtl_netif.hwaddr_len = 6;

        netif_add(&rtl_netif, &ipaddr, &netmask, &gw, NULL, NULL, tcpip_input);
        netif_set_default(&rtl_netif);
        netif_set_up(&rtl_netif);
        netif_initialized = true;
    }

    if (link_up) {
        ESP_LOGI(TAG, "Ethernet Cable Connected! Starting DHCP Client...");
        netif_set_link_up(&rtl_netif);
        dhcp_start(&rtl_netif); 
    } else {
        ESP_LOGW(TAG, "Ethernet Cable Disconnected!");
        netif_set_link_down(&rtl_netif);
    }
}

// ==============================================================
// 4. APPLICATION LOGIC: Waiting for the IP
// ==============================================================
void dhcp_monitor_task(void *arg) {
    while (1) {
        if (netif_initialized && netif_is_up(&rtl_netif) && !ip4_addr_isany_val(*netif_ip4_addr(&rtl_netif))) {
            ESP_LOGI(TAG, "================================");
            ESP_LOGI(TAG, "   CHERRY-USB ETHERNET UP!      ");
            ESP_LOGI(TAG, "================================");
            ESP_LOGI(TAG, "IP Address:  %s", ip4addr_ntoa(netif_ip4_addr(&rtl_netif)));
            ESP_LOGI(TAG, "Subnet Mask: %s", ip4addr_ntoa(netif_ip4_netmask(&rtl_netif)));
            ESP_LOGI(TAG, "Gateway:     %s", ip4addr_ntoa(netif_ip4_gw(&rtl_netif)));
            ESP_LOGI(TAG, "================================");
            
            vTaskDelete(NULL); 
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    // 1. Initialize ESP-IDF networking core
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Start our background monitor task
    xTaskCreate(dhcp_monitor_task, "dhcp_monitor", 4096, NULL, 5, NULL);

    // 3. Initialize CherryUSB Stack
    ESP_LOGI(TAG, "Initializing CherryUSB Stack...");
    usbh_initialize(0, 0x60080000); 
}