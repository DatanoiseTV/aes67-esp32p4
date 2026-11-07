/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * RTP audio streaming engine.
 *
 * Handles RTP packet construction, parsing, transmission and reception
 * for AES67 audio streams. Manages per-stream jitter buffers allocated
 * in PSRAM when available.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "aes67_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RTP header as defined in RFC 3550, packed for network wire format */
typedef struct __attribute__((packed)) {
    uint8_t  flags;         /* V=2, P, X, CC */
    uint8_t  pt;            /* M bit + payload type */
    uint16_t seq;           /* Sequence number (network byte order) */
    uint32_t timestamp;     /* RTP timestamp (network byte order) */
    uint32_t ssrc;          /* Synchronization source (network byte order) */
} aes67_rtp_header_t;

/* Stream direction */
typedef enum {
    AES67_STREAM_SOURCE = 0,    /* TX: capture audio, send RTP */
    AES67_STREAM_SINK   = 1,    /* RX: receive RTP, play audio */
} aes67_stream_dir_t;

/* Stream status flags (bitmask) */
#define AES67_RTP_STATUS_SEQ_ERROR      (1 << 0)
#define AES67_RTP_STATUS_SSRC_ERROR     (1 << 1)
#define AES67_RTP_STATUS_PT_ERROR       (1 << 2)
#define AES67_RTP_STATUS_TIMESTAMP_ERR  (1 << 3)
#define AES67_RTP_STATUS_RECEIVING      (1 << 4)
#define AES67_RTP_STATUS_MUTED          (1 << 5)
#define AES67_RTP_STATUS_OVERFLOW       (1 << 6)
#define AES67_RTP_STATUS_UNDERFLOW      (1 << 7)

/* Configuration for a single RTP stream */
typedef struct {
    aes67_stream_dir_t direction;
    uint32_t dest_ip;               /* Multicast destination IP (network order) */
    uint16_t dest_port;             /* Destination port (host order) */
    uint16_t src_port;              /* Source port (host order) */
    uint8_t  payload_type;          /* RTP payload type (e.g. 98) */
    uint32_t ssrc;                  /* SSRC identifier */
    uint32_t sample_rate;           /* Sample rate in Hz */
    uint8_t  channels;              /* Number of audio channels */
    uint8_t  word_length;           /* Bytes per sample (2, 3, or 4) */
    aes67_codec_t codec;            /* Audio codec */
    uint16_t packet_time_us;        /* Packet time in microseconds */
    uint32_t playout_delay_us;      /* Playout delay for sinks */
    uint8_t  dscp;                  /* DSCP marking */
    uint8_t  ttl;                   /* Multicast TTL */
    /* Channel routing: maps RTP channel index to local audio channel */
    uint8_t  routing[CONFIG_AES67_MAX_CHANNELS_PER_STREAM];
} aes67_rtp_stream_config_t;

/* Runtime stream status */
typedef struct {
    uint32_t status_flags;          /* Bitmask of AES67_RTP_STATUS_* */
    uint32_t packets_sent;          /* Total packets transmitted */
    uint32_t packets_received;      /* Total packets received */
    uint32_t packets_lost;          /* Detected packet loss count */
    uint32_t packets_late;          /* Packets arriving after playout */
    uint32_t seq_errors;            /* Sequence discontinuity count */
    int32_t  jitter_us;             /* Estimated jitter in microseconds */
    uint32_t last_rtp_timestamp;    /* Last RTP timestamp processed */
    uint16_t last_seq;              /* Last sequence number */
} aes67_rtp_stream_status_t;

/* Opaque stream handle */
typedef struct aes67_rtp_stream *aes67_rtp_stream_handle_t;

/* Opaque RTP engine handle */
typedef struct aes67_rtp_engine *aes67_rtp_engine_handle_t;

/**
 * Initialize the RTP engine. Creates the network sockets and processing tasks.
 */
esp_err_t aes67_rtp_engine_init(const aes67_net_config_t *net_config,
                                bool use_psram,
                                aes67_rtp_engine_handle_t *handle);

/**
 * Start the RTP engine (begins packet processing).
 */
esp_err_t aes67_rtp_engine_start(aes67_rtp_engine_handle_t handle);

/**
 * Stop the RTP engine.
 */
esp_err_t aes67_rtp_engine_stop(aes67_rtp_engine_handle_t handle);

/**
 * Destroy the RTP engine and all streams.
 */
esp_err_t aes67_rtp_engine_destroy(aes67_rtp_engine_handle_t handle);

/**
 * Add an RTP stream (source or sink). Returns a stream handle.
 */
esp_err_t aes67_rtp_stream_add(aes67_rtp_engine_handle_t engine,
                               const aes67_rtp_stream_config_t *config,
                               aes67_rtp_stream_handle_t *stream);

/**
 * Remove an RTP stream.
 */
esp_err_t aes67_rtp_stream_remove(aes67_rtp_engine_handle_t engine,
                                  aes67_rtp_stream_handle_t stream);

/**
 * Get stream status.
 */
/**
 * @brief Take the RTP timestamp of the next aes67_rtp_engine_process_tx() call.
 *
 * A source takes its RTP timestamp from the first process_tx() call and
 * counts on from there. When the application's media clock is re-aligned to
 * PTP (after locking, or a PTP step), call this so the next packet starts
 * from the timestamp passed with it.
 */
esp_err_t aes67_rtp_stream_resync_timestamp(aes67_rtp_stream_handle_t stream);

esp_err_t aes67_rtp_stream_get_status(aes67_rtp_stream_handle_t stream,
                                      aes67_rtp_stream_status_t *status);

/**
 * Write audio samples to a source stream's buffer (called by audio driver).
 * Samples are interleaved, in native byte order, at the stream's word length.
 *
 * @param stream        Source stream handle
 * @param samples       Pointer to interleaved sample data
 * @param frame_count   Number of audio frames (one sample per channel = one frame)
 * @return ESP_OK on success
 */
esp_err_t aes67_rtp_source_write(aes67_rtp_stream_handle_t stream,
                                 const void *samples, uint32_t frame_count);

/**
 * Read audio samples from a sink stream's jitter buffer (called by audio driver).
 * Returns silence if the jitter buffer is empty or the stream is muted.
 *
 * @param stream        Sink stream handle
 * @param samples       Output buffer for interleaved sample data
 * @param frame_count   Number of frames to read
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no data available
 */
esp_err_t aes67_rtp_sink_read(aes67_rtp_stream_handle_t stream,
                              void *samples, uint32_t frame_count);

/**
 * Called by the audio frame timer (TIC) to trigger RTP packet transmission
 * for all active source streams.
 */
esp_err_t aes67_rtp_engine_process_tx(aes67_rtp_engine_handle_t handle,
                                      uint32_t rtp_timestamp);

/**
 * Get the stream buffer used for RTP RX -> playback decoupling.
 */
#include "freertos/stream_buffer.h"
StreamBufferHandle_t aes67_rtp_engine_get_stream_buf(aes67_rtp_engine_handle_t handle);

/**
 * Set the task to notify when new sink data arrives.
 */
#include "freertos/task.h"
void aes67_rtp_engine_set_notify_task(aes67_rtp_engine_handle_t handle,
                                      TaskHandle_t task);

/**
 * Enable/disable direct I2S playback from the RTP RX path.
 */
void aes67_rtp_engine_set_playback(aes67_rtp_engine_handle_t handle,
                                   void *audio_handle);

/**
 * Get available frames in a sink stream's jitter buffer.
 */
uint32_t aes67_rtp_sink_available(aes67_rtp_stream_handle_t stream);

#ifdef __cplusplus
}
#endif
