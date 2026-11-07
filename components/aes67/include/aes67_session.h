/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Session manager for AES67 source and sink streams.
 *
 * Orchestrates stream lifecycle: creating/removing sources and sinks,
 * generating SDP, managing SAP announcements, and mapping between
 * RTP streams and the local audio I/O.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "aes67_config.h"
#include "aes67_rtp.h"
#include "aes67_sdp.h"
#include "aes67_ptp.h"
#include "aes67_sap.h"
#include "aes67_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Source stream descriptor */
typedef struct {
    uint8_t id;
    char name[64];
    aes67_rtp_stream_config_t rtp_config;
    aes67_sdp_t sdp;
    aes67_rtp_stream_handle_t rtp_stream;
    uint16_t sap_msg_id;    /* CRC16 hash used for SAP announce/delete matching */
    bool enabled;
} aes67_source_t;

/* Sink stream descriptor */
typedef struct {
    uint8_t id;
    char name[64];
    aes67_rtp_stream_config_t rtp_config;
    aes67_sdp_t sdp;
    aes67_rtp_stream_handle_t rtp_stream;
    bool enabled;
    char remote_sdp[AES67_SDP_MAX_LEN];
} aes67_sink_t;

/* Session manager context (opaque) */
typedef struct aes67_session_mgr *aes67_session_handle_t;

/**
 * Initialize the session manager with references to all subsystems.
 */
esp_err_t aes67_session_init(aes67_rtp_engine_handle_t rtp,
                             aes67_ptp_handle_t ptp,
                             aes67_sap_handle_t sap,
                             aes67_audio_handle_t audio,
                             const aes67_config_t *config,
                             aes67_session_handle_t *handle);

/**
 * Create a source stream. Generates SDP, adds RTP stream, and optionally
 * starts SAP announcements.
 *
 * @param handle    Session manager
 * @param name      Stream name (used in SDP s= line)
 * @param channels  Number of channels (0 = use default)
 * @param codec     Audio codec (or -1 for default)
 * @param id        Output: assigned stream ID
 * @return ESP_OK on success
 */
esp_err_t aes67_session_add_source(aes67_session_handle_t handle,
                                   const char *name,
                                   uint8_t channels,
                                   aes67_codec_t codec,
                                   uint8_t *id);

/**
 * @brief Re-announce local sources whose PTP reference clock changed.
 *
 * The SDP of a source names the grandmaster it is timed against. Call this
 * periodically (e.g. once a second): when the grandmaster changes, or
 * becomes known after start-up, the sources are re-announced with the new
 * identity and a new session version.
 */
esp_err_t aes67_session_refresh_refclk(aes67_session_handle_t handle);

/**
 * Remove a source stream by ID.
 */
esp_err_t aes67_session_remove_source(aes67_session_handle_t handle, uint8_t id);

/**
 * Create a sink stream from an SDP description (e.g. from SAP discovery).
 *
 * @param handle    Session manager
 * @param name      Local name for this sink
 * @param sdp_text  SDP of the remote source to subscribe to
 * @param id        Output: assigned stream ID
 * @return ESP_OK on success
 */
esp_err_t aes67_session_add_sink(aes67_session_handle_t handle,
                                 const char *name,
                                 const char *sdp_text,
                                 uint8_t *id);

/**
 * Remove a sink stream by ID.
 */
esp_err_t aes67_session_remove_sink(aes67_session_handle_t handle, uint8_t id);

/**
 * Get a source descriptor by ID. Returns ESP_ERR_NOT_FOUND if not active.
 */
esp_err_t aes67_session_get_source(aes67_session_handle_t handle, uint8_t id,
                                   aes67_source_t *source);

/**
 * Get a sink descriptor by ID.
 */
esp_err_t aes67_session_get_sink(aes67_session_handle_t handle, uint8_t id,
                                 aes67_sink_t *sink);

/**
 * Get the number of active sources.
 */
int aes67_session_get_source_count(aes67_session_handle_t handle);

/**
 * Get the number of active sinks.
 */
int aes67_session_get_sink_count(aes67_session_handle_t handle);

/**
 * Set the packet time for new source streams (does not affect existing).
 */
void aes67_session_set_packet_time(aes67_session_handle_t handle, uint16_t ptime_us);

/**
 * Destroy session manager and all streams.
 */
esp_err_t aes67_session_destroy(aes67_session_handle_t handle);

#ifdef __cplusplus
}
#endif
