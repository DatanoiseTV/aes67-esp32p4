/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * SDP (Session Description Protocol) parser and generator for AES67.
 *
 * Generates and parses SDP documents conforming to AES67 / RAVENNA
 * requirements, including PTP reference clock and media clock attributes.
 */

#include "aes67_sdp.h"
#include "aes67_net.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

#include "esp_log.h"

static const char *TAG = "aes67_sdp";

const char *aes67_codec_to_str(aes67_codec_t codec)
{
    switch (codec) {
    case AES67_CODEC_L16:   return "L16";
    case AES67_CODEC_L24:   return "L24";
    case AES67_CODEC_L32:   return "L32";
    case AES67_CODEC_AM824: return "AM824";
    default:                return "L24";
    }
}

int aes67_codec_from_str(const char *name)
{
    if (!name) return -1;
    if (strcmp(name, "L16") == 0)   return AES67_CODEC_L16;
    if (strcmp(name, "L24") == 0)   return AES67_CODEC_L24;
    if (strcmp(name, "L32") == 0)   return AES67_CODEC_L32;
    if (strcmp(name, "AM824") == 0) return AES67_CODEC_AM824;
    return -1;
}

/* aes67_codec_word_length() is now inline in aes67_config.h */

int aes67_sdp_generate(const aes67_sdp_t *sdp, char *buf, size_t buf_len)
{
    if (!sdp || !buf || buf_len == 0) {
        return -1;
    }

    char origin_ip_str[16];
    char conn_ip_str[16];
    aes67_net_u32_to_ip(sdp->origin_ip, origin_ip_str, sizeof(origin_ip_str));
    aes67_net_u32_to_ip(sdp->connection_ip, conn_ip_str, sizeof(conn_ip_str));

    const char *codec_str = aes67_codec_to_str(sdp->codec);
    const char *username = sdp->origin_username[0] ? sdp->origin_username : "-";

    /* Build ptime string. AES67 uses fractional ms for sub-ms packet times. */
    char ptime_str[16];
    if (sdp->ptime_us >= 1000 && (sdp->ptime_us % 1000) == 0) {
        snprintf(ptime_str, sizeof(ptime_str), "%u", sdp->ptime_us / 1000);
    } else if (sdp->ptime_us >= 1000) {
        /* e.g. 1500us -> "1.5" */
        snprintf(ptime_str, sizeof(ptime_str), "%u.%03u",
                 sdp->ptime_us / 1000, sdp->ptime_us % 1000);
        /* Strip trailing zeros after decimal point */
        size_t len = strlen(ptime_str);
        while (len > 0 && ptime_str[len - 1] == '0') {
            ptime_str[--len] = '\0';
        }
    } else {
        /* Sub-millisecond: e.g. 125us -> "0.125" */
        snprintf(ptime_str, sizeof(ptime_str), "0.%03u", sdp->ptime_us);
        size_t len = strlen(ptime_str);
        while (len > 0 && ptime_str[len - 1] == '0') {
            ptime_str[--len] = '\0';
        }
    }

    /* Build PTP grandmaster ID string: XX-XX-XX-FF-FE-XX-XX-XX */
    char ptp_gm_str[24] = {0};
    if (sdp->has_ptp_refclk) {
        snprintf(ptp_gm_str, sizeof(ptp_gm_str),
                 "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X",
                 sdp->ptp_grandmaster_id[0], sdp->ptp_grandmaster_id[1],
                 sdp->ptp_grandmaster_id[2], sdp->ptp_grandmaster_id[3],
                 sdp->ptp_grandmaster_id[4], sdp->ptp_grandmaster_id[5],
                 sdp->ptp_grandmaster_id[6], sdp->ptp_grandmaster_id[7]);
    }

    /* Compute samples per packet for the framecount attribute */
    uint32_t framecount = (sdp->sample_rate * sdp->ptime_us) / 1000000;
    if (framecount == 0) framecount = 48;

    int written = snprintf(buf, buf_len,
        "v=0\r\n"
        "o=%s %" PRIu32 " %" PRIu32 " IN IP4 %s\r\n"
        "s=%s\r\n"
        "c=IN IP4 %s/%u\r\n"
        "t=0 0\r\n"
        "m=audio %u RTP/AVP %u\r\n"
        "i=Channels 1-%u\r\n"
        "a=sync-time:0\r\n"
        "a=framecount:%" PRIu32 "\r\n"
        "a=rtpmap:%u %s/%" PRIu32 "/%u\r\n"
        "a=ptime:%s\r\n"
        "a=%s\r\n",
        username, sdp->session_id, sdp->session_version, origin_ip_str,
        sdp->session_name,
        conn_ip_str, sdp->ttl,
        sdp->port, sdp->payload_type,
        sdp->channels,
        framecount,
        sdp->payload_type, codec_str, sdp->sample_rate, sdp->channels,
        ptime_str,
        /* Sources are announced with "recvonly", as Dante and Merging do in
         * their SAP announcements (the receiver's side of the session). */
        "recvonly");

    if (written < 0 || (size_t)written >= buf_len) {
        ESP_LOGE(TAG, "SDP buffer overflow during generate");
        return -1;
    }

    /* Append PTP reference clock attribute.
     * Use "traceable" when the GM is known and traceable,
     * otherwise include the specific GM identity. */
    if (sdp->has_ptp_refclk) {
        int n;
        if (sdp->ptp_traceable) {
            n = snprintf(buf + written, buf_len - written,
                         "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n");
        } else {
            n = snprintf(buf + written, buf_len - written,
                         "a=ts-refclk:ptp=IEEE1588-2008:%s:%u\r\n",
                         ptp_gm_str, sdp->ptp_domain);
        }
        if (n < 0 || (size_t)(written + n) >= buf_len) {
            return -1;
        }
        written += n;
    }

    /* Append media clock attribute */
    if (sdp->has_mediaclk) {
        int n = snprintf(buf + written, buf_len - written,
                         "a=mediaclk:direct=%" PRIu32 "\r\n",
                         sdp->mediaclk_offset);
        if (n < 0 || (size_t)(written + n) >= buf_len) {
            return -1;
        }
        written += n;
    }

    /* Append clock domain */
    {
        int n = snprintf(buf + written, buf_len - written,
                         "a=clock-domain:PTPv2 %u\r\n",
                         sdp->ptp_domain);
        if (n < 0 || (size_t)(written + n) >= buf_len) {
            return -1;
        }
        written += n;
    }

    return written;
}

/* Parse a single PTP grandmaster ID from "XX-XX-XX-XX-XX-XX-XX-XX" format */
static bool parse_ptp_gmid(const char *str, uint8_t gm[8])
{
    if (strlen(str) < 23) return false;

    unsigned int vals[8];
    int matched = sscanf(str, "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X",
                         &vals[0], &vals[1], &vals[2], &vals[3],
                         &vals[4], &vals[5], &vals[6], &vals[7]);
    if (matched != 8) return false;

    for (int i = 0; i < 8; i++) {
        gm[i] = (uint8_t)vals[i];
    }
    return true;
}

/* Parse ptime value which may be fractional (e.g. "1", "0.125", "1.5") */
static uint16_t parse_ptime_us(const char *str)
{
    /* Split on decimal point */
    const char *dot = strchr(str, '.');
    if (!dot) {
        /* Integer milliseconds */
        return (uint16_t)(atoi(str) * 1000);
    }

    /* Integer part in ms */
    int integer_ms = atoi(str);

    /* Fractional part - pad to 3 digits for microseconds */
    const char *frac = dot + 1;
    int frac_val = 0;
    int digits = 0;
    while (*frac && isdigit((unsigned char)*frac) && digits < 3) {
        frac_val = frac_val * 10 + (*frac - '0');
        frac++;
        digits++;
    }
    /* Pad remaining digits to get microseconds */
    while (digits < 3) {
        frac_val *= 10;
        digits++;
    }

    return (uint16_t)(integer_ms * 1000 + frac_val);
}

/* Parse a=rtpmap line, e.g. "98 L24/48000/2" */
static void parse_rtpmap(const char *val, aes67_sdp_t *sdp)
{
    unsigned int pt;
    char codec_name[16];
    unsigned int rate, ch;

    int matched = sscanf(val, "%u %15[^/]/%u/%u", &pt, codec_name, &rate, &ch);
    if (matched >= 3) {
        sdp->payload_type = (uint8_t)pt;
        int codec = aes67_codec_from_str(codec_name);
        if (codec >= 0) {
            sdp->codec = (aes67_codec_t)codec;
            sdp->word_length = aes67_codec_word_length(sdp->codec);
        }
        sdp->sample_rate = rate;
        sdp->channels = (matched >= 4) ? (uint8_t)ch : 1;
    }
}

/* Parse a=ts-refclk line, e.g. "ptp=IEEE1588-2008:XX-XX-...:0" */
static void parse_ts_refclk(const char *val, aes67_sdp_t *sdp)
{
    /* Check for PTP reference clock */
    if (strncmp(val, "ptp=IEEE1588-2008:", 18) != 0) {
        /* Could be "ptp=traceable" */
        if (strcmp(val, "ptp=traceable") == 0) {
            sdp->has_ptp_refclk = true;
            sdp->ptp_traceable = true;
        }
        return;
    }

    const char *gm_start = val + 18;
    if (parse_ptp_gmid(gm_start, sdp->ptp_grandmaster_id)) {
        sdp->has_ptp_refclk = true;
    }

    /* Parse domain after the GM ID: "XX-XX-XX-XX-XX-XX-XX-XX:0" */
    const char *colon = strchr(gm_start + 23, ':');
    if (colon) {
        sdp->ptp_domain = (uint8_t)atoi(colon + 1);
    }
}

/* Parse a=mediaclk line, e.g. "direct=0" */
static void parse_mediaclk(const char *val, aes67_sdp_t *sdp)
{
    if (strncmp(val, "direct=", 7) == 0) {
        sdp->has_mediaclk = true;
        sdp->mediaclk_offset = (uint32_t)strtoul(val + 7, NULL, 10);
    }
}

/* Parse origin line: "- 12345 1 IN IP4 192.168.1.10" */
static void parse_origin(const char *val, aes67_sdp_t *sdp)
{
    char username[32];
    unsigned int sid, sver;
    char net_type[4], addr_type[8], addr[16];

    int matched = sscanf(val, "%31s %u %u %3s %7s %15s",
                         username, &sid, &sver, net_type, addr_type, addr);
    if (matched >= 6) {
        strncpy(sdp->origin_username, username, sizeof(sdp->origin_username) - 1);
        sdp->origin_username[sizeof(sdp->origin_username) - 1] = '\0';
        sdp->session_id = sid;
        sdp->session_version = sver;
        sdp->origin_ip = aes67_net_ip_to_u32(addr);
    }
}

/* Parse connection line: "IN IP4 239.69.0.1/32" */
static void parse_connection(const char *val, aes67_sdp_t *sdp)
{
    char net_type[4], addr_type[8], addr_with_ttl[32];

    int matched = sscanf(val, "%3s %7s %31s", net_type, addr_type, addr_with_ttl);
    if (matched < 3) return;

    /* Split address and TTL at '/' */
    char *slash = strchr(addr_with_ttl, '/');
    if (slash) {
        *slash = '\0';
        sdp->ttl = (uint8_t)atoi(slash + 1);
    }
    sdp->connection_ip = aes67_net_ip_to_u32(addr_with_ttl);
}

/* Parse media line: "audio 5004 RTP/AVP 98" */
static void parse_media(const char *val, aes67_sdp_t *sdp)
{
    char media_type[16], transport[16];
    unsigned int port, pt;

    int matched = sscanf(val, "%15s %u %15s %u", media_type, &port, transport, &pt);
    if (matched >= 4 && strcmp(media_type, "audio") == 0) {
        sdp->port = (uint16_t)port;
        sdp->payload_type = (uint8_t)pt;
    }
}

/* Process a single attribute (a=) line */
static void parse_attribute(const char *val, aes67_sdp_t *sdp)
{
    if (strncmp(val, "rtpmap:", 7) == 0) {
        parse_rtpmap(val + 7, sdp);
    } else if (strncmp(val, "ptime:", 6) == 0) {
        sdp->ptime_us = parse_ptime_us(val + 6);
    } else if (strncmp(val, "ts-refclk:", 10) == 0) {
        parse_ts_refclk(val + 10, sdp);
    } else if (strncmp(val, "mediaclk:", 9) == 0) {
        parse_mediaclk(val + 9, sdp);
    }
}

esp_err_t aes67_sdp_parse(const char *sdp_text, aes67_sdp_t *sdp)
{
    if (!sdp_text || !sdp) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(sdp, 0, sizeof(aes67_sdp_t));

    /* Work on a mutable copy for line tokenization */
    size_t text_len = strlen(sdp_text);
    if (text_len == 0 || text_len >= AES67_SDP_MAX_LEN) {
        ESP_LOGE(TAG, "SDP text empty or too long (%u bytes)", (unsigned)text_len);
        return ESP_ERR_INVALID_ARG;
    }

    char buf[AES67_SDP_MAX_LEN];
    memcpy(buf, sdp_text, text_len + 1);

    /* Parse line by line. Handle both \r\n and \n line endings. */
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);

    while (line) {
        /* Strip trailing \r if present */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') {
            line[len - 1] = '\0';
            len--;
        }

        /* Each SDP line must be at least "X=Y" (3 chars) */
        if (len >= 2 && line[1] == '=') {
            const char *val = line + 2;
            switch (line[0]) {
            case 'o':
                parse_origin(val, sdp);
                break;
            case 's':
                strncpy(sdp->session_name, val, sizeof(sdp->session_name) - 1);
                sdp->session_name[sizeof(sdp->session_name) - 1] = '\0';
                break;
            case 'c':
                parse_connection(val, sdp);
                break;
            case 'm':
                parse_media(val, sdp);
                break;
            case 'a':
                parse_attribute(val, sdp);
                break;
            default:
                /* v=, t= and others we silently skip */
                break;
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* Sanity check: at minimum we need a port and sample rate */
    if (sdp->port == 0 || sdp->sample_rate == 0) {
        ESP_LOGW(TAG, "SDP parse incomplete: port=%u rate=%u",
                 sdp->port, sdp->sample_rate);
        return ESP_ERR_INVALID_ARG;
    }

    /* Derive word length from codec if not set */
    if (sdp->word_length == 0) {
        sdp->word_length = aes67_codec_word_length(sdp->codec);
    }

    ESP_LOGD(TAG, "Parsed SDP: %s, %s/%u/%u, port %u, ptime %u us",
             sdp->session_name, aes67_codec_to_str(sdp->codec),
             sdp->sample_rate, sdp->channels, sdp->port, sdp->ptime_us);

    return ESP_OK;
}
