#include <lwip/netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs_flash.h"

#define DEBUG_PRINTS

#ifdef DEBUG_PRINTS
#define DEBUG_LOG(fmt, ...) ESP_LOGI("YEELIGHT", fmt, ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...)
#endif

extern const uint8_t private_ips_start[] asm("_binary_PRIVATE_txt_start");
extern const uint8_t private_ips_end[] asm("_binary_PRIVATE_txt_end");

namespace Const {

constexpr int MAX_BULBS                 = 32;
constexpr int BULB_PORT                 = 55443;
constexpr const char* SSDP_ADDR         = "239.255.255.250";
constexpr int SSDP_PORT                 = 1982;
constexpr const char* SSDP_DISCOVER_MSG = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1982\r\nMAN: \"ssdp:discover\"\r\nST: wifi_bulb\r\n";
constexpr int WIFI_CONNECTED_BIT        = BIT0;
constexpr gpio_num_t BUTTON_1           = GPIO_NUM_1;
constexpr const char* PROV_PREFIX       = "PROV_";

} // namespace Const

enum class BulbCommand {
    SET_POWER,
    SET_RGB,
    SET_BRIGHTNESS,
    SET_COLOR_TEMP,
    TOGGLE,
};

const char* CommandStrings[] = {
    "set_power",
    "set_rgb",
    "set_bright",
    "set_ct_abx",
    "toggle",
};

namespace Global {

namespace Bulb {

char names[Const::MAX_BULBS][64];
char ips[Const::MAX_BULBS][16];
int sockets[Const::MAX_BULBS];
int ids[Const::MAX_BULBS];
int count = 0;

} // namespace Bulb

EventGroupHandle_t wifi_event_group;

} // namespace Global

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == NETWORK_PROV_EVENT) {

        switch (event_id) {
        case NETWORK_PROV_START:          DEBUG_LOG("Provisioning started"); break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            [[maybe_unused]] wifi_sta_config_t* wifi_conf = (wifi_sta_config_t*)event_data;
            DEBUG_LOG("Received WiFi credentials");
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS: DEBUG_LOG("Provisioning successful"); break;
        case NETWORK_PROV_END:               {
            DEBUG_LOG("Provisioning ended");
            network_prov_mgr_deinit();
            break;
        }
        }

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        DEBUG_LOG("WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        DEBUG_LOG("WiFi connected and got IP");
        xEventGroupSetBits(Global::wifi_event_group, Const::WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        DEBUG_LOG("WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    }
}

inline void configure_socket_timeout(const int fd, const int sec, const int usec) {
    const struct timeval tv = {sec, usec};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

inline int net_open_tcp(const char* ip_address) {
    DEBUG_LOG("Opening TCP socket to %s", ip_address);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        DEBUG_LOG("Failed to create socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(Const::BULB_PORT);

    inet_pton(AF_INET, ip_address, &addr.sin_addr);
    configure_socket_timeout(fd, 1, 0);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        DEBUG_LOG("Failed to connect to %s", ip_address);
        close(fd);
        return -1;
    }

    DEBUG_LOG("Successfully connected to %s", ip_address);
    return fd;
}

namespace Bulb {

inline void ensure_connected(const int index) {

    if (Global::Bulb::sockets[index] >= 0) {
        return;
    }

    Global::Bulb::sockets[index] = net_open_tcp(Global::Bulb::ips[index]);
}

inline void dispatch(const int index, const BulbCommand cmd, const char* parameters) {
    ensure_connected(index);

    if (Global::Bulb::sockets[index] < 0) {
        DEBUG_LOG("Dispatch aborted: not connected to bulb %d", index);
        return;
    }

    char message_buffer[512];
    const char* method_str = CommandStrings[static_cast<int>(cmd)];

    const int len = snprintf(message_buffer, sizeof(message_buffer), "{\"id\":%d,\"method\":\"%s\",\"params\":%s}\r\n", Global::Bulb::ids[index]++, method_str, parameters);

    DEBUG_LOG("Sending command to bulb %d: %s", index, method_str);

    if (send(Global::Bulb::sockets[index], message_buffer, len, 0) < 0) {
        DEBUG_LOG("Failed to send command to bulb %d", index);
        close(Global::Bulb::sockets[index]);
        Global::Bulb::sockets[index] = -1;

        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    char response_buffer[512];

    int recv_len = recv(Global::Bulb::sockets[index], response_buffer, sizeof(response_buffer) - 1, MSG_DONTWAIT);

    if (recv_len > 0) {
        response_buffer[recv_len] = '\0';
        DEBUG_LOG("Response from bulb %d: %s", index, response_buffer);
    } else {
        DEBUG_LOG("No response from bulb %d", index);
    }
}

inline void add(const char* target_name, const char* target_ip) {
    if (Global::Bulb::count >= Const::MAX_BULBS) {
        return;
    }

    for (int i = 0; i < Global::Bulb::count; i++) {
        if (strcmp(Global::Bulb::ips[i], target_ip) == 0) {
            return;
        }
    }

    strncpy(Global::Bulb::names[Global::Bulb::count], target_name, 63);
    strncpy(Global::Bulb::ips[Global::Bulb::count], target_ip, 15);

    Global::Bulb::sockets[Global::Bulb::count] = -1;
    Global::Bulb::ids[Global::Bulb::count]     = 1;

    DEBUG_LOG("Added bulb %d: %s (%s)", Global::Bulb::count, target_name, target_ip);

    Global::Bulb::count++;
}

inline void discover_all(int timeout_ms) {
    DEBUG_LOG("Starting SSDP discovery...");
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        DEBUG_LOG("Failed to create UDP socket for discovery");
        return;
    }

    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family      = AF_INET;
    mcast_addr.sin_addr.s_addr = inet_addr(Const::SSDP_ADDR);
    mcast_addr.sin_port        = htons(Const::SSDP_PORT);

    sendto(fd, Const::SSDP_DISCOVER_MSG, strlen(Const::SSDP_DISCOVER_MSG), 0, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));

    configure_socket_timeout(fd, timeout_ms / 1000, (timeout_ms % 1000) * 1000);

    char buffer[1024];

    while (recv(fd, buffer, sizeof(buffer) - 1, 0) > 0) {
        char* loc = strstr(buffer, "Location: yeelight://");
        if (!loc)
            continue;
        loc += 21;
        char* colon = strchr(loc, ':');
        if (!colon)
            continue;
        *colon = '\0';
        DEBUG_LOG("Discovered bulb at %s", loc);
        add("discovered", loc);
    }

    DEBUG_LOG("Discovery finished");
    close(fd);
}

void parse_embedded_ips() {
    DEBUG_LOG("Parsing embedded private IPs...");
    size_t size = private_ips_end - private_ips_start;

    if (size == 0) {
        DEBUG_LOG("No embedded IPs found");
        return;
    }

    char* buf = (char*)malloc(size + 1);
    memcpy(buf, private_ips_start, size);
    buf[size] = '\0';

    char* saveptr;
    char* line = strtok_r(buf, "\n", &saveptr);

    while (line) {
        size_t string_length = strlen(line);

        while (string_length > 0 && (line[string_length - 1] == '\n' || line[string_length - 1] == '\r')) {
            line[--string_length] = '\0';
        }

        if (string_length >= 3 && line[0] != '_' && line[string_length - 1] != '_') {
            char* space_pointer = strchr(line, ' ');

            if (space_pointer) {
                *space_pointer = '\0';
                add(line, space_pointer + 1);
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(buf);
    DEBUG_LOG("Finished parsing embedded IPs");
}

} // namespace Bulb

void wifi_init() {
    DEBUG_LOG("Initializing WiFi...");
    Global::wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);

    network_prov_mgr_config_t config = {};
    config.scheme                    = network_prov_scheme_ble;
    config.scheme_event_handler      = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;

    network_prov_mgr_init(config);

    bool provisioned = false;
    network_prov_mgr_is_wifi_provisioned(&provisioned);

    esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    if (!provisioned) {
        DEBUG_LOG("Not provisioned. Starting BLE provisioning...");
        uint8_t eth_mac[6];
        esp_efuse_mac_get_default(eth_mac);
        char service_name[32];
        snprintf(service_name, sizeof(service_name), "%s%02X%02X%02X", Const::PROV_PREFIX, eth_mac[3], eth_mac[4], eth_mac[5]);
        network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, (const void*)"1234", service_name, NULL);
    } else {
        DEBUG_LOG("Already provisioned. Starting WiFi...");
        network_prov_mgr_deinit();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }

    xEventGroupWaitBits(Global::wifi_event_group, Const::WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

void yeelight_control_task(void* pvParameters) {
    DEBUG_LOG("Yeelight control task started");
    Bulb::parse_embedded_ips();
    Bulb::discover_all(2000);

    bool last_state = true;
    while (true) {
        bool current_state = gpio_get_level(Const::BUTTON_1);
        if (last_state == true && current_state == false) {
            DEBUG_LOG("Button pressed! Toggling first bulb.");
            if (Global::Bulb::count > 0) {
                Bulb::dispatch(0, BulbCommand::TOGGLE, "[]");
            } else {
                DEBUG_LOG("No bulbs available to toggle.");
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void) {
    DEBUG_LOG("System booting up...");
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << Const::BUTTON_1);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;

    gpio_config(&io_conf);

    wifi_init();

    for (int i = 0; i < Const::MAX_BULBS; i++) {
        Global::Bulb::sockets[i] = -1;
    }

    xTaskCreate(yeelight_control_task, "yeelight_task", 8192, NULL, 5, NULL);

    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
