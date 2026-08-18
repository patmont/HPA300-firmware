#pragma once

#include "esp_err.h"

typedef struct dns_server *dns_server_handle_t;

// Starts a minimal captive-portal DNS server which resolves every IPv4 name
// to the address of the named ESP-NETIF interface.
dns_server_handle_t dns_server_start(const char *netif_key);
void dns_server_stop(dns_server_handle_t handle);
