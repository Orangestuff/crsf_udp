#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include <arpa/inet.h>
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "crc.h"
#include "html.h"

// ---------------------------------------------------------
// WT32-ETH01 v1.4 Pin Mapping
// ---------------------------------------------------------
#define ETH_PHY_ADDR         1
#define ETH_PHY_RST_GPIO     16
#define ETH_MDC_GPIO         23
#define ETH_MDIO_GPIO        18

// ---------------------------------------------------------
// CRSF Configuration
// ---------------------------------------------------------
#define CRSF_UART_PORT      UART_NUM_2
#define CRSF_RX_PIN         5   
#define CRSF_DUMMY_TX_PIN   4   
#define UART_BUF_SIZE       1024

// ---------------------------------------------------------
// Global Configuration Variables (Defaults)
// ---------------------------------------------------------
char target_ip[16] = "192.168.88.100"; 
int target_port = 8888;               

int use_static = 0; // 0 = DHCP, 1 = Static IP
char local_ip[16] = "192.168.88.250";
char local_mask[16] = "255.255.255.0";
char local_gw[16] = "192.168.88.1";

// ---------------------------------------------------------
// NVS Storage
// ---------------------------------------------------------
void load_config_from_nvs() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        size_t size = sizeof(target_ip);
        nvs_get_str(my_handle, "ip", target_ip, &size);
        nvs_get_i32(my_handle, "port", (int32_t*)&target_port);
        
        size = sizeof(local_ip);
        nvs_get_str(my_handle, "loc_ip", local_ip, &size);
        size = sizeof(local_mask);
        nvs_get_str(my_handle, "loc_mask", local_mask, &size);
        size = sizeof(local_gw);
        nvs_get_str(my_handle, "loc_gw", local_gw, &size);
        
        nvs_get_i32(my_handle, "use_stat", (int32_t*)&use_static);
        nvs_close(my_handle);
    }
}

void save_config_to_nvs() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_str(my_handle, "ip", target_ip);
        nvs_set_i32(my_handle, "port", target_port);
        
        nvs_set_str(my_handle, "loc_ip", local_ip);
        nvs_set_str(my_handle, "loc_mask", local_mask);
        nvs_set_str(my_handle, "loc_gw", local_gw);
        nvs_set_i32(my_handle, "use_stat", use_static);
        
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

// ---------------------------------------------------------
// HTTP Server Handlers
// ---------------------------------------------------------
static esp_err_t index_get_handler(httpd_req_t *req) {
    // Allocate the large 4KB buffer on the heap instead of the stack
    char *response = malloc(4096);
    if (response == NULL) {
        printf("ERROR: Failed to allocate memory for webpage\n");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const char* checked_attr = (use_static == 1) ? "checked" : "";
    
    snprintf(response, 4096, html_template, 
             target_ip, target_port, checked_attr, 
             local_ip, local_mask, local_gw);
             
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    
    // Free the memory immediately after sending
    free(response); 
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    char buf[1024];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    char new_ip[16] = {0}, new_port_str[10] = {0};
    char new_loc_ip[16] = {0}, new_loc_mask[16] = {0}, new_loc_gw[16] = {0};
    char use_static_str[5] = {0};

    // Extract values
    httpd_query_key_value(buf, "ip", new_ip, sizeof(new_ip));
    httpd_query_key_value(buf, "port", new_port_str, sizeof(new_port_str));
    httpd_query_key_value(buf, "local_ip", new_loc_ip, sizeof(new_loc_ip));
    httpd_query_key_value(buf, "local_mask", new_loc_mask, sizeof(new_loc_mask));
    httpd_query_key_value(buf, "local_gw", new_loc_gw, sizeof(new_loc_gw));

    // Checkbox presence evaluation
    if (httpd_query_key_value(buf, "use_static", use_static_str, sizeof(use_static_str)) == ESP_OK) {
        use_static = 1;
    } else {
        use_static = 0;
    }

    // Assign back to globals safely
    if (strlen(new_ip) > 0) strncpy(target_ip, new_ip, sizeof(target_ip) - 1);
    if (strlen(new_port_str) > 0) target_port = atoi(new_port_str);
    if (strlen(new_loc_ip) > 0) strncpy(local_ip, new_loc_ip, sizeof(local_ip) - 1);
    if (strlen(new_loc_mask) > 0) strncpy(local_mask, new_loc_mask, sizeof(local_mask) - 1);
    if (strlen(new_loc_gw) > 0) strncpy(local_gw, new_loc_gw, sizeof(local_gw) - 1);

    save_config_to_nvs();

    // Force browser to redirect to the new static IP (or current IP if DHCP)
    char redirect_url[64];
    if (use_static) {
        snprintf(redirect_url, sizeof(redirect_url), "http://%s/", local_ip);
    } else {
        snprintf(redirect_url, sizeof(redirect_url), "/");
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", redirect_url);
    httpd_resp_send(req, NULL, 0);

    // Give the ESP32 a half-second to finish sending the HTTP packet, then reboot
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

void start_webserver() {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_uri_t uri_post = { .uri = "/config", .method = HTTP_POST, .handler = config_post_handler, .user_ctx = NULL };
        
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_post);
        printf("Web Server Started Successfully.\n");
    }
}

// ---------------------------------------------------------
// The Bridge Task (UDP Sending + Parsing Loop)
// ---------------------------------------------------------
void crsf_rx_udp_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        printf("Unable to create socket: errno %d\n", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;

    printf("CRSF Sliding Window Parser Started on RX Pin %d\n", CRSF_RX_PIN);

    uint8_t window[256];
    uint16_t window_len = 0;
    uint8_t uart_data[128];

    while (1) {
        int rx_bytes = uart_read_bytes(CRSF_UART_PORT, uart_data, sizeof(uart_data), 10 / portTICK_PERIOD_MS);
        
        dest_addr.sin_addr.s_addr = inet_addr(target_ip);
        dest_addr.sin_port = htons(target_port);

        if (rx_bytes > 0) {
            for (int i = 0; i < rx_bytes; i++) {
                if (window_len < sizeof(window)) window[window_len++] = uart_data[i];
            }
            
            while (window_len >= 4) { 
                uint8_t sync = window[0];
                if (sync != 0xC8 && sync != 0xEE && sync != 0xEA) {
                    memmove(window, window + 1, window_len - 1); 
                    window_len--; continue;
                }
                
                uint8_t frame_len = window[1];
                if (frame_len < 2 || frame_len > 62) {
                    memmove(window, window + 1, window_len - 1); 
                    window_len--; continue;
                }
                
                uint8_t total_frame_size = frame_len + 2; 
                if (window_len < total_frame_size) break; 
                
                uint8_t calculated_crc = crsf_crc8(&window[2], frame_len - 1);
                uint8_t received_crc = window[total_frame_size - 1];
                
                if (calculated_crc == received_crc) {
                    process_crsf_packet(window);
                    sendto(sock, window, total_frame_size, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                    memmove(window, window + total_frame_size, window_len - total_frame_size);
                    window_len -= total_frame_size;
                } else {
                    memmove(window, window + 1, window_len - 1);
                    window_len--;
                }
            }
        }
    }
}

// ---------------------------------------------------------
// Ethernet Event Handlers
// ---------------------------------------------------------
static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
            printf("Ethernet Link Up\n");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            printf("Ethernet Link Down\n");
            break;
        default:
            break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    printf("\nSUCCESS: Network Connection Established!\n");
    printf("========================================\n");
    printf("Assigned IP:  " IPSTR "\n", IP2STR(&ip_info->ip));
    printf("Target UDP:   %s:%d\n", target_ip, target_port);
    printf("========================================\n\n");

    start_webserver();
    xTaskCreate(crsf_rx_udp_task, "crsf_rx_udp", 4096, NULL, 10, NULL);
}

// ---------------------------------------------------------
// Main Entry
// ---------------------------------------------------------
void app_main(void)
{
    // --- 1. Initialize NVS ---
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    load_config_from_nvs();

    // --- 2. Initialize UART ---
    uart_config_t uart_config = {
        .baud_rate = 400000,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(CRSF_UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CRSF_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CRSF_UART_PORT, CRSF_DUMMY_TX_PIN, CRSF_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_line_inverse(CRSF_UART_PORT, UART_SIGNAL_RXD_INV));

    // --- 3. Initialize Network Stack ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_mdc_gpio_num = ETH_MDC_GPIO;
    esp32_emac_config.smi_mdio_gpio_num = ETH_MDIO_GPIO;
    
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // --- 4. STATIC IP CONFIGURATION ---
    if (use_static == 1) {
        esp_netif_dhcpc_stop(eth_netif);
        esp_netif_ip_info_t ip_info;
        ip_info.ip.addr = inet_addr(local_ip);
        ip_info.netmask.addr = inet_addr(local_mask);
        ip_info.gw.addr = inet_addr(local_gw);
        esp_netif_set_ip_info(eth_netif, &ip_info);
        printf("STATIC IP INITIATED: %s\n", local_ip);
    } else {
        printf("DHCP INITIATED\n");
    }

    // --- 5. Start Ethernet ---
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}