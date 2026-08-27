#ifndef NAREDEF_PT_UDP
#define NAREDEF_PT_UDP

/**
 * Native PaperTracker streaming.
 *
 * Speaks the same UDP wire format as the PaperTracker Android bridge
 * (com.bridge.papertrackermodern): a 16-byte big-endian header with magic
 * "PT", followed by up to 1184 bytes of a fragmented JPEG frame, sent to the
 * PaperTracker desktop app on UDP 45454.
 *
 *   off size  field
 *    0    2   magic "PT"
 *    2    1   protocol version (1)
 *    3    1   message type (1 = JPEG frame)
 *    4    1   slot: face=0, left=1, right=2
 *    5    1   reserved (0)
 *    6    2   sequence id, wraps at 65535
 *    8    2   fragment count  = ceil(len / 1184)
 *   10    2   fragment index
 *   12    2   fragment length
 *   14    2   reserved (0)
 *   16   ..   JPEG fragment
 *
 * The desktop app announces itself with a JSON `lan_announce` UDP broadcast on
 * the same port; we latch the sender address as the stream target. A static
 * target can also be pinned in NVS for setups where the announce never
 * arrives.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "globals.h"

#define PT_PORT              45454
#define PT_MAGIC0            'P'
#define PT_MAGIC1            'T'
#define PT_VERSION           1
#define PT_TYPE_FRAME        1
#define PT_PACKET_MAX        1200
#define PT_HEADER_LEN        16
#define PT_CHUNK_MAX         (PT_PACKET_MAX - PT_HEADER_LEN)  // 1184
#define PT_ANNOUNCE_TIMEOUT_MS 15000
#define PT_CYCLE_TIME_MS     45   // one pass over all three cameras

// Camera index (0:left, 1:right, 2:face) -> PaperTracker slot id.
static const uint8_t PT_SLOT_FOR_INDEX[3] = { 1, 2, 0 };

static const char* PT_TAG = "PTUDP";

/** Fetch one validated JPEG for a camera index. Implemented by spimaster.cpp. */
typedef bool (*pt_frame_fetch_t)(uint8_t index, const uint8_t** out, uint16_t* len);

static pt_frame_fetch_t pt_fetch = nullptr;

static volatile bool     pt_enabled       = true;
static volatile bool     pt_streaming     = false;
static volatile bool     pt_cycle_busy    = false;  // true while the SPI buffers are ours
static uint32_t          pt_target_ip     = 0;      // network byte order
static bool              pt_target_pinned = false;  // from NVS, never expires
static int64_t           pt_last_announce = 0;      // esp_timer us
static uint16_t          pt_sequence      = 0;
static uint32_t          pt_frames_sent   = 0;
static uint32_t          pt_packets_sent  = 0;
static uint32_t          pt_send_errors   = 0;
static uint8_t           pt_packet[PT_PACKET_MAX];

static inline bool pt_target_valid() {
    if (pt_target_ip == 0) return false;
    if (pt_target_pinned) return true;
    return (esp_timer_get_time() - pt_last_announce) < (int64_t)PT_ANNOUNCE_TIMEOUT_MS * 1000;
}

static inline bool pt_is_streaming() { return pt_streaming; }

/**
 * Block until the stream task is outside its SPI cycle. Callers must already
 * have set their `flags` bit so no new cycle starts. Returns false on timeout.
 */
static bool pt_wait_idle(uint32_t timeout_ms) {
    TickType_t start = xTaskGetTickCount();
    while (pt_cycle_busy) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

static void pt_load_config() {
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t enable = 1;
    if (nvs_get_u8(handle, "pt_enable", &enable) == ESP_OK) {
        pt_enabled = (enable != 0);
    }
    char host[16] = {0};
    size_t len = sizeof(host);
    if (nvs_get_str(handle, "pt_host", host, &len) == ESP_OK && host[0]) {
        uint32_t addr = inet_addr(host);
        if (addr != INADDR_NONE) {
            pt_target_ip = addr;
            pt_target_pinned = true;
            ESP_LOGI(PT_TAG, "Pinned target from NVS: %s", host);
        }
    }
    nvs_close(handle);
}

static esp_err_t pt_save_config(const char* host, const int8_t enable) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (enable >= 0) {
        err = nvs_set_u8(handle, "pt_enable", enable ? 1 : 0);
    }
    if (err == ESP_OK && host) {
        err = nvs_set_str(handle, "pt_host", host);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/** Pin (or clear, with an empty string) the PaperTracker host. */
static bool pt_set_target(const char* host) {
    if (!host || !host[0]) {
        pt_target_pinned = false;
        pt_target_ip = 0;
        pt_save_config("", -1);
        ESP_LOGI(PT_TAG, "Target cleared, falling back to discovery");
        return true;
    }
    uint32_t addr = inet_addr(host);
    if (addr == INADDR_NONE) return false;
    pt_target_ip = addr;
    pt_target_pinned = true;
    pt_save_config(host, -1);
    ESP_LOGI(PT_TAG, "Target pinned: %s", host);
    return true;
}

static void pt_set_enabled(bool enable) {
    pt_enabled = enable;
    pt_save_config(nullptr, enable ? 1 : 0);
    ESP_LOGI(PT_TAG, "Streaming %s", enable ? "enabled" : "disabled");
}

/** Format the current target as dotted quad, or "-" when unknown. */
static void pt_target_str(char* buf, size_t len) {
    if (pt_target_ip == 0) {
        snprintf(buf, len, "-");
        return;
    }
    struct in_addr addr;
    addr.s_addr = pt_target_ip;
    snprintf(buf, len, "%s", inet_ntoa(addr));
}

/** Split one JPEG into PT v1 fragments and push them out. */
static bool pt_send_frame(int sock, uint8_t slot, const uint8_t* jpeg, uint16_t len) {
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(PT_PORT);
    dest.sin_addr.s_addr = pt_target_ip;

    uint16_t total = (len + PT_CHUNK_MAX - 1) / PT_CHUNK_MAX;
    if (total == 0) total = 1;
    uint16_t sequence = pt_sequence++;

    for (uint16_t index = 0; index < total; index++) {
        uint16_t offset = index * PT_CHUNK_MAX;
        uint16_t chunk = len - offset;
        if (chunk > PT_CHUNK_MAX) chunk = PT_CHUNK_MAX;

        pt_packet[0]  = PT_MAGIC0;
        pt_packet[1]  = PT_MAGIC1;
        pt_packet[2]  = PT_VERSION;
        pt_packet[3]  = PT_TYPE_FRAME;
        pt_packet[4]  = slot;
        pt_packet[5]  = 0;
        pt_packet[6]  = sequence >> 8;   pt_packet[7]  = sequence & 0xFF;
        pt_packet[8]  = total >> 8;      pt_packet[9]  = total & 0xFF;
        pt_packet[10] = index >> 8;      pt_packet[11] = index & 0xFF;
        pt_packet[12] = chunk >> 8;      pt_packet[13] = chunk & 0xFF;
        pt_packet[14] = 0;               pt_packet[15] = 0;
        memcpy(pt_packet + PT_HEADER_LEN, jpeg + offset, chunk);

        int sent = sendto(sock, pt_packet, PT_HEADER_LEN + chunk, 0,
                          (struct sockaddr*)&dest, sizeof(dest));
        if (sent < 0) {
            // Transmit queue full: give lwip a tick and retry this fragment once.
            vTaskDelay(pdMS_TO_TICKS(2));
            sent = sendto(sock, pt_packet, PT_HEADER_LEN + chunk, 0,
                          (struct sockaddr*)&dest, sizeof(dest));
        }
        if (sent < 0) {
            pt_send_errors++;
            return false;
        }
        pt_packets_sent++;
    }
    pt_frames_sent++;
    return true;
}

/** Listen for the desktop app's `lan_announce` broadcast and latch its address. */
static void pt_discovery_task(void* arg) {
    char buf[1024];
    while (1) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PT_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(PT_TAG, "discovery bind failed: %d", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGI(PT_TAG, "Listening for lan_announce on :%d", PT_PORT);

        while (1) {
            struct sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            int len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                               (struct sockaddr*)&from, &from_len);
            if (len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                ESP_LOGW(PT_TAG, "recvfrom failed: %d", errno);
                break;
            }
            buf[len] = '\0';
            if (!strstr(buf, "lan_announce")) continue;

            pt_last_announce = esp_timer_get_time();
            if (!pt_target_pinned && pt_target_ip != from.sin_addr.s_addr) {
                pt_target_ip = from.sin_addr.s_addr;
                ESP_LOGI(PT_TAG, "Discovered PaperTracker at %s", inet_ntoa(from.sin_addr));
            }
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/** Read each camera in turn and push its frame to the desktop app. */
static void pt_stream_task(void* arg) {
    int sock = -1;
    int64_t last_cycle = 0;

    while (1) {
        // Browser streams own the SPI buffers while they run; stand down.
        if (!pt_enabled || flags != 0 || !(globalStatus & GLOBALSTAT_CONNECTED) ||
            !pt_target_valid()) {
            if (pt_streaming) {
                pt_streaming = false;
                // A browser stream may be the reason we stood down; in that
                // case it still owns the streaming bit and the sleep timer.
                if (flags == 0) {
                    globalStatus &= ~GLOBALSTAT_STREAMING;
                    idle_timer_arm_5min();
                }
                ESP_LOGI(PT_TAG, "Streaming stopped");
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        if (!pt_streaming) {
            char target[16];
            pt_target_str(target, sizeof(target));
            ESP_LOGI(PT_TAG, "Streaming to %s:%d", target, PT_PORT);
            pt_streaming = true;
            globalStatus |= GLOBALSTAT_STREAMING;
            gpio_set_level(PIN_POW, 1);
            idle_timer_cancel();
        }

        pt_cycle_busy = true;
        for (uint8_t index = 0; index < 3; index++) {
            if (flags != 0) break;  // a browser stream just claimed the buffers
            const uint8_t* data = nullptr;
            uint16_t len = 0;
            if (!pt_fetch || !pt_fetch(index, &data, &len)) continue;
            pt_send_frame(sock, PT_SLOT_FOR_INDEX[index], data, len);
        }
        pt_cycle_busy = false;

        int64_t now = esp_timer_get_time() / 1000;
        int64_t elapsed = now - last_cycle;
        last_cycle = now;
        if (elapsed < PT_CYCLE_TIME_MS) {
            vTaskDelay(pdMS_TO_TICKS(PT_CYCLE_TIME_MS - elapsed));
        }
    }
}

static void pt_udp_start(pt_frame_fetch_t fetch) {
    pt_fetch = fetch;
    pt_load_config();
    xTaskCreate(pt_discovery_task, "pt_discovery", 4 * 1024, NULL, 5, NULL);
    xTaskCreate(pt_stream_task, "pt_stream", 4 * 1024, NULL, 5, NULL);
    ESP_LOGI(PT_TAG, "PaperTracker streaming %s", pt_enabled ? "armed" : "disabled");
}

#endif // NAREDEF_PT_UDP
