/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * SAP (Session Announcement Protocol) - RFC 2974
 *
 * Handles periodic multicast announcement and discovery of AES67
 * audio sessions via SDP payloads over the SAP multicast address.
 */

#include "aes67_sap.h"
#include "aes67_sdp.h"
#include "aes67_net.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aes67_sap";

/* SAP protocol constants */
#define SAP_HEADER_SIZE      8
#define SAP_CONTENT_TYPE     "application/sdp"
#define SAP_CONTENT_TYPE_LEN 16  /* includes null terminator */
#define SAP_ANNOUNCE_FLAG    0x20
#define SAP_DELETE_FLAG      0x24
#define SAP_TX_INTERVAL_MS   2000    /* Announce every 2s for fast discovery */
#define SAP_TIMEOUT_MS       120000  /* Expire remote sources after 120s silence: Dante and
                                      * Merging announce every 30s (RFC 2974 suggests
                                      * 10 intervals or an hour) */
#define SAP_RX_BUF_SIZE      1500
#define SAP_MAX_LOCAL         8
#define SAP_MAX_REMOTE        16

#define SAP_RX_TASK_STACK     12288  /* Needs room for callback -> add_source -> SDP log */
#define SAP_TX_TASK_STACK     4096
#define SAP_RX_TASK_PRIO      5   /* Background - discovery */
#define SAP_TX_TASK_PRIO      4   /* Background - periodic announce */

/* Locally announced source */
typedef struct {
    bool active;
    uint16_t msg_id;
    uint32_t origin_ip;
    char sdp[AES67_SAP_MAX_SDP_LEN];
} sap_local_source_t;

/* Full SAP context */
struct aes67_sap_ctx {
    int sock;
    TaskHandle_t rx_task;
    TaskHandle_t tx_task;
    bool running;

    aes67_sap_event_cb_t event_cb;
    void *user_data;

    sap_local_source_t local[SAP_MAX_LOCAL];
    int local_count;

    aes67_sap_remote_source_t remote[SAP_MAX_REMOTE];
    int remote_count;
};

/* Build a SAP packet (announce or delete) into buf.
 * Returns total packet length, or -1 on error. */
static int build_sap_packet(uint8_t *buf, size_t buf_len,
                            uint8_t flags, uint16_t msg_id,
                            uint32_t origin_ip, const char *sdp)
{
    size_t sdp_len = strlen(sdp);
    size_t total = SAP_HEADER_SIZE + SAP_CONTENT_TYPE_LEN + sdp_len;
    if (total > buf_len) {
        return -1;
    }

    /* SAP header per RFC 2974 */
    buf[0] = flags;
    buf[1] = 0;  /* auth_len = 0 */
    buf[2] = (uint8_t)(msg_id >> 8);
    buf[3] = (uint8_t)(msg_id & 0xFF);

    /* Originating source IP. The origin_ip value from lwIP is already
     * in network byte order, so copy the 4 bytes directly. */
    memcpy(buf + 4, &origin_ip, 4);

    /* Content type string with null terminator */
    memcpy(buf + SAP_HEADER_SIZE, SAP_CONTENT_TYPE, SAP_CONTENT_TYPE_LEN);

    /* SDP payload */
    memcpy(buf + SAP_HEADER_SIZE + SAP_CONTENT_TYPE_LEN, sdp, sdp_len);

    return (int)total;
}

/* Send a SAP packet to the multicast group */
static esp_err_t send_sap_packet(int sock, const uint8_t *pkt, int pkt_len)
{
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(AES67_SAP_PORT),
    };
    inet_aton(AES67_SAP_MCAST_ADDR, &dest.sin_addr);

    int sent = sendto(sock, pkt, pkt_len, 0,
                      (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        ESP_LOGE(TAG, "SAP sendto %s:%d failed: errno %d",
                 AES67_SAP_MCAST_ADDR, AES67_SAP_PORT, errno);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "SAP sent %d bytes to %s:%d",
             sent, AES67_SAP_MCAST_ADDR, AES67_SAP_PORT);
    return ESP_OK;
}

/* Find a remote source by origin_ip + msg_id. Returns index or -1. */
static int find_remote(struct aes67_sap_ctx *ctx, uint32_t origin_ip,
                       uint16_t msg_id)
{
    for (int i = 0; i < ctx->remote_count; i++) {
        if (ctx->remote[i].origin_ip == origin_ip &&
            ctx->remote[i].msg_id == msg_id) {
            return i;
        }
    }
    return -1;
}

/* Remove a remote source by index, compacting the array */
static void remove_remote(struct aes67_sap_ctx *ctx, int idx)
{
    if (idx < 0 || idx >= ctx->remote_count) return;

    if (idx < ctx->remote_count - 1) {
        memmove(&ctx->remote[idx], &ctx->remote[idx + 1],
                sizeof(aes67_sap_remote_source_t) * (ctx->remote_count - 1 - idx));
    }
    ctx->remote_count--;
}

/* Extract session name from SDP text for the remote source name field */
static void extract_session_name(const char *sdp, char *name, size_t name_len)
{
    const char *s_line = strstr(sdp, "s=");
    if (!s_line) {
        strncpy(name, "(unknown)", name_len - 1);
        name[name_len - 1] = '\0';
        return;
    }

    s_line += 2;
    const char *end = s_line;
    while (*end && *end != '\r' && *end != '\n') {
        end++;
    }

    size_t len = (size_t)(end - s_line);
    if (len >= name_len) len = name_len - 1;
    memcpy(name, s_line, len);
    name[len] = '\0';
}

/* Handle a received SAP packet */
static void handle_sap_rx(struct aes67_sap_ctx *ctx, const uint8_t *data,
                          int len)
{
    /* Minimum: 8 byte header + content type + some SDP */
    if (len < SAP_HEADER_SIZE + SAP_CONTENT_TYPE_LEN + 4) {
        return;
    }

    uint8_t flags = data[0];
    uint16_t msg_id = ((uint16_t)data[2] << 8) | data[3];
    uint32_t origin_ip;
    memcpy(&origin_ip, data + 4, 4); /* Already in network byte order */

    /* Verify content type */
    if (memcmp(data + SAP_HEADER_SIZE, SAP_CONTENT_TYPE,
               SAP_CONTENT_TYPE_LEN) != 0) {
        ESP_LOGD(TAG, "SAP packet with unknown content type, ignoring");
        return;
    }

    const char *sdp = (const char *)(data + SAP_HEADER_SIZE + SAP_CONTENT_TYPE_LEN);
    int sdp_len = len - SAP_HEADER_SIZE - SAP_CONTENT_TYPE_LEN;

    bool is_delete = (flags & 0x04) != 0;

    if (is_delete) {
        /* Remove this source */
        int idx = find_remote(ctx, origin_ip, msg_id);
        if (idx >= 0) {
            aes67_sap_remote_source_t removed = ctx->remote[idx];
            remove_remote(ctx, idx);
            ESP_LOGI(TAG, "Source gone: \"%s\" (%d sources remaining)",
                     removed.name, ctx->remote_count);
            if (ctx->event_cb) {
                ctx->event_cb(false, &removed, ctx->user_data);
            }
        }
    } else {
        /* Announce: add or update */
        int idx = find_remote(ctx, origin_ip, msg_id);
        if (idx >= 0) {
            /* Update existing entry timestamp */
            ctx->remote[idx].last_seen = xTaskGetTickCount();
            /* Update SDP in case it changed */
            if (sdp_len < (int)sizeof(ctx->remote[idx].sdp)) {
                memcpy(ctx->remote[idx].sdp, sdp, sdp_len);
                ctx->remote[idx].sdp[sdp_len] = '\0';
                extract_session_name(ctx->remote[idx].sdp,
                                     ctx->remote[idx].name,
                                     sizeof(ctx->remote[idx].name));
            }
        } else {
            /* New source */
            if (ctx->remote_count >= SAP_MAX_REMOTE) {
                ESP_LOGW(TAG, "Remote source table full, ignoring new source");
                return;
            }

            aes67_sap_remote_source_t *src = &ctx->remote[ctx->remote_count];
            memset(src, 0, sizeof(*src));
            src->origin_ip = origin_ip;
            src->msg_id = msg_id;
            src->last_seen = xTaskGetTickCount();

            if (sdp_len < (int)sizeof(src->sdp)) {
                memcpy(src->sdp, sdp, sdp_len);
                src->sdp[sdp_len] = '\0';
            }
            extract_session_name(src->sdp, src->name, sizeof(src->name));

            ctx->remote_count++;

            /* Parse SDP for additional info to display */
            char ip_str[16] = {0};
            struct in_addr addr_tmp = { .s_addr = origin_ip };
            const char *ip = inet_ntoa(addr_tmp);
            if (ip) {
                strncpy(ip_str, ip, sizeof(ip_str) - 1);
            }

            ESP_LOGI(TAG, "Discovered: \"%s\" from %s (%d sources on network)",
                     src->name, ip_str, ctx->remote_count);
            if (ctx->event_cb) {
                ctx->event_cb(true, src, ctx->user_data);
            }
        }
    }
}

/* Check for timed-out remote sources (not seen within SAP_TIMEOUT_MS) */
static void expire_remote_sources(struct aes67_sap_ctx *ctx)
{
    uint32_t now = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(SAP_TIMEOUT_MS);

    for (int i = ctx->remote_count - 1; i >= 0; i--) {
        uint32_t elapsed = now - ctx->remote[i].last_seen;
        if (elapsed >= timeout_ticks) {
            aes67_sap_remote_source_t expired = ctx->remote[i];
            remove_remote(ctx, i);
            ESP_LOGI(TAG, "Source timeout: \"%s\" (no announcement for %ds)",
                     expired.name, SAP_TIMEOUT_MS / 1000);
            if (ctx->event_cb) {
                ctx->event_cb(false, &expired, ctx->user_data);
            }
        }
    }
}

/* RX task: listens for incoming SAP packets */
static void sap_rx_task(void *arg)
{
    struct aes67_sap_ctx *ctx = (struct aes67_sap_ctx *)arg;

    /* Allocate RX buffer on heap to avoid stack overflow. select() and
     * the newlib printf internals consume significant stack space. */
    uint8_t *rx_buf = malloc(SAP_RX_BUF_SIZE);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate SAP RX buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SAP RX task started");

    while (ctx->running) {
        /* Use a timeout so we can periodically check for expired sources */
        struct timeval tv = {
            .tv_sec = 5,
            .tv_usec = 0,
        };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);

        int ret = select(ctx->sock + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "SAP select() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (ret > 0 && FD_ISSET(ctx->sock, &rfds)) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int len = recvfrom(ctx->sock, rx_buf, SAP_RX_BUF_SIZE, 0,
                               (struct sockaddr *)&from, &from_len);
            if (len > 0) {
                handle_sap_rx(ctx, rx_buf, len);
            }
        }

        expire_remote_sources(ctx);
    }

    free(rx_buf);
    ESP_LOGI(TAG, "SAP RX task stopped");
    vTaskDelete(NULL);
}

/* TX task: periodically re-announces local sources */
static void sap_tx_task(void *arg)
{
    struct aes67_sap_ctx *ctx = (struct aes67_sap_ctx *)arg;
    uint8_t tx_buf[SAP_RX_BUF_SIZE];

    ESP_LOGI(TAG, "SAP TX task started");

    while (ctx->running) {
        /* Announce all active local sources */
        for (int i = 0; i < SAP_MAX_LOCAL; i++) {
            if (!ctx->local[i].active) continue;

            int pkt_len = build_sap_packet(tx_buf, sizeof(tx_buf),
                                           SAP_ANNOUNCE_FLAG,
                                           ctx->local[i].msg_id,
                                           ctx->local[i].origin_ip,
                                           ctx->local[i].sdp);
            if (pkt_len > 0) {
                send_sap_packet(ctx->sock, tx_buf, pkt_len);
            }
        }

        /* Wait for the next announcement interval, checking running flag */
        for (int i = 0; i < SAP_TX_INTERVAL_MS / 100 && ctx->running; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    ESP_LOGI(TAG, "SAP TX task stopped");
    vTaskDelete(NULL);
}

esp_err_t aes67_sap_init(aes67_sap_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    struct aes67_sap_ctx *ctx = calloc(1, sizeof(struct aes67_sap_ctx));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate SAP context");
        return ESP_ERR_NO_MEM;
    }

    ctx->sock = -1;
    ctx->running = false;
    ctx->event_cb = NULL;
    ctx->user_data = NULL;
    ctx->local_count = 0;
    ctx->remote_count = 0;

    *handle = ctx;
    ESP_LOGI(TAG, "SAP subsystem initialized");
    return ESP_OK;
}

esp_err_t aes67_sap_start(aes67_sap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->running) return ESP_ERR_INVALID_STATE;

    /* Create UDP socket for SAP */
    handle->sock = aes67_net_create_udp_socket(AES67_SAP_PORT, true);
    if (handle->sock < 0) {
        ESP_LOGE(TAG, "Failed to create SAP socket");
        return ESP_FAIL;
    }

    /* Get local IP first so we can bind multicast to the correct interface */
    uint32_t local_ip = 0;
    aes67_net_get_local_ip(&local_ip);
    char local_ip_str[16];
    aes67_net_u32_to_ip(local_ip, local_ip_str, sizeof(local_ip_str));

    /* Join the SAP multicast group on the local interface (not INADDR_ANY) */
    esp_err_t err = aes67_net_join_multicast(handle->sock,
                                              AES67_SAP_MCAST_ADDR,
                                              local_ip_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to join SAP multicast group");
        close(handle->sock);
        handle->sock = -1;
        return err;
    }

    /* RFC 2974 requires SAP packets to have TTL=255 */
    aes67_net_set_multicast_ttl(handle->sock, 255);

    /* Disable multicast loopback so we don't receive our own announcements */
    int loop = 0;
    setsockopt(handle->sock, IPPROTO_IP, IP_MULTICAST_LOOP,
               &loop, sizeof(loop));

    /* Set the outgoing multicast interface to our local IP so packets
     * go out via Ethernet, not a random interface */
    struct in_addr mcast_if = { .s_addr = local_ip };
    setsockopt(handle->sock, IPPROTO_IP, IP_MULTICAST_IF,
               &mcast_if, sizeof(mcast_if));

    handle->running = true;

    /* Start RX task */
    BaseType_t ret = xTaskCreate(sap_rx_task, "sap_rx", SAP_RX_TASK_STACK,
                                 handle, SAP_RX_TASK_PRIO, &handle->rx_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SAP RX task");
        handle->running = false;
        close(handle->sock);
        handle->sock = -1;
        return ESP_ERR_NO_MEM;
    }

    /* Start TX task */
    ret = xTaskCreate(sap_tx_task, "sap_tx", SAP_TX_TASK_STACK,
                      handle, SAP_TX_TASK_PRIO, &handle->tx_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SAP TX task");
        handle->running = false;
        /* Wait a bit for RX task to notice and exit */
        vTaskDelay(pdMS_TO_TICKS(200));
        close(handle->sock);
        handle->sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SAP started on port %d", AES67_SAP_PORT);
    return ESP_OK;
}

esp_err_t aes67_sap_stop(aes67_sap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (!handle->running) return ESP_OK;

    ESP_LOGI(TAG, "Stopping SAP...");
    handle->running = false;

    /* Give tasks time to exit their loops */
    vTaskDelay(pdMS_TO_TICKS(500));

    if (handle->sock >= 0) {
        aes67_net_leave_multicast(handle->sock, AES67_SAP_MCAST_ADDR, NULL);
        close(handle->sock);
        handle->sock = -1;
    }

    handle->rx_task = NULL;
    handle->tx_task = NULL;

    ESP_LOGI(TAG, "SAP stopped");
    return ESP_OK;
}

esp_err_t aes67_sap_destroy(aes67_sap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    if (handle->running) {
        aes67_sap_stop(handle);
    }

    free(handle);
    ESP_LOGI(TAG, "SAP context destroyed");
    return ESP_OK;
}

esp_err_t aes67_sap_announce(aes67_sap_handle_t handle,
                             uint16_t msg_id, uint32_t origin_ip,
                             const char *sdp)
{
    if (!handle || !sdp) return ESP_ERR_INVALID_ARG;

    /* Check if this msg_id is already registered */
    for (int i = 0; i < SAP_MAX_LOCAL; i++) {
        if (handle->local[i].active && handle->local[i].msg_id == msg_id) {
            /* Update existing entry */
            strncpy(handle->local[i].sdp, sdp, AES67_SAP_MAX_SDP_LEN - 1);
            handle->local[i].sdp[AES67_SAP_MAX_SDP_LEN - 1] = '\0';
            handle->local[i].origin_ip = origin_ip;
            ESP_LOGI(TAG, "Updated local source announcement (id=0x%04X)", msg_id);
            return ESP_OK;
        }
    }

    /* Find a free slot */
    for (int i = 0; i < SAP_MAX_LOCAL; i++) {
        if (!handle->local[i].active) {
            handle->local[i].active = true;
            handle->local[i].msg_id = msg_id;
            handle->local[i].origin_ip = origin_ip;
            strncpy(handle->local[i].sdp, sdp, AES67_SAP_MAX_SDP_LEN - 1);
            handle->local[i].sdp[AES67_SAP_MAX_SDP_LEN - 1] = '\0';
            handle->local_count++;
            ESP_LOGI(TAG, "Added local source announcement (id=0x%04X)", msg_id);

            /* Send an immediate announcement if already running */
            if (handle->running && handle->sock >= 0) {
                uint8_t pkt[SAP_RX_BUF_SIZE];
                int pkt_len = build_sap_packet(pkt, sizeof(pkt),
                                               SAP_ANNOUNCE_FLAG,
                                               msg_id, origin_ip, sdp);
                if (pkt_len > 0) {
                    send_sap_packet(handle->sock, pkt, pkt_len);
                }
            }
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "Local source table full (max %d)", SAP_MAX_LOCAL);
    return ESP_ERR_NO_MEM;
}

esp_err_t aes67_sap_delete(aes67_sap_handle_t handle,
                           uint16_t msg_id, uint32_t origin_ip,
                           const char *sdp)
{
    if (!handle || !sdp) return ESP_ERR_INVALID_ARG;

    /* Send a deletion packet */
    if (handle->running && handle->sock >= 0) {
        uint8_t pkt[SAP_RX_BUF_SIZE];
        int pkt_len = build_sap_packet(pkt, sizeof(pkt),
                                       SAP_DELETE_FLAG,
                                       msg_id, origin_ip, sdp);
        if (pkt_len > 0) {
            send_sap_packet(handle->sock, pkt, pkt_len);
        }
    }

    /* Remove from local source table */
    for (int i = 0; i < SAP_MAX_LOCAL; i++) {
        if (handle->local[i].active && handle->local[i].msg_id == msg_id) {
            handle->local[i].active = false;
            handle->local_count--;
            ESP_LOGI(TAG, "Removed local source (id=0x%04X)", msg_id);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Local source id=0x%04X not found for deletion", msg_id);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t aes67_sap_register_cb(aes67_sap_handle_t handle,
                                aes67_sap_event_cb_t cb, void *user_data)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    handle->event_cb = cb;
    handle->user_data = user_data;
    return ESP_OK;
}

int aes67_sap_get_remote_count(aes67_sap_handle_t handle)
{
    if (!handle) return 0;
    return handle->remote_count;
}

esp_err_t aes67_sap_get_remote(aes67_sap_handle_t handle, int index,
                               aes67_sap_remote_source_t *source)
{
    if (!handle || !source) return ESP_ERR_INVALID_ARG;
    if (index < 0 || index >= handle->remote_count) return ESP_ERR_NOT_FOUND;

    *source = handle->remote[index];
    return ESP_OK;
}
