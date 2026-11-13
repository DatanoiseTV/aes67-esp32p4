/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * AES67/RAVENNA node lifecycle management.
 *
 * Coordinates initialization, startup and teardown of all AES67
 * subsystems: PTP, RTP engine, SAP discovery, audio I/O and the
 * session manager that ties them together.
 */

#include "aes67.h"
#include "aes67_config.h"
#include "aes67_ptp.h"
#include "aes67_rtp.h"
#include "aes67_sap.h"
#include "aes67_audio.h"
#include "aes67_session.h"
#include "aes67_hw_timer.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2s_std.h"

static const char *TAG = "aes67";

/* mDNS init/deinit -- declared here since aes67_mdns.c has no header */
esp_err_t aes67_mdns_init(const char *node_name, uint16_t rtp_port);
void aes67_mdns_deinit(void);

/* Define the event base for the ESP event loop */
ESP_EVENT_DEFINE_BASE(AES67_EVENT);

/* Semaphore signaled by the hardware PTP timer ISR */
static SemaphoreHandle_t s_frame_sem = NULL;

/* Internal node structure holding all subsystem handles */
struct aes67_node {
    aes67_config_t              config;
    aes67_ptp_handle_t          ptp;
    aes67_rtp_engine_handle_t   rtp;
    aes67_sap_handle_t          sap;
    aes67_audio_handle_t        audio;
    aes67_session_handle_t      session;
    TaskHandle_t                tx_task;
    TaskHandle_t                refclk_task;           /* NULL when not running */
    volatile bool               refclk_stop;
    StreamBufferHandle_t        external_playout_buf;  /* optional, see aes67_node_set_playout_buf */
    bool                        running;
};

/* Global node pointer for the ISR callback (single instance) */
static aes67_node_handle_t s_node = NULL;

/*
 * Hardware PTP timer ISR callback. Fires at exact PTP-clock-aligned
 * audio frame boundaries. Gives the semaphore to wake the TX task.
 */
static void IRAM_ATTR hw_frame_isr(void *user_data)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_sem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/*
 * Unified audio frame task (TIC). Driven by the hardware PTP timer ISR,
 * this task handles both TX and RX audio processing at PTP-aligned
 * frame boundaries:
 *
 * TX path: Read from source ring buffers -> convert -> send RTP packets
 * RX path: Read from sink jitter buffers -> write to I2S playback
 *
 * This ensures both directions are synchronized to the PTP clock with
 * hardware-precision timing.
 */
static void audio_frame_task(void *arg)
{
    aes67_node_handle_t node = (aes67_node_handle_t)arg;
    uint32_t frame_count = 0;

    /* Pre-allocate playback buffer for sink -> I2S path */
    uint32_t spp = (node->config.audio.sample_rate *
                     node->config.audio.packet_time_us) / 1000000;
    int32_t *playback_buf = heap_caps_malloc(
        spp * node->config.audio.channels * sizeof(int32_t),
        MALLOC_CAP_INTERNAL);

    /* Capture buffer for I2S RX -> source stream feeding */
    StreamBufferHandle_t cap_buf = aes67_audio_get_capture_buf(node->audio);
    int32_t *cap_samples = NULL;
    if (cap_buf) {
        cap_samples = heap_caps_malloc(
            spp * node->config.audio.channels * sizeof(int32_t),
            MALLOC_CAP_INTERNAL);
    }

    ESP_LOGI(TAG, "Audio frame task started (hw-timed TX%s)",
             cap_buf ? "+capture" : "");

    while (node->running) {
        /* Wait for the hardware PTP frame timer ISR */
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }

        frame_count++;

        /* Get current PTP time for RTP timestamp generation */
        uint64_t ptp_time_ns;
        aes67_ptp_get_time_ns(node->ptp, &ptp_time_ns);
        uint32_t rtp_ts = aes67_ptp_time_to_rtp_ts(
            ptp_time_ns, node->config.audio.sample_rate);

        /* Feed captured I2S audio into source stream ring buffers.
         * The RX DMA ISR writes captured int32 samples to cap_buf.
         * We read one packet worth and write to all active sources. */
        if (cap_buf && cap_samples) {
            uint32_t need = spp * node->config.audio.channels * sizeof(int32_t);
            size_t got = xStreamBufferReceive(cap_buf, cap_samples, need, 0);
            if (got >= need) {
                /* Write captured audio to all active source streams */
                for (int i = 0; i < CONFIG_AES67_MAX_SOURCES; i++) {
                    aes67_source_t src;
                    if (aes67_session_get_source(node->session, i, &src) == ESP_OK &&
                        src.enabled && src.rtp_stream) {
                        aes67_rtp_source_write(src.rtp_stream, cap_samples, spp);
                    }
                }
            }
        }

        /* TX: Process all active source streams - read from ring buffers,
         * convert to network format, send RTP packets */
        aes67_rtp_engine_process_tx(node->rtp, rtp_ts);
    }

    ESP_LOGI(TAG, "Audio frame task stopped");
    vTaskDelete(NULL);
}

/*
 * Keeps the reference clock in the announced SDPs current: once a second the
 * session manager compares each source's ts-refclk with the PTP grandmaster
 * and re-announces the source when it changed (or became known after start).
 * Low priority, off the audio path. A notification ends the wait early so
 * aes67_node_stop() does not have to sit out the full period.
 */
static void refclk_task(void *arg)
{
    aes67_node_handle_t node = (aes67_node_handle_t)arg;

    while (!node->refclk_stop) {
        aes67_session_refresh_refclk(node->session);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }

    node->refclk_task = NULL;
    vTaskDelete(NULL);
}

/*
 * Dedicated playback task. Runs in a tight loop reading from the
 * first active sink's jitter buffer and writing to I2S.
 * i2s_channel_write blocks until DMA has room, which naturally
 * paces output at 48kHz. This task is completely decoupled from
 * both the RTP RX task and the TIC task.
 */
static void playback_task(void *arg)
{
    aes67_node_handle_t node = (aes67_node_handle_t)arg;
    const uint8_t ch = node->config.audio.channels;

    /* Playback buffer: holds up to 768 frames (16ms at 48kHz = 4 DMA descriptors).
     * Reading multiple packets per i2s_channel_write call amortizes the ~0.4ms
     * overhead of xStreamBufferReceive + context switches. Without this, the
     * per-iteration overhead causes ~10% throughput loss. */
    const uint32_t max_frames = 768;
    const uint32_t buf_bytes = max_frames * ch * sizeof(int32_t);
    int32_t *pcm_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Playback task: alloc failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Playback task started (ch=%u, DMA-paced, buf=%lu bytes)",
             ch, (unsigned long)buf_bytes);

    uint32_t total_frames = 0;
    uint32_t write_count = 0;
    bool sink_found = false;

    while (node->running) {
        /* Wait for a sink to be added */
        if (!sink_found) {
            int sink_count = aes67_session_get_sink_count(node->session);
            for (int s = 0; s < sink_count && s < CONFIG_AES67_MAX_SINKS; s++) {
                aes67_sink_t sink;
                if (aes67_session_get_sink(node->session, s, &sink) == ESP_OK &&
                    sink.enabled && sink.rtp_stream) {
                    sink_found = true;
                    ESP_LOGI(TAG, "Playback: locked to sink '%s' "
                             "(ch=%u, wl=%u, rate=%lu, ptime=%lu us)",
                             sink.name, sink.rtp_config.channels,
                             sink.rtp_config.word_length,
                             (unsigned long)sink.rtp_config.sample_rate,
                             (unsigned long)sink.rtp_config.packet_time_us);
                    break;
                }
            }
            if (!sink_found) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        /* Get the stream buffer: the optional external feed (e.g. an Rx-hook
         * integration) if one was registered, otherwise the RTP engine's. */
        StreamBufferHandle_t sbuf = node->external_playout_buf ?
                                     node->external_playout_buf :
                                     aes67_rtp_engine_get_stream_buf(node->rtp);
        if (!sbuf) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t frame_align = ch * sizeof(int32_t);
        if (!sink_found) {
            continue;
        }

        /* Two-stage read: first blocking wait for at least one packet,
         * then immediately drain ALL accumulated data non-blocking.
         * This batches multiple packets into one i2s_channel_write call,
         * amortizing the ~0.4ms task scheduling overhead that otherwise
         * causes 10% throughput loss (4ms DMA cycle + 0.4ms overhead). */
        size_t received = xStreamBufferReceive(sbuf, pcm_buf, frame_align,
                                                pdMS_TO_TICKS(20));
        if (received < frame_align) {
            /* Stream stopped: flush silence through all DMA descriptors.
             * With auto_clear=false, old data replays unless we push zeros. */
            i2s_chan_handle_t tx = aes67_audio_get_tx_chan(node->audio);
            if (tx && node->config.output_mode == AES67_OUTPUT_I2S) {
                memset(pcm_buf, 0, buf_bytes);
                for (int d = 0; d < 4; d++) {
                    size_t written = 0;
                    i2s_channel_write(tx, pcm_buf, 192 * frame_align,
                                      &written, pdMS_TO_TICKS(10));
                }
            }
            continue;
        }
        /* Drain all remaining data immediately (non-blocking) */
        if (received < buf_bytes) {
            size_t extra = xStreamBufferReceive(sbuf, (uint8_t *)pcm_buf + received,
                                                buf_bytes - received, 0);
            received += extra;
        }

        /* Align to frame boundary */
        uint32_t frames = received / frame_align;
        uint32_t aligned_bytes = frames * frame_align;

        /* Route to output */
        switch (node->config.output_mode) {
        case AES67_OUTPUT_I2S: {
            i2s_chan_handle_t tx = aes67_audio_get_tx_chan(node->audio);
            if (tx) {
                size_t written = 0;
                i2s_channel_write(tx, pcm_buf, aligned_bytes,
                                  &written, portMAX_DELAY);
            }
            break;
        }
        case AES67_OUTPUT_CALLBACK:
            if (node->config.audio_cb) {
                node->config.audio_cb(pcm_buf, frames, ch,
                                      node->config.audio.word_length,
                                      node->config.audio.sample_rate,
                                      node->config.audio_cb_user_data);
            }
            break;
        case AES67_OUTPUT_STREAM_BUFFER:
            /* Data stays in stream buffer for app consumption */
            break;
        }

        total_frames += frames;
        write_count++;

        if ((write_count % 1000) == 0) {
            static int64_t last_stat_us = 0;
            static uint32_t last_stat_frames = 0;
            int64_t now = esp_timer_get_time();
            if (last_stat_us > 0) {
                int64_t delta_us = now - last_stat_us;
                uint32_t delta_frames = total_frames - last_stat_frames;
                if (delta_us > 0) {
                    ESP_LOGI(TAG, "PB: %lu fps, sb=%u",
                             (unsigned long)(delta_frames * 1000000ULL / delta_us),
                             (unsigned)xStreamBufferBytesAvailable(sbuf));
                }
            }
            last_stat_us = now;
            last_stat_frames = total_frames;
        }
    }

    heap_caps_free(pcm_buf);
    ESP_LOGI(TAG, "Playback task stopped");
    vTaskDelete(NULL);
}

/*
 * Audio frame callback from I2S DMA (unused - kept for API compat).
 */
static void audio_frame_callback(uint32_t frame_count, void *user_data)
{
}

esp_err_t aes67_node_init(const aes67_config_t *config, aes67_node_handle_t *handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_node *node = calloc(1, sizeof(struct aes67_node));
    if (!node) {
        ESP_LOGE(TAG, "failed to allocate node context");
        return ESP_ERR_NO_MEM;
    }

    /* Apply caller config or fall back to Kconfig defaults */
    if (config) {
        memcpy(&node->config, config, sizeof(aes67_config_t));
    } else {
        aes67_config_t defaults = AES67_CONFIG_DEFAULT();
        memcpy(&node->config, &defaults, sizeof(aes67_config_t));
    }

    esp_err_t ret;

    /* 1. PTP clock subsystem */
    ret = aes67_ptp_init(node->config.net.eth_handle, &node->config.ptp, &node->ptp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PTP init failed: %s", esp_err_to_name(ret));
        goto fail_ptp;
    }

    /* 2. RTP packet engine */
    ret = aes67_rtp_engine_init(&node->config.net, node->config.rtp_rings_psram, &node->rtp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTP engine init failed: %s", esp_err_to_name(ret));
        goto fail_rtp;
    }

    /* 3. SAP discovery (optional) */
    if (node->config.sap_enabled) {
        ret = aes67_sap_init(&node->sap);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SAP init failed: %s", esp_err_to_name(ret));
            goto fail_sap;
        }
    }

    /* 4. Audio I/O driver (I2S + DMA) */
    ret = aes67_audio_init(&node->config.audio, &node->config.i2s_pins,
                           node->config.use_psram, node->config.ring_buffer_ms,
                           &node->audio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(ret));
        goto fail_audio;
    }

    /* 5. Session manager -- connects all subsystems */
    ret = aes67_session_init(node->rtp, node->ptp, node->sap,
                             node->audio, &node->config, &node->session);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "session manager init failed: %s", esp_err_to_name(ret));
        goto fail_session;
    }

    /* 6. Register the audio frame callback (for I2S DMA capture handling) */
    ret = aes67_audio_register_frame_cb(node->audio, audio_frame_callback, node);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio frame callback registration failed: %s",
                 esp_err_to_name(ret));
        goto fail_cb;
    }

    /* 7. Initialize hardware PTP frame timer for RTP TX.
     * frame_period = samples_per_packet / sample_rate in nanoseconds */
    uint32_t samples_per_pkt = (node->config.audio.sample_rate *
                                 node->config.audio.packet_time_us) / 1000000;
    uint32_t frame_ns = (uint32_t)((uint64_t)samples_per_pkt * 1000000000ULL /
                                    node->config.audio.sample_rate);

    /* Create the frame semaphore for ISR -> task signaling */
    s_frame_sem = xSemaphoreCreateBinary();
    if (!s_frame_sem) {
        ESP_LOGE(TAG, "failed to create frame semaphore");
        goto fail_cb;
    }

    ret = aes67_hw_timer_init(frame_ns, hw_frame_isr, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hw PTP timer init failed: %s (falling back to I2S timing)",
                 esp_err_to_name(ret));
        /* Non-fatal: I2S DMA callback still works as fallback */
    }

    s_node = node;
    node->running = false;
    *handle = node;

    ESP_LOGI(TAG, "node \"%s\" initialized (v%d.%d.%d)",
             node->config.node_name,
             AES67_VERSION_MAJOR, AES67_VERSION_MINOR, AES67_VERSION_PATCH);

    return ESP_OK;

fail_cb:
    aes67_session_destroy(node->session);
fail_session:
    aes67_audio_destroy(node->audio);
fail_audio:
    if (node->config.sap_enabled && node->sap) {
        aes67_sap_destroy(node->sap);
    }
fail_sap:
    aes67_rtp_engine_destroy(node->rtp);
fail_rtp:
    aes67_ptp_destroy(node->ptp);
fail_ptp:
    free(node);
    return ret;
}

esp_err_t aes67_node_start(aes67_node_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->running) {
        ESP_LOGW(TAG, "node already running");
        return ESP_OK;
    }

    esp_err_t ret;

    /* Start subsystems in dependency order */
    ret = aes67_ptp_start(handle->ptp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PTP start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = aes67_rtp_engine_start(handle->rtp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTP engine start failed: %s", esp_err_to_name(ret));
        aes67_ptp_stop(handle->ptp);
        return ret;
    }

    if (handle->config.sap_enabled && handle->sap) {
        ret = aes67_sap_start(handle->sap);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SAP start failed: %s", esp_err_to_name(ret));
            aes67_rtp_engine_stop(handle->rtp);
            aes67_ptp_stop(handle->ptp);
            return ret;
        }
    }

    ret = aes67_audio_start(handle->audio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(ret));
        if (handle->config.sap_enabled && handle->sap) {
            aes67_sap_stop(handle->sap);
        }
        aes67_rtp_engine_stop(handle->rtp);
        aes67_ptp_stop(handle->ptp);
        return ret;
    }

    /* mDNS advertisement (optional) */
    if (handle->config.mdns_enabled) {
        ret = aes67_mdns_init(handle->config.node_name, handle->config.net.rtp_port);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "mDNS init failed: %s (continuing without mDNS)",
                     esp_err_to_name(ret));
            /* Non-fatal: the node still works without mDNS */
        }
    }

    handle->running = true;

    /* Enable direct RTP RX -> staging ring path */
    aes67_rtp_engine_set_playback(handle->rtp, handle->audio);
    ESP_LOGI(TAG, "Direct RTP->I2S path enabled (zero-buffer)");

    /* Playback task: only needed for callback/stream_buffer output modes.
     * For I2S mode, the DMA ISR reads directly from the stream buffer,
     * bypassing the playback task entirely (zero scheduling overhead). */
    if (handle->config.output_mode != AES67_OUTPUT_I2S) {
        static TaskHandle_t pb_task = NULL;
        xTaskCreatePinnedToCore(playback_task, "aes67_pb", 8192, handle,
                                21, &pb_task, 0);
        aes67_rtp_engine_set_notify_task(handle->rtp, pb_task);
        ESP_LOGI(TAG, "Playback task started (output_mode=%d)", handle->config.output_mode);
    } else {
        ESP_LOGI(TAG, "I2S output via DMA ISR (no playback task needed)");
    }

    /* Start the hardware PTP frame timer and audio frame task.
     * This task handles both TX and RX at PTP-aligned boundaries. */
    ret = aes67_hw_timer_start();
    if (ret == ESP_OK) {
        BaseType_t xret = xTaskCreatePinnedToCore(
            audio_frame_task, "aes67_tic", 8192, handle,
            21,     /* High priority, just below audio I/O (22) */
            &handle->tx_task,
            0       /* Pin to core 0 (PTP/network core) */
        );
        if (xret != pdPASS) {
            ESP_LOGW(TAG, "failed to create audio frame task");
            aes67_hw_timer_stop();
        } else {
            ESP_LOGI(TAG, "Audio frame processing driven by hardware PTP timer");
        }
    } else {
        ESP_LOGW(TAG, "hw timer start failed, using I2S DMA callback");
    }

    handle->refclk_stop = false;
    if (xTaskCreate(refclk_task, "aes67_refclk", 6144, handle,
                    tskIDLE_PRIORITY + 2, &handle->refclk_task) != pdPASS) {
        handle->refclk_task = NULL;
        ESP_LOGW(TAG, "failed to create refclk task: SDP keeps its start-up reference clock");
    }

    ESP_LOGI(TAG, "node started");

    return ESP_OK;
}

esp_err_t aes67_node_stop(aes67_node_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->running) {
        ESP_LOGW(TAG, "node not running");
        return ESP_OK;
    }

    /* Tear down in reverse order */
    if (handle->refclk_task) {
        /* The task uses the session and SAP handles: wait until it is gone */
        handle->refclk_stop = true;
        xTaskNotifyGive(handle->refclk_task);
        while (handle->refclk_task) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (handle->config.mdns_enabled) {
        aes67_mdns_deinit();
    }

    aes67_audio_stop(handle->audio);

    if (handle->config.sap_enabled && handle->sap) {
        aes67_sap_stop(handle->sap);
    }

    aes67_rtp_engine_stop(handle->rtp);
    aes67_ptp_stop(handle->ptp);

    handle->running = false;
    ESP_LOGI(TAG, "node stopped");

    return ESP_OK;
}

esp_err_t aes67_node_destroy(aes67_node_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Stop first if still running */
    if (handle->running) {
        aes67_node_stop(handle);
    }

    /* Destroy subsystems in reverse init order */
    aes67_session_destroy(handle->session);
    aes67_audio_destroy(handle->audio);

    if (handle->config.sap_enabled && handle->sap) {
        aes67_sap_destroy(handle->sap);
    }

    aes67_rtp_engine_destroy(handle->rtp);
    aes67_ptp_destroy(handle->ptp);

    ESP_LOGI(TAG, "node \"%s\" destroyed", handle->config.node_name);
    free(handle);

    return ESP_OK;
}

esp_err_t aes67_node_get_config(aes67_node_handle_t handle, aes67_config_t *config)
{
    if (!handle || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(config, &handle->config, sizeof(aes67_config_t));
    return ESP_OK;
}

esp_err_t aes67_node_get_session(aes67_node_handle_t handle,
                                  aes67_session_handle_t *session)
{
    if (!handle || !session) {
        return ESP_ERR_INVALID_ARG;
    }

    *session = handle->session;
    return ESP_OK;
}

esp_err_t aes67_node_get_audio(aes67_node_handle_t handle,
                               aes67_audio_handle_t *audio)
{
    if (!handle || !audio) {
        return ESP_ERR_INVALID_ARG;
    }
    *audio = handle->audio;
    return ESP_OK;
}

esp_err_t aes67_node_get_sap(aes67_node_handle_t handle,
                              aes67_sap_handle_t *sap)
{
    if (!handle || !sap) {
        return ESP_ERR_INVALID_ARG;
    }

    *sap = handle->sap;
    return ESP_OK;
}

esp_err_t aes67_node_get_ptp(aes67_node_handle_t handle,
                              aes67_ptp_handle_t *ptp)
{
    if (!handle || !ptp) {
        return ESP_ERR_INVALID_ARG;
    }

    *ptp = handle->ptp;
    return ESP_OK;
}

esp_err_t aes67_node_get_rtp(aes67_node_handle_t handle,
                              aes67_rtp_engine_handle_t *rtp)
{
    if (!handle || !rtp) {
        return ESP_ERR_INVALID_ARG;
    }

    *rtp = handle->rtp;
    return ESP_OK;
}

esp_err_t aes67_node_set_playout_buf(aes67_node_handle_t handle,
                                      StreamBufferHandle_t sbuf)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    handle->external_playout_buf = sbuf;
    return ESP_OK;
}
