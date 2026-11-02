#include "wifi_portal.h"
#include "config_pins.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "WIFI_PORTAL";
static httpd_handle_t s_http_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_is_running = false;

static const char *HTML_HEADER =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>VAM Telemetry Portal</title>"
"<style>"
"*{box-sizing:border-box}body{background:#0b0f19;color:#e2e8f0;font-family:system-ui,-apple-system,sans-serif;margin:0;padding:16px}"
".container{max-width:680px;margin:0 auto;background:#151d2f;border:1px solid #28354f;border-radius:12px;padding:20px;box-shadow:0 8px 30px rgba(0,0,0,0.5)}"
"h1{color:#38bdf8;font-size:22px;margin:0 0 6px 0;display:flex;align-items:center;gap:8px}"
".sub{color:#94a3b8;font-size:13px;margin-bottom:16px}"
".live-bar{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:18px}"
".box{background:#1f2b43;padding:12px;border-radius:8px;text-align:center;border:1px solid #334466}"
".box-val{font-size:18px;font-weight:bold;color:#4ade80;margin-top:4px}"
".box-lbl{font-size:11px;color:#94a3b8;text-transform:uppercase}"
"table{width:100%;border-collapse:collapse;margin:16px 0;background:#0d1424;border-radius:8px;overflow:hidden}"
"th,td{padding:10px 12px;border-bottom:1px solid #23314d;font-size:13px;text-align:left}"
"th{background:#18233a;color:#94a3b8;font-weight:600}"
".btn{background:#0284c7;color:#fff;text-decoration:none;padding:6px 12px;border-radius:6px;font-size:12px;display:inline-block;margin-right:4px}"
".btn-del{background:#dc2626}"
".btn-ref{background:#475569;margin-top:12px}"
"</style></head><body>"
"<div class='container'>"
"<h1>⚡ VAM Telemetry Logger</h1>"
"<div class='sub'>Adalogger FeatherWing MicroSD Logs & Telemetry</div>";

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, HTML_HEADER, strlen(HTML_HEADER));

    // Dynamic Live Status Block
    char live_html[512];
    snprintf(live_html, sizeof(live_html),
             "<div class='live-bar'>"
             "<div class='box'><div class='box-lbl'>Total Current</div><div class='box-val'>%+.2f A</div></div>"
             "<div class='box'><div class='box-lbl'>Power / Batt</div><div class='box-val'>%.1f W (%.1fV)</div></div>"
             "<div class='box'><div class='box-lbl'>Log Status</div><div class='box-val' style='color:%s'>%s</div></div>"
             "</div>"
             "<table><tr><th>Filename</th><th>Size</th><th>Actions</th></tr>",
             g_sys_state.latest_telemetry.total_system_current_a,
             g_sys_state.latest_telemetry.total_power_w,
             g_sys_state.latest_telemetry.battery_v,
             g_sys_state.is_logging ? "#f87171" : "#4ade80",
             g_sys_state.is_logging ? "RECORDING" : "STANDBY");
    httpd_resp_send_chunk(req, live_html, strlen(live_html));

    // File list from /sdcard/logs
    DIR *d = opendir("/sdcard/logs");
    int count = 0;
    if (d) {
        struct dirent *dir;
        char row[512];
        struct stat st;
        char fpath[300];

        while ((dir = readdir(d)) != NULL) {
            if (strstr(dir->d_name, ".csv") != NULL) {
                snprintf(fpath, sizeof(fpath), "/sdcard/logs/%s", dir->d_name);
                size_t sz = 0;
                if (stat(fpath, &st) == 0) {
                    sz = st.st_size;
                }
                snprintf(row, sizeof(row),
                         "<tr><td><b>%.64s</b></td><td>%.1f KB</td><td>"
                         "<a class='btn' href='/download?file=%.64s'>⬇ Download</a>"
                         "<a class='btn btn-del' href='/delete?file=%.64s' onclick=\"return confirm('Delete this CSV log?')\">🗑</a>"
                         "</td></tr>",
                         dir->d_name, (float)sz / 1024.0f, dir->d_name, dir->d_name);
                httpd_resp_send_chunk(req, row, strlen(row));
                count++;
            }
        }
        closedir(d);
    }

    if (count == 0) {
        const char *empty_row = "<tr><td colspan='3' style='text-align:center;color:#64748b;padding:24px'>No CSV files found on SD card.</td></tr>";
        httpd_resp_send_chunk(req, empty_row, strlen(empty_row));
    }

    const char *footer = "</table><div style='text-align:center'><a href='/' class='btn btn-ref'>🔄 Refresh List</a></div></div></body></html>";
    httpd_resp_send_chunk(req, footer, strlen(footer));
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req) {
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char filename[64];
        if (httpd_query_key_value(buf, "file", filename, sizeof(filename)) == ESP_OK) {
            char filepath[300];
            snprintf(filepath, sizeof(filepath), "/sdcard/logs/%s", filename);

            if (g_spi_bus_mutex) xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
            FILE *f = fopen(filepath, "r");
            if (g_spi_bus_mutex) xSemaphoreGive(g_spi_bus_mutex);

            if (f) {
                httpd_resp_set_type(req, "text/csv");
                char disp[128];
                snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
                httpd_resp_set_hdr(req, "Content-Disposition", disp);

                char chunk[1024];
                size_t read_bytes;
                while (1) {
                    if (g_spi_bus_mutex) xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
                    read_bytes = fread(chunk, 1, sizeof(chunk), f);
                    if (g_spi_bus_mutex) xSemaphoreGive(g_spi_bus_mutex);

                    if (read_bytes == 0) break;
                    httpd_resp_send_chunk(req, chunk, read_bytes);
                }

                if (g_spi_bus_mutex) xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
                fclose(f);
                if (g_spi_bus_mutex) xSemaphoreGive(g_spi_bus_mutex);

                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_OK;
            }
        }
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t delete_get_handler(httpd_req_t *req) {
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char filename[64];
        if (httpd_query_key_value(buf, "file", filename, sizeof(filename)) == ESP_OK) {
            char filepath[300];
            snprintf(filepath, sizeof(filepath), "/sdcard/logs/%s", filename);

            if (g_spi_bus_mutex) xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
            unlink(filepath);
            if (g_spi_bus_mutex) xSemaphoreGive(g_spi_bus_mutex);

            ESP_LOGI(TAG, "Deleted log file via Wi-Fi: %s", filepath);
        }
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t wifi_portal_start(void) {
    if (s_is_running) return ESP_OK;

    ESP_LOGI(TAG, "Starting Wi-Fi Access Point (SSID: ClampMeter-WiFi)...");
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ClampMeter-WiFi",
            .ssid_len = strlen("ClampMeter-WiFi"),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(s_http_server, &root_uri);

        httpd_uri_t download_uri = { .uri = "/download", .method = HTTP_GET, .handler = download_get_handler };
        httpd_register_uri_handler(s_http_server, &download_uri);

        httpd_uri_t del_uri = { .uri = "/delete", .method = HTTP_GET, .handler = delete_get_handler };
        httpd_register_uri_handler(s_http_server, &del_uri);

        ESP_LOGI(TAG, "Wi-Fi Web Portal active: http://192.168.4.1");
    }

    s_is_running = true;
    g_sys_state.wifi_portal_active = true;
    return ESP_OK;
}

esp_err_t wifi_portal_stop(void) {
    if (!s_is_running) return ESP_OK;

    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    s_is_running = false;
    g_sys_state.wifi_portal_active = false;
    ESP_LOGI(TAG, "Wi-Fi Web Portal stopped.");
    return ESP_OK;
}

bool wifi_portal_is_running(void) {
    return s_is_running;
}

