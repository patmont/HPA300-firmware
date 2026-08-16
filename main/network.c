#include "network.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "controller.h"
#include "diagnostics.h"
#include "dns_server.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota_update.h"

#define TAG "NETWORK"
#define SETTINGS_NAMESPACE "hpa300"
#define API_TOKEN_KEY "api_token"
#define API_TOKEN_MIN_LENGTH 16
#define API_TOKEN_MAX_LENGTH 64
#define WIFI_RETRY_LIMIT 5
#define REQUEST_BODY_MAX 512
#define OTA_MAINTENANCE_WINDOW_MS (10 * 60 * 1000)
#define OTA_RECEIVE_BUFFER_SIZE 4096
#define OTA_PREFLIGHT_BUFFER_SIZE 512

extern const char provision_html_start[] asm("_binary_provision_html_start");
extern const char provision_html_end[] asm("_binary_provision_html_end");
extern const char update_html_start[] asm("_binary_update_html_start");
extern const char update_html_end[] asm("_binary_update_html_end");

static httpd_handle_t s_http_server;
static dns_server_handle_t s_dns_server;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_wifi_started;
static bool s_http_ready;
static bool s_has_wifi_credentials;
static bool s_connected;
static bool s_provisioning;
static bool s_provisioning_forced;
static bool s_reconnect_pending;
static bool s_maintenance_active;
static bool s_maintenance_keep_ap;
static volatile bool s_ota_request_active;
static TickType_t s_maintenance_started;
static unsigned s_retry_count;
static char s_api_token[API_TOKEN_MAX_LENGTH + 1];
static char s_captive_portal_uri[32];
static char s_last_ota_result[96];

static esp_err_t provisioning_enable(bool forced);
static void maintenance_close(bool preserve_ap);
static void restart_task(void *arg);

static void update_network_led(void)
{
    controller_network_status_t status;
    if (s_provisioning) {
        status = CONTROLLER_NETWORK_PROVISIONING;
    } else if (s_connected && s_http_ready) {
        status = CONTROLLER_NETWORK_CONNECTED;
    } else {
        status = CONTROLLER_NETWORK_CONNECTING;
    }

    esp_err_t err = controller_set_network_status(status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to update network status LED: %s", esp_err_to_name(err));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires reinitialization");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "failed to erase unusable NVS");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t load_api_token(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_api_token[0] = '\0';
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "failed to open settings");

    size_t length = sizeof(s_api_token);
    err = nvs_get_str(nvs, API_TOKEN_KEY, s_api_token, &length);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_api_token[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static esp_err_t save_api_token(const char *token)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
                        "failed to open settings for writing");
    esp_err_t err = nvs_set_str(nvs, API_TOKEN_KEY, token);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        strlcpy(s_api_token, token, sizeof(s_api_token));
    }
    return err;
}

static void make_setup_ssid(char *ssid, size_t size)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, size, "HPA300-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static esp_err_t configure_ap(void)
{
    wifi_config_t config = { 0 };
    char ssid[sizeof(config.ap.ssid)];
    make_setup_ssid(ssid, sizeof(ssid));
    strlcpy((char *)config.ap.ssid, ssid, sizeof(config.ap.ssid));
    config.ap.ssid_len = strlen(ssid);
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_OPEN;
    config.ap.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG,
                        "failed to configure setup access point");
    ESP_LOGW(TAG, "Provisioning access point enabled: %s", ssid);
    return ESP_OK;
}

static void configure_captive_portal_dhcp(void)
{
    if (s_ap_netif == NULL) {
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
        return;
    }
    snprintf(s_captive_portal_uri, sizeof(s_captive_portal_uri),
             "http://" IPSTR, IP2STR(&ip_info.ip));
    esp_netif_dhcps_stop(s_ap_netif);
    esp_err_t err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_CAPTIVEPORTAL_URI,
                                           s_captive_portal_uri,
                                           strlen(s_captive_portal_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP captive-portal hint unavailable: %s", esp_err_to_name(err));
    }
    esp_netif_dhcps_start(s_ap_netif);
}

static esp_err_t provisioning_enable(bool forced)
{
    if (s_provisioning) {
        s_provisioning_forced = s_provisioning_forced || forced;
        update_network_led();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG,
                        "failed to enable AP+STA mode");
    ESP_RETURN_ON_ERROR(configure_ap(), TAG, "failed to configure provisioning AP");
    s_provisioning = true;
    s_provisioning_forced = forced;
    if (s_wifi_started) {
        configure_captive_portal_dhcp();
        s_dns_server = dns_server_start("WIFI_AP_DEF");
        ESP_RETURN_ON_FALSE(s_dns_server != NULL, ESP_ERR_NO_MEM, TAG,
                            "failed to start captive DNS");
    }
    update_network_led();
    return ESP_OK;
}

static void provisioning_disable(void)
{
    if (!s_provisioning || s_api_token[0] == '\0' || s_provisioning_forced) {
        return;
    }
    s_provisioning = false;
    if (s_dns_server != NULL) {
        dns_server_stop(s_dns_server);
        s_dns_server = NULL;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to disable setup AP: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Provisioning access point disabled");
    }
    update_network_led();
}

static void maintenance_close(bool preserve_ap)
{
    if (!s_maintenance_active) {
        return;
    }
    s_maintenance_active = false;
    s_provisioning_forced = false;
    if (!preserve_ap && !s_maintenance_keep_ap) {
        provisioning_disable();
    }
    s_maintenance_keep_ap = false;
    ESP_LOGI(TAG, "firmware maintenance window closed");
}

void network_service(void)
{
    if (!s_maintenance_active || network_ota_is_busy()) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if ((now - s_maintenance_started) >= pdMS_TO_TICKS(OTA_MAINTENANCE_WINDOW_MS)) {
        maintenance_close(false);
    }
}

bool network_ota_is_busy(void)
{
    return s_ota_request_active || ota_update_is_busy();
}

static void reconnect_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(30000));
    s_reconnect_pending = false;
    if (!s_connected && s_has_wifi_credentials) {
        esp_wifi_connect();
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        update_network_led();
        if (s_has_wifi_credentials) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        update_network_led();
        if (!s_has_wifi_credentials) {
            return;
        }
        if (s_retry_count < WIFI_RETRY_LIMIT) {
            s_retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Wi-Fi reconnect attempt %u/%u", s_retry_count, WIFI_RETRY_LIMIT);
        } else if (!s_reconnect_pending) {
            s_retry_count = 0;
            s_reconnect_pending = true;
            ESP_LOGW(TAG, "Wi-Fi unavailable; retrying in 30 seconds (setup requires the physical gesture)");
            if (xTaskCreate(reconnect_task, "wifi_reconnect", 2048, NULL, 4, NULL) != pdPASS) {
                s_reconnect_pending = false;
                ESP_LOGE(TAG, "failed to schedule Wi-Fi reconnect");
            }
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_connected = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected, address " IPSTR, IP2STR(&event->ip_info.ip));
        provisioning_disable();
        update_network_led();
    }
}

static esp_err_t send_json(httpd_req_t *request, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    if (text == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "JSON allocation failed");
    }
    httpd_resp_set_type(request, "application/json");
    esp_err_t err = httpd_resp_sendstr(request, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_json_error(httpd_req_t *request, const char *status,
                                 const char *message)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(json, "error", message);
    httpd_resp_set_status(request, status);
    esp_err_t err = send_json(request, json);
    cJSON_Delete(json);
    return err;
}

static void add_boot_diagnostics(cJSON *json)
{
    const diagnostics_boot_info_t *boot = diagnostics_get_boot_info();
    cJSON *reset = cJSON_AddObjectToObject(json, "last_boot");
    if (reset == NULL) {
        return;
    }
    cJSON_AddStringToObject(reset, "reason", boot->reset_reason_name);
    cJSON_AddNumberToObject(reset, "code", boot->reset_reason);
    cJSON_AddBoolToObject(reset, "power_related", boot->power_related);
    cJSON_AddBoolToObject(reset, "planned", boot->planned);
    if (boot->planned) {
        cJSON_AddStringToObject(reset, "planned_reason", boot->planned_reason);
    }

    cJSON *runtime = cJSON_AddObjectToObject(json, "runtime");
    if (runtime == NULL) {
        return;
    }
    cJSON_AddNumberToObject(runtime, "uptime_seconds",
                            (double)(esp_timer_get_time() / UINT64_C(1000000)));
    cJSON_AddNumberToObject(runtime, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(runtime, "minimum_free_heap_bytes",
                            esp_get_minimum_free_heap_size());
}

static void close_upload_session(httpd_req_t *request)
{
    int socket = httpd_req_to_sockfd(request);
    if (socket >= 0) {
        // Rejected uploads can have a large unread request body. Closing the
        // session prevents those bytes from starving the HTTP task or being
        // parsed as another request on a persistent connection.
        httpd_sess_trigger_close(request->handle, socket);
    }
}

static esp_err_t send_json_error_and_close(httpd_req_t *request, const char *status,
                                           const char *message)
{
    httpd_resp_set_hdr(request, "Connection", "close");
    esp_err_t err = send_json_error(request, status, message);
    close_upload_session(request);
    return err;
}

static bool request_is_authorized(httpd_req_t *request)
{
    if (s_api_token[0] == '\0') {
        return false;
    }
    size_t value_length = httpd_req_get_hdr_value_len(request, "Authorization");
    if (value_length == 0 || value_length >= API_TOKEN_MAX_LENGTH + 8) {
        return false;
    }
    char value[API_TOKEN_MAX_LENGTH + 8];
    if (httpd_req_get_hdr_value_str(request, "Authorization", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    return strncmp(value, "Bearer ", 7) == 0 && strcmp(value + 7, s_api_token) == 0;
}

static esp_err_t require_authorization(httpd_req_t *request)
{
    if (request_is_authorized(request)) {
        return ESP_OK;
    }
    httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer");
    return send_json_error(request, "401 Unauthorized", "missing or invalid bearer token");
}

static bool request_is_from_ap(httpd_req_t *request)
{
    if (s_ap_netif == NULL) {
        return false;
    }
    int socket = httpd_req_to_sockfd(request);
    struct sockaddr_in local = { 0 };
    socklen_t length = sizeof(local);
    if (socket < 0) {
        return false;
    }
    esp_netif_ip_info_t ap_ip;
    if (getsockname(socket, (struct sockaddr *)&local, &length) == 0 &&
        local.sin_family == AF_INET &&
        esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK &&
        local.sin_addr.s_addr == ap_ip.ip.addr) {
        return true;
    }

    // An accepted socket from a server bound to INADDR_ANY can report
    // 0.0.0.0 as its local address. In that case identify the interface by
    // matching the peer against the SoftAP's actual associated/DHCP clients.
    struct sockaddr_in peer = { 0 };
    length = sizeof(peer);
    if (getpeername(socket, (struct sockaddr *)&peer, &length) != 0 ||
        peer.sin_family != AF_INET) {
        return false;
    }
    wifi_sta_list_t wifi_clients = { 0 };
    wifi_sta_mac_ip_list_t ip_clients = { 0 };
    if (esp_wifi_ap_get_sta_list(&wifi_clients) != ESP_OK ||
        esp_wifi_ap_get_sta_list_with_ip(&wifi_clients, &ip_clients) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < ip_clients.num; ++i) {
        if (ip_clients.sta[i].ip.addr == peer.sin_addr.s_addr) {
            return true;
        }
    }

    // Some DHCP clients are absent briefly from the MAC/IP list even though
    // they already have a working AP socket. Accept the SoftAP subnet only
    // when it does not overlap the active STA subnet. If the networks overlap,
    // fail closed and require the bearer token.
    if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) != ESP_OK ||
        (peer.sin_addr.s_addr & ap_ip.netmask.addr) !=
            (ap_ip.ip.addr & ap_ip.netmask.addr)) {
        return false;
    }
    esp_netif_ip_info_t sta_ip = { 0 };
    if (s_sta_netif != NULL && esp_netif_get_ip_info(s_sta_netif, &sta_ip) == ESP_OK &&
        sta_ip.ip.addr != 0 &&
        (peer.sin_addr.s_addr & sta_ip.netmask.addr) ==
            (sta_ip.ip.addr & sta_ip.netmask.addr)) {
        return false;
    }
    return true;
}

static esp_err_t require_ota_access(httpd_req_t *request, bool require_window)
{
    network_service();
    if (require_window && !s_maintenance_active) {
        return send_json_error(request, "403 Forbidden",
                               "use the physical 4,5,4,5 gesture to open maintenance");
    }
    if (request_is_from_ap(request)) {
        return ESP_OK;
    }
    return require_authorization(request);
}

static esp_err_t receive_body(httpd_req_t *request, char *body, size_t capacity)
{
    if (request->content_len <= 0 || (size_t)request->content_len >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t received_total = 0;
    while (received_total < (size_t)request->content_len) {
        int received = httpd_req_recv(request, body + received_total,
                                      request->content_len - received_total);
        if (received <= 0) {
            return ESP_FAIL;
        }
        received_total += (size_t)received;
    }
    body[received_total] = '\0';
    return ESP_OK;
}

static cJSON *make_state_json(void)
{
    controller_snapshot_t state;
    if (controller_get_snapshot(&state) != ESP_OK) {
        return NULL;
    }
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(json, "power", state.fan_speed != FAN_SPEED_OFF);
    cJSON_AddNumberToObject(json, "speed", state.fan_speed);
    cJSON_AddNumberToObject(json, "percentage", state.fan_speed * 25);
    cJSON_AddStringToObject(json, "source", controller_source_name(state.last_change_source));
    cJSON_AddStringToObject(json, "timer", controller_shutoff_mode_name(state.shutoff_mode));
    cJSON_AddNumberToObject(json, "led_brightness", state.led_brightness_percent);
    cJSON_AddBoolToObject(json, "wifi_connected", s_connected);
    return json;
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    if (s_provisioning) {
        httpd_resp_set_type(request, "text/html");
        return httpd_resp_send(request, provision_html_start,
                               provision_html_end - provision_html_start);
    }
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_sendstr(request,
                              "HPA300 local API\nGET /api/v1/device\nGET /api/v1/state\nPUT /api/v1/fan\nGET /update\n");
}

static esp_err_t device_get_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return require_authorization(request);
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[18];
    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", id);
    cJSON_AddStringToObject(json, "name", "HPA300");
    cJSON_AddStringToObject(json, "model", "Honeywell HPA300 custom controller");
    cJSON_AddStringToObject(json, "api_version", "1");
    cJSON_AddStringToObject(json, "firmware_version", app->version);
    cJSON_AddNumberToObject(json, "speed_count", 4);
    add_boot_diagnostics(json);
    esp_err_t err = send_json(request, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t state_get_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return require_authorization(request);
    }
    cJSON *json = make_state_json();
    if (json == NULL) {
        return send_json_error(request, "500 Internal Server Error", "failed to read controller state");
    }
    esp_err_t err = send_json(request, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t fan_put_handler(httpd_req_t *request)
{
    if (!request_is_authorized(request)) {
        return require_authorization(request);
    }
    char body[128];
    if (receive_body(request, body, sizeof(body)) != ESP_OK) {
        return send_json_error(request, "400 Bad Request", "invalid request body");
    }
    cJSON *json = cJSON_Parse(body);
    cJSON *speed_json = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "speed");
    if (!cJSON_IsNumber(speed_json) || speed_json->valuedouble != speed_json->valueint ||
        speed_json->valueint < FAN_SPEED_OFF || speed_json->valueint >= NUM_FAN_SPEEDS) {
        cJSON_Delete(json);
        return send_json_error(request, "422 Unprocessable Entity", "speed must be an integer from 0 through 4");
    }
    fan_speed_t speed = (fan_speed_t)speed_json->valueint;
    cJSON_Delete(json);
    if (controller_set_remote_fan_speed(speed) != ESP_OK) {
        return send_json_error(request, "500 Internal Server Error", "fan transition failed");
    }
    cJSON *state = make_state_json();
    if (state == NULL) {
        return send_json_error(request, "500 Internal Server Error", "failed to read updated state");
    }
    esp_err_t err = send_json(request, state);
    cJSON_Delete(state);
    return err;
}

static esp_err_t update_page_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, update_html_start,
                           update_html_end - update_html_start);
}

static esp_err_t connectivity_check_get_handler(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (strcmp(request->uri, "/generate_204") == 0) {
        httpd_resp_set_status(request, "204 No Content");
        return httpd_resp_send(request, NULL, 0);
    }
    httpd_resp_set_type(request, "text/plain");
    if (strcmp(request->uri, "/ncsi.txt") == 0) {
        return httpd_resp_sendstr(request, "Microsoft NCSI");
    }
    if (strcmp(request->uri, "/hotspot-detect.html") == 0) {
        return httpd_resp_sendstr(request, "Success");
    }
    return httpd_resp_sendstr(request, "Microsoft Connect Test");
}

static esp_err_t ota_status_get_handler(httpd_req_t *request)
{
    esp_err_t access = require_ota_access(request, false);
    if (access != ESP_OK) {
        return access;
    }
    ota_update_info_t info;
    if (ota_update_get_info(&info) != ESP_OK) {
        return send_json_error(request, "500 Internal Server Error",
                               "failed to read OTA partitions");
    }
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(json, "version", info.running_version);
    cJSON_AddStringToObject(json, "slot", info.running_slot);
    const esp_partition_t *inactive = esp_ota_get_next_update_partition(NULL);
    if (inactive != NULL) {
        cJSON_AddNumberToObject(json, "max_image_bytes", inactive->size);
    }
    cJSON_AddBoolToObject(json, "maintenance_active", s_maintenance_active);
    cJSON_AddBoolToObject(json, "busy", network_ota_is_busy());
    cJSON_AddBoolToObject(json, "previous_available", info.previous_available);
    if (info.previous_available) {
        cJSON_AddStringToObject(json, "previous_version", info.previous_version);
        cJSON_AddStringToObject(json, "previous_slot", info.previous_slot);
    }
    if (s_last_ota_result[0] != '\0') {
        cJSON_AddStringToObject(json, "last_result", s_last_ota_result);
    }
    add_boot_diagnostics(json);
    esp_err_t err = send_json(request, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t ota_failure(httpd_req_t *request, esp_err_t cause,
                             const char *message)
{
    ota_update_abort();
    s_ota_request_active = false;
    update_network_led();
    snprintf(s_last_ota_result, sizeof(s_last_ota_result), "failed: %s",
             esp_err_to_name(cause));
    ESP_LOGE(TAG, "%s: %s", message, esp_err_to_name(cause));
    if (cause == ESP_ERR_INVALID_STATE || cause == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        return send_json_error(request, "409 Conflict", message);
    }
    if (cause == ESP_ERR_INVALID_SIZE) {
        return send_json_error(request, "413 Payload Too Large", message);
    }
    if (cause == ESP_ERR_OTA_VALIDATE_FAILED || cause == ESP_ERR_INVALID_ARG) {
        return send_json_error(request, "422 Unprocessable Entity", message);
    }
    return send_json_error(request, "500 Internal Server Error", message);
}

static esp_err_t ota_post_handler(httpd_req_t *request)
{
    esp_err_t access = require_ota_access(request, true);
    if (access != ESP_OK) {
        return access;
    }
    if (network_ota_is_busy()) {
        return send_json_error(request, "409 Conflict", "a firmware update is already active");
    }
    if (request->content_len <= 0) {
        return send_json_error(request, "422 Unprocessable Entity", "firmware body is required");
    }
    const esp_partition_t *inactive = esp_ota_get_next_update_partition(NULL);
    if (inactive == NULL) {
        return send_json_error(request, "500 Internal Server Error",
                               "inactive OTA partition was not found");
    }
    if (request->content_len > inactive->size) {
        snprintf(s_last_ota_result, sizeof(s_last_ota_result),
                 "failed: image too large");
        return send_json_error_and_close(request, "413 Payload Too Large",
                                         "firmware exceeds the inactive OTA partition");
    }
    // Reserve the maintenance window before receiving the preflight bytes. A
    // slow AP upload that began in time must not have its interface disabled.
    s_ota_request_active = true;

    uint8_t *buffer = malloc(OTA_RECEIVE_BUFFER_SIZE);
    if (buffer == NULL) {
        s_ota_request_active = false;
        return send_json_error(request, "500 Internal Server Error", "OTA buffer allocation failed");
    }
    size_t expected = (size_t)request->content_len;
    size_t preflight_length = expected < OTA_PREFLIGHT_BUFFER_SIZE
                                  ? expected
                                  : OTA_PREFLIGHT_BUFFER_SIZE;
    size_t received = 0;
    while (received < preflight_length) {
        int count = httpd_req_recv(request, (char *)buffer + received,
                                   preflight_length - received);
        if (count <= 0) {
            free(buffer);
            esp_err_t response =
                ota_failure(request, ESP_FAIL, "firmware upload was interrupted");
            close_upload_session(request);
            return response;
        }
        received += (size_t)count;
    }

    if (controller_set_remote_fan_speed(FAN_SPEED_OFF) != ESP_OK) {
        free(buffer);
        return ota_failure(request, ESP_FAIL, "failed to stop the fan safely");
    }
    controller_set_network_status(CONTROLLER_NETWORK_UPDATING);

    char new_version[sizeof(((esp_app_desc_t *)0)->version)];
    esp_err_t err = ota_update_begin(buffer, received, expected,
                                     new_version, sizeof(new_version));
    if (err != ESP_OK) {
        free(buffer);
        esp_err_t response = ota_failure(request, err, "firmware header is invalid");
        close_upload_session(request);
        return response;
    }
    err = ota_update_write(buffer, received);
    size_t total = received;
    while (err == ESP_OK && total < expected) {
        size_t wanted = expected - total;
        if (wanted > OTA_RECEIVE_BUFFER_SIZE) {
            wanted = OTA_RECEIVE_BUFFER_SIZE;
        }
        int count = httpd_req_recv(request, (char *)buffer, wanted);
        if (count <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = ota_update_write(buffer, (size_t)count);
        total += (size_t)count;
    }
    free(buffer);
    if (err != ESP_OK) {
        bool incomplete = total < expected;
        esp_err_t response = ota_failure(request, err, incomplete
                                                          ? "firmware upload was interrupted"
                                                          : "firmware write failed");
        if (incomplete) {
            close_upload_session(request);
        }
        return response;
    }

    err = ota_update_finish();
    if (err != ESP_OK) {
        return ota_failure(request, err, "firmware image or signature is invalid");
    }

    const esp_app_desc_t *running = esp_app_get_description();
    snprintf(s_last_ota_result, sizeof(s_last_ota_result), "installed %s", new_version);
    maintenance_close(true);
    bool restarting =
        xTaskCreate(restart_task, "ota_restart", 2048,
                    (void *)(intptr_t)DIAGNOSTICS_RESTART_OTA, 5, NULL) == pdPASS;
    if (!restarting) {
        s_ota_request_active = false;
        update_network_led();
        strlcpy(s_last_ota_result, "installed; manual restart required",
                sizeof(s_last_ota_result));
        ESP_LOGE(TAG, "firmware installed but restart task could not be created");
    }
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(json, "accepted", true);
    cJSON_AddStringToObject(json, "from_version", running->version);
    cJSON_AddStringToObject(json, "to_version", new_version);
    cJSON_AddBoolToObject(json, "restarting", restarting);
    esp_err_t response_err = send_json(request, json);
    cJSON_Delete(json);
    return response_err;
}

static esp_err_t ota_rollback_post_handler(httpd_req_t *request)
{
    esp_err_t access = require_ota_access(request, true);
    if (access != ESP_OK) {
        return access;
    }
    if (network_ota_is_busy()) {
        return send_json_error(request, "409 Conflict", "a firmware update is active");
    }
    if (controller_set_remote_fan_speed(FAN_SPEED_OFF) != ESP_OK) {
        return ota_failure(request, ESP_FAIL, "failed to stop the fan safely");
    }
    s_ota_request_active = true;
    controller_set_network_status(CONTROLLER_NETWORK_UPDATING);
    esp_err_t err = ota_update_select_previous();
    if (err == ESP_ERR_NOT_FOUND) {
        s_ota_request_active = false;
        update_network_led();
        return send_json_error(request, "409 Conflict", "no previous firmware is available");
    }
    if (err != ESP_OK) {
        return ota_failure(request, err, "previous firmware failed validation");
    }

    maintenance_close(true);
    bool restarting =
        xTaskCreate(restart_task, "rollback_restart", 2048,
                    (void *)(intptr_t)DIAGNOSTICS_RESTART_ROLLBACK, 5, NULL) == pdPASS;
    if (!restarting) {
        s_ota_request_active = false;
        update_network_led();
        ESP_LOGE(TAG, "previous firmware selected but restart task could not be created");
    }
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(json, "accepted", true);
    cJSON_AddBoolToObject(json, "restarting", restarting);
    esp_err_t response_err = send_json(request, json);
    cJSON_Delete(json);
    return response_err;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    diagnostics_mark_planned_restart((diagnostics_restart_t)(intptr_t)arg);
    esp_restart();
}

static esp_err_t provision_post_handler(httpd_req_t *request)
{
    if (!s_provisioning) {
        return send_json_error(request, "403 Forbidden", "provisioning is not active");
    }
    char body[REQUEST_BODY_MAX];
    if (receive_body(request, body, sizeof(body)) != ESP_OK) {
        return send_json_error(request, "400 Bad Request", "invalid request body");
    }
    cJSON *json = cJSON_Parse(body);
    cJSON *ssid_json = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *password_json = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "password");
    cJSON *token_json = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "api_token");
    if (!cJSON_IsString(ssid_json) || !cJSON_IsString(password_json) || !cJSON_IsString(token_json)) {
        cJSON_Delete(json);
        return send_json_error(request, "422 Unprocessable Entity", "ssid, password, and api_token are required strings");
    }

    size_t ssid_length = strlen(ssid_json->valuestring);
    size_t password_length = strlen(password_json->valuestring);
    size_t token_length = strlen(token_json->valuestring);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 63 ||
        (password_length > 0 && password_length < 8) ||
        token_length < API_TOKEN_MIN_LENGTH || token_length > API_TOKEN_MAX_LENGTH) {
        cJSON_Delete(json);
        return send_json_error(request, "422 Unprocessable Entity",
                               "invalid SSID, Wi-Fi password, or API token length");
    }

    wifi_config_t config = { 0 };
    // An IEEE 802.11 SSID may occupy all 32 bytes and therefore need not be
    // NUL-terminated in wifi_config_t.
    memcpy(config.sta.ssid, ssid_json->valuestring, ssid_length);
    strlcpy((char *)config.sta.password, password_json->valuestring, sizeof(config.sta.password));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.threshold.authmode = password_length == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = save_api_token(token_json->valuestring);
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    cJSON_Delete(json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to store provisioning data: %s", esp_err_to_name(err));
        return send_json_error(request, "500 Internal Server Error", "failed to save configuration");
    }

    s_has_wifi_credentials = true;
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "saved", true);
    cJSON_AddBoolToObject(response, "restarting", true);
    err = send_json(request, response);
    cJSON_Delete(response);
    if (err == ESP_OK &&
        xTaskCreate(restart_task, "restart", 2048,
                    (void *)(intptr_t)DIAGNOSTICS_RESTART_PROVISIONING, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "configuration saved but restart task could not be created");
    }
    return err;
}

static esp_err_t not_found_handler(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    if (s_provisioning && request->method == HTTP_GET) {
        httpd_resp_set_status(request, "302 Temporary Redirect");
        httpd_resp_set_hdr(request, "Location", "/");
        return httpd_resp_sendstr(request, "Redirecting to HPA300 setup");
    }
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 16;
    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "failed to start HTTP server");

    const httpd_uri_t handlers[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/api/v1/device", .method = HTTP_GET, .handler = device_get_handler },
        { .uri = "/api/v1/state", .method = HTTP_GET, .handler = state_get_handler },
        { .uri = "/api/v1/fan", .method = HTTP_PUT, .handler = fan_put_handler },
        { .uri = "/api/v1/provision", .method = HTTP_POST, .handler = provision_post_handler },
        { .uri = "/update", .method = HTTP_GET, .handler = update_page_get_handler },
        { .uri = "/api/v1/ota", .method = HTTP_GET, .handler = ota_status_get_handler },
        { .uri = "/api/v1/ota", .method = HTTP_POST, .handler = ota_post_handler },
        { .uri = "/api/v1/ota/rollback", .method = HTTP_POST, .handler = ota_rollback_post_handler },
        { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = connectivity_check_get_handler },
        { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = connectivity_check_get_handler },
        { .uri = "/generate_204", .method = HTTP_GET, .handler = connectivity_check_get_handler },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = connectivity_check_get_handler },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &handlers[i]), TAG,
                            "failed to register HTTP handler");
    }
    ESP_RETURN_ON_ERROR(httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND,
                                                    not_found_handler), TAG,
                        "failed to register captive redirect");
    s_http_ready = true;
    update_network_led();
    ESP_LOGI(TAG, "HTTP API started on port 80");
    return ESP_OK;
}

esp_err_t network_init(void)
{
    update_network_led();
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "failed to initialize NVS");
    ESP_RETURN_ON_ERROR(load_api_token(), TAG, "failed to load API token");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "failed to initialize TCP/IP stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "failed to create event loop");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_sta_netif != NULL && s_ap_netif != NULL, ESP_ERR_NO_MEM, TAG,
                        "failed to create Wi-Fi interfaces");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "failed to initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "failed to enable Wi-Fi persistence");

    wifi_config_t stored_config = { 0 };
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &stored_config);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_RETURN_ON_ERROR(err, TAG, "failed to read stored Wi-Fi configuration");
    }
    s_has_wifi_credentials = stored_config.sta.ssid[0] != '\0';

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL), TAG,
                        "failed to register Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL), TAG,
                        "failed to register IP events");

    bool needs_provisioning = !s_has_wifi_credentials || s_api_token[0] == '\0';
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(needs_provisioning ? WIFI_MODE_APSTA : WIFI_MODE_STA),
                        TAG, "failed to set Wi-Fi mode");
    if (needs_provisioning) {
        ESP_RETURN_ON_ERROR(configure_ap(), TAG, "failed to configure setup AP");
        s_provisioning = true;
        s_provisioning_forced = false;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed to start Wi-Fi");
    s_wifi_started = true;
    if (s_provisioning) {
        configure_captive_portal_dhcp();
        s_dns_server = dns_server_start("WIFI_AP_DEF");
        ESP_RETURN_ON_FALSE(s_dns_server != NULL, ESP_ERR_NO_MEM, TAG,
                            "failed to start captive DNS");
        update_network_led();
    }
    return start_http_server();
}

esp_err_t network_start_provisioning(void)
{
    ESP_RETURN_ON_FALSE(s_wifi_started, ESP_ERR_INVALID_STATE, TAG, "network is not initialized");
    if (!s_maintenance_active) {
        s_maintenance_keep_ap = !s_has_wifi_credentials || s_api_token[0] == '\0' ||
                                (s_provisioning && !s_provisioning_forced);
    }
    ESP_RETURN_ON_ERROR(provisioning_enable(true), TAG,
                        "failed to enable maintenance access point");
    s_maintenance_active = true;
    s_maintenance_started = xTaskGetTickCount();
    ESP_LOGW(TAG, "physical setup gesture accepted; firmware maintenance open for ten minutes");
    return ESP_OK;
}
