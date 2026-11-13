/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * AES67 configuration structures.
 *
 * Kconfig provides compile-time defaults; these structs allow runtime
 * override before node initialization.
 *
 * Key design: all buffer sizes and frame counts are COMPUTED from the
 * base audio parameters (sample_rate, channels, word_length, packet_time_us)
 * via aes67_stream_params_compute(). No hardcoded 192, 2, 48000, etc.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_eth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version ---- */
#define AES67_VERSION_MAJOR  2
#define AES67_VERSION_MINOR  2
#define AES67_VERSION_PATCH  0

/* ---- Limits ---- */
#define AES67_MAX_CHANNELS_PER_STREAM  8
#define AES67_MAX_TIC_FRAMES           1024
#define AES67_MIN_TIC_FRAMES           48
#define AES67_RTP_HEADER_SIZE          12
#define AES67_MAX_WORD_LENGTH          4    /* AM824/L32 = 4 bytes */

/* ---- Enums ---- */

/* Audio codec identifiers matching AES67/RAVENNA conventions */
typedef enum {
    AES67_CODEC_L16 = 0,   /* 16-bit linear PCM, word_length=2 */
    AES67_CODEC_L24,        /* 24-bit linear PCM, word_length=3 */
    AES67_CODEC_L32,        /* 32-bit linear PCM, word_length=4 */
    AES67_CODEC_AM824,      /* IEC 61937 / AM824, word_length=4 */
} aes67_codec_t;

/* Output mode for received audio data */
typedef enum {
    AES67_OUTPUT_I2S = 0,           /* Direct I2S output (default) */
    AES67_OUTPUT_STREAM_BUFFER,     /* FreeRTOS StreamBuffer for app processing */
    AES67_OUTPUT_CALLBACK,          /* User callback per packet */
} aes67_output_mode_t;

/* Callback for received audio data (when output_mode == AES67_OUTPUT_CALLBACK) */
typedef void (*aes67_audio_data_cb_t)(const int32_t *samples,
                                       uint32_t frame_count,
                                       uint8_t channels,
                                       uint8_t word_length,
                                       uint32_t sample_rate,
                                       void *user_data);

/* ---- Stream Parameters (computed, not configured) ---- */

/*
 * Derived parameters computed from the base audio config.
 * Use aes67_stream_params_compute() to populate.
 * These replace all hardcoded 192, 768, 1536, etc.
 */
typedef struct {
    /* From config */
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  word_length;           /* Bytes per sample on the wire (2/3/4) */
    uint16_t packet_time_us;

    /* Computed */
    uint32_t samples_per_packet;    /* Frames per RTP packet (e.g., 48, 192) */
    uint32_t frame_bytes_wire;      /* Bytes per frame on wire: channels * word_length */
    uint32_t frame_bytes_native;    /* Bytes per frame in memory: channels * 4 (int32) */
    uint32_t packet_payload_size;   /* RTP payload bytes: spp * frame_bytes_wire */
    uint32_t packet_native_size;    /* Converted packet bytes: spp * frame_bytes_native */
    uint32_t max_packet_payload;    /* Max RTP payload: MAX_TIC_FRAMES * frame_bytes_wire */
} aes67_stream_params_t;

/* Compute derived parameters from base config */
static inline void aes67_stream_params_compute(aes67_stream_params_t *p,
                                                uint32_t sample_rate,
                                                uint8_t channels,
                                                uint8_t word_length,
                                                uint16_t packet_time_us)
{
    p->sample_rate = sample_rate;
    p->channels = channels;
    p->word_length = word_length;
    p->packet_time_us = packet_time_us;

    p->samples_per_packet = (sample_rate * (uint32_t)packet_time_us) / 1000000;
    if (p->samples_per_packet == 0) p->samples_per_packet = 48;
    if (p->samples_per_packet > AES67_MAX_TIC_FRAMES) {
        p->samples_per_packet = AES67_MAX_TIC_FRAMES;
    }

    p->frame_bytes_wire = (uint32_t)channels * word_length;
    p->frame_bytes_native = (uint32_t)channels * sizeof(int32_t);
    p->packet_payload_size = p->samples_per_packet * p->frame_bytes_wire;
    p->packet_native_size = p->samples_per_packet * p->frame_bytes_native;
    p->max_packet_payload = AES67_MAX_TIC_FRAMES * p->frame_bytes_wire;
}

/* Get word_length from codec enum */
static inline uint8_t aes67_codec_word_length(aes67_codec_t codec)
{
    switch (codec) {
    case AES67_CODEC_L16:  return 2;
    case AES67_CODEC_L24:  return 3;
    case AES67_CODEC_L32:  return 4;
    case AES67_CODEC_AM824: return 4;
    default: return 3;
    }
}

/* Get codec from word_length */
static inline aes67_codec_t aes67_word_length_codec(uint8_t wl)
{
    switch (wl) {
    case 2: return AES67_CODEC_L16;
    case 3: return AES67_CODEC_L24;
    case 4: return AES67_CODEC_L32;
    default: return AES67_CODEC_L24;
    }
}

/* ---- Configuration Structs ---- */

/* Network configuration */
typedef struct {
    char interface_name[16];            /* Ethernet interface, e.g. "ETH_0" */
    esp_eth_handle_t eth_handle;        /* Ethernet driver handle */
    char rtp_mcast_base[16];            /* Base multicast address for RTP */
    uint16_t rtp_port;                  /* RTP port (default 5004) */
    uint8_t ttl;                        /* Multicast TTL */
    uint8_t dscp;                       /* DSCP for QoS (default 46 = EF) */
} aes67_net_config_t;

/* PTP configuration */
typedef struct {
    uint8_t domain;                     /* PTP domain 0-127 */
    uint8_t dscp;                       /* DSCP for PTP packets */
    bool prefer_grandmaster;            /* Become GM if no other master found */
} aes67_ptp_config_t;

/* Audio configuration */
typedef struct {
    uint32_t sample_rate;               /* Sample rate in Hz (44100 or 48000) */
    uint8_t word_length;                /* Bytes per sample (2, 3, or 4) */
    uint16_t packet_time_us;            /* Packet time in microseconds */
    uint8_t channels;                   /* Number of audio channels (1-8) */
    aes67_codec_t codec;                /* Audio codec */
} aes67_audio_config_t;

/* I2S pin configuration */
typedef struct {
    int mck_gpio;
    int bck_gpio;
    int ws_gpio;
    int dout_gpio;
    int din_gpio;
} aes67_i2s_pins_t;

/* Top-level node configuration */
typedef struct {
    char node_name[64];                 /* Node name for SDP/mDNS */
    aes67_net_config_t net;
    aes67_ptp_config_t ptp;
    aes67_audio_config_t audio;
    aes67_i2s_pins_t i2s_pins;
    uint16_t jitter_buffer_ms;          /* Jitter buffer depth in ms */
    uint16_t ring_buffer_ms;            /* Audio ring buffer size in ms */
    bool use_psram;                     /* Allocate large buffers in PSRAM */
    bool rtp_rings_psram;               /* Also put the per-stream RTP rings in PSRAM.
                                         * Off by default: PSRAM reads cost throughput
                                         * in the i2s_channel_write playout path. Saves
                                         * ~9 KB of internal RAM per 8-channel stream. */
    bool sap_enabled;                   /* Enable SAP announcements */
    bool mdns_enabled;                  /* Enable mDNS advertisement */

    /* Output routing */
    aes67_output_mode_t output_mode;    /* How to deliver received audio */
    aes67_audio_data_cb_t audio_cb;     /* Callback (when mode == CALLBACK) */
    void *audio_cb_user_data;           /* User data for callback */
} aes67_config_t;

/* Initialize a config struct with Kconfig defaults */
#define AES67_CONFIG_DEFAULT() { \
    .node_name = CONFIG_AES67_NODE_NAME, \
    .net = { \
        .interface_name = "ETH_0", \
        .eth_handle = NULL, \
        .rtp_mcast_base = CONFIG_AES67_RTP_MCAST_BASE, \
        .rtp_port = CONFIG_AES67_RTP_PORT, \
        .ttl = 64, \
        .dscp = 46, \
    }, \
    .ptp = { \
        .domain = CONFIG_AES67_PTP_DOMAIN, \
        .dscp = 46, \
        .prefer_grandmaster = true, \
    }, \
    .audio = { \
        .sample_rate = CONFIG_AES67_DEFAULT_SAMPLE_RATE, \
        .word_length = CONFIG_AES67_DEFAULT_WORD_LENGTH, \
        .packet_time_us = CONFIG_AES67_DEFAULT_PACKET_TIME_US, \
        .channels = CONFIG_AES67_DEFAULT_CHANNELS, \
        .codec = AES67_CODEC_L24, \
    }, \
    .i2s_pins = { \
        .mck_gpio = CONFIG_AES67_I2S_MCK_GPIO, \
        .bck_gpio = CONFIG_AES67_I2S_BCK_GPIO, \
        .ws_gpio = CONFIG_AES67_I2S_WS_GPIO, \
        .dout_gpio = CONFIG_AES67_I2S_DOUT_GPIO, \
        .din_gpio = CONFIG_AES67_I2S_DIN_GPIO, \
    }, \
    .jitter_buffer_ms = CONFIG_AES67_JITTER_BUFFER_MS, \
    .ring_buffer_ms = CONFIG_AES67_AUDIO_RING_BUFFER_MS, \
    .use_psram = true, \
    .rtp_rings_psram = false, \
    .sap_enabled = true, \
    .mdns_enabled = true, \
    .output_mode = AES67_OUTPUT_I2S, \
    .audio_cb = NULL, \
    .audio_cb_user_data = NULL, \
}

#ifdef __cplusplus
}
#endif
