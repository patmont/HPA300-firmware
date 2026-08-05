#include "dns_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define TAG "CAPTIVE_DNS"
#define DNS_PORT 53
#define DNS_PACKET_MAX 256

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t authority_count;
    uint16_t additional_count;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t name_pointer;
    uint16_t type;
    uint16_t class_code;
    uint32_t ttl;
    uint16_t address_length;
    uint32_t address;
} dns_answer_t;

struct dns_server {
    volatile bool running;
    volatile TaskHandle_t task;
    int socket_fd;
    const char *netif_key;
};

static size_t question_end_offset(const uint8_t *packet, size_t length)
{
    size_t offset = sizeof(dns_header_t);
    while (offset < length && packet[offset] != 0) {
        uint8_t label_length = packet[offset];
        if ((label_length & 0xc0) != 0 || offset + 1U + label_length >= length) {
            return 0;
        }
        offset += 1U + label_length;
    }
    // Zero terminator plus QTYPE and QCLASS.
    if (offset + 5U > length) {
        return 0;
    }
    return offset + 5U;
}

static int build_response(uint8_t *packet, size_t request_length, size_t capacity,
                          uint32_t address)
{
    if (request_length < sizeof(dns_header_t) ||
        ntohs(((dns_header_t *)packet)->question_count) != 1) {
        return 0;
    }

    size_t question_end = question_end_offset(packet, request_length);
    if (question_end == 0 || question_end + sizeof(dns_answer_t) > capacity) {
        return 0;
    }

    // Only answer an IN/A question. QTYPE and QCLASS immediately precede the
    // calculated end of the question.
    uint16_t query_type;
    uint16_t query_class;
    memcpy(&query_type, packet + question_end - 4, sizeof(query_type));
    memcpy(&query_class, packet + question_end - 2, sizeof(query_class));
    if (ntohs(query_type) != 1 || ntohs(query_class) != 1) {
        return 0;
    }

    dns_header_t *header = (dns_header_t *)packet;
    header->flags = htons(0x8180); // standard response, recursion unavailable, no error
    header->answer_count = htons(1);
    header->authority_count = 0;
    header->additional_count = 0;

    dns_answer_t answer = {
        .name_pointer = htons(0xc00c),
        .type = htons(1),
        .class_code = htons(1),
        .ttl = htonl(60),
        .address_length = htons(4),
        .address = address,
    };
    memcpy(packet + question_end, &answer, sizeof(answer));
    return (int)(question_end + sizeof(answer));
}

static void dns_task(void *arg)
{
    dns_server_handle_t server = arg;
    struct sockaddr_in bind_address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    server->socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (server->socket_fd < 0 ||
        bind(server->socket_fd, (struct sockaddr *)&bind_address, sizeof(bind_address)) < 0) {
        ESP_LOGE(TAG, "failed to bind DNS socket: errno=%d", errno);
        if (server->socket_fd >= 0) {
            close(server->socket_fd);
        }
        server->socket_fd = -1;
        server->running = false;
        server->task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "captive DNS server started");
    while (server->running) {
        uint8_t packet[DNS_PACKET_MAX];
        struct sockaddr_storage source;
        socklen_t source_length = sizeof(source);
        int received = recvfrom(server->socket_fd, packet, sizeof(packet), 0,
                                (struct sockaddr *)&source, &source_length);
        if (received <= 0) {
            continue;
        }

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(server->netif_key);
        esp_netif_ip_info_t ip_info;
        if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
            continue;
        }

        int response_length = build_response(packet, (size_t)received, sizeof(packet),
                                             ip_info.ip.addr);
        if (response_length > 0) {
            sendto(server->socket_fd, packet, response_length, 0,
                   (struct sockaddr *)&source, source_length);
        }
    }

    if (server->socket_fd >= 0) {
        close(server->socket_fd);
        server->socket_fd = -1;
    }
    server->task = NULL;
    vTaskDelete(NULL);
}

dns_server_handle_t dns_server_start(const char *netif_key)
{
    if (netif_key == NULL) {
        return NULL;
    }

    dns_server_handle_t server = calloc(1, sizeof(*server));
    if (server == NULL) {
        return NULL;
    }
    server->running = true;
    server->socket_fd = -1;
    server->netif_key = netif_key;
    if (xTaskCreate(dns_task, "captive_dns", 3072, server, 5,
                    (TaskHandle_t *)&server->task) != pdPASS) {
        free(server);
        return NULL;
    }
    return server;
}

void dns_server_stop(dns_server_handle_t server)
{
    if (server == NULL) {
        return;
    }
    server->running = false;
    if (server->socket_fd >= 0) {
        shutdown(server->socket_fd, SHUT_RDWR);
    }
    for (int i = 0; i < 20 && server->task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (server->task != NULL) {
        ESP_LOGW(TAG, "DNS task did not stop promptly; leaving its handle allocated");
        return;
    }
    free(server);
}
