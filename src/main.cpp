#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace Const {

constexpr int MAX_BULBS                     = 32;
constexpr int BULB_PORT                     = 55443;
constexpr const char* SSDP_ADDR             = "239.255.255.250";
constexpr int SSDP_PORT                     = 1982;
constexpr const char* SSDP_DISCOVER_MESSAGE = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1982\r\nMAN: \"ssdp:discover\"\r\nST: wifi_bulb\r\n";

} // namespace Const

namespace Global {

namespace Bulb {

char names[Const::MAX_BULBS][64];
char ips[Const::MAX_BULBS][16];
int sockets[Const::MAX_BULBS];
int ids[Const::MAX_BULBS];
int count = 0;

} // namespace Bulb

} // namespace Global

inline void configure_socket_timeout(const int file_descriptor, const int timeout_seconds, const int timeout_microseconds) {
    struct timeval timeout_value = {timeout_seconds, timeout_microseconds};
    setsockopt(file_descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value));
    setsockopt(file_descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value));
}

inline int net_open_tcp(const char* ip_address) {
    const int file_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (file_descriptor < 0) {
        return -1;
    }

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port   = htons(Const::BULB_PORT);
    inet_pton(AF_INET, ip_address, &server_address.sin_addr);

    configure_socket_timeout(file_descriptor, 1, 0);

    if (connect(file_descriptor, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        close(file_descriptor);
        return -1;
    }
    return file_descriptor;
}

namespace Bulb {

inline void ensure_connected(int index) {
    if (Global::Bulb::sockets[index] >= 0) {
        return;
    }
    Global::Bulb::sockets[index] = net_open_tcp(Global::Bulb::ips[index]);
}


inline void send(int index, const char* method, const char* parameters) {
    ensure_connected(index);
    if (Global::Bulb::sockets[index] < 0) {
        return;
    }

    char message_buffer[512];

    const int message_length = snprintf(
        message_buffer,
        sizeof(message_buffer),
        "{\"id\":%d,\"method\":\"%s\",\"params\":%s}\r\n",
        Global::Bulb::ids[index]++,
        method,
        parameters
    );

    ssize_t bytes_sent = ::send(Global::Bulb::sockets[index], message_buffer, message_length, 0);

    if (bytes_sent < 0) {
        printf("Send failed for bulb %s, closing socket.\n", Global::Bulb::names[index]);
        close(Global::Bulb::sockets[index]);
        Global::Bulb::sockets[index] = -1;
        return;
    }

    // Give the bulb's weak CPU a tiny moment to process before the next command
    // 50ms is usually enough to prevent the buffer from choking
    usleep(50000);

    char response_buffer[1024];
    // Using MSG_DONTWAIT so we don't hang if the bulb is silent
    recv(Global::Bulb::sockets[index], response_buffer, sizeof(response_buffer), MSG_DONTWAIT);
}

inline void set_rgba(int index, int red, int green, int blue, int alpha) {
    int rgb_value = (red << 16) | (green << 8) | blue;
    char payload_color[128];
    char payload_brightness[128];

    snprintf(payload_color, sizeof(payload_color), "[%d, \"smooth\", 500]", rgb_value);
    send(index, "set_rgb", payload_color);

    snprintf(payload_brightness, sizeof(payload_brightness), "[%d, \"smooth\", 500]", alpha);
    send(index, "set_bright", payload_brightness);
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

    Global::Bulb::count++;
}

inline void discover_all(int timeout_milliseconds) {
    const int file_descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (file_descriptor < 0) {
        return;
    }

    struct sockaddr_in multicast_address = {0};
    multicast_address.sin_family         = AF_INET;
    multicast_address.sin_addr.s_addr    = inet_addr(Const::SSDP_ADDR);
    multicast_address.sin_port           = htons(Const::SSDP_PORT);

    sendto(file_descriptor, Const::SSDP_DISCOVER_MESSAGE, strlen(Const::SSDP_DISCOVER_MESSAGE), 0, (struct sockaddr*)&multicast_address, sizeof(multicast_address));

    configure_socket_timeout(file_descriptor, timeout_milliseconds / 1000, (timeout_milliseconds % 1000) * 1000);

    char buffer[2048];
    while (recv(file_descriptor, buffer, sizeof(buffer) - 1, 0) > 0) {
        char* location_pointer = strstr(buffer, "Location: yeelight://");

        if (!location_pointer) {
            continue;
        }

        location_pointer += 21;
        char* colon_pointer = strchr(location_pointer, ':');

        if (!colon_pointer) {
            continue;
        }

        *colon_pointer = '\0';
        add("discovered", location_pointer);
    }

    close(file_descriptor);
}

} // namespace Bulb

void load_ips_from_file(const char* file_path) {
    FILE* file_handle = fopen(file_path, "r");
    if (!file_handle) {
        return;
    }

    char line_buffer[256];
    while (fgets(line_buffer, sizeof(line_buffer), file_handle)) {
        size_t string_length = strlen(line_buffer);

        while (string_length > 0 && (line_buffer[string_length - 1] == '\n' || line_buffer[string_length - 1] == '\r')) {
            line_buffer[--string_length] = '\0';
        }

        if (string_length < 3 || line_buffer[0] == '_' || line_buffer[string_length - 1] == '_') {
            continue;
        }

        char* space_pointer = strchr(line_buffer, ' ');
        if (!space_pointer) {
            continue;
        }

        *space_pointer = '\0';
        Bulb::add(line_buffer, space_pointer + 1);
    }

    fclose(file_handle);
}

int main() {
    for (int i = 0; i < Const::MAX_BULBS; i++) {
        Global::Bulb::sockets[i] = -1;
    }

    load_ips_from_file("./PRIVATE.txt");
    Bulb::discover_all(1000);

    for (int i = 0; i < Global::Bulb::count; i++) {
        printf("Setting %s (%s) to Red\n", Global::Bulb::names[i], Global::Bulb::ips[i]);
        Bulb::send(i, "set_power", "[\"on\", \"sudden\", 0]");
        Bulb::set_rgba(i, 219, 155, 66, 70);
    }

    for (int i = 0; i < Global::Bulb::count; i++) {
        if (Global::Bulb::sockets[i] >= 0) {
            close(Global::Bulb::sockets[i]);
        }
    }

    return 0;
}
