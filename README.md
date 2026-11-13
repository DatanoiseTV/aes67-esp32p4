# AES67/RAVENNA for ESP32-P4

An ESP-IDF component implementing the AES67 (and RAVENNA-compatible) audio-over-IP standard on the ESP32-P4, based on patterns from the [aes67-linux-daemon](https://github.com/bondagit/aes67-linux-daemon) reference implementation.

## Performance

- **0.7ms end-to-end latency** with ptime=0.125ms source (6 frames at 48kHz)
- **100% DMA utilization** -- zero underruns at up to 8000 pps
- **0 packet loss** with IEEE 802.3x flow control
- Tested at 645,000+ packets with zero drops or sequence gaps
- ISR-driven playback at up to 3000Hz with zero task scheduling overhead

### Latency by Source Ptime

| Source ptime | Packets/sec | ESP32 latency | Total end-to-end |
|---|---|---|---|
| 0.125ms (6 frames) | 8000 | 0.4ms | ~0.7ms |
| 0.333ms (16 frames) | 3000 | 0.5ms | ~1.0ms |
| 1ms (48 frames) | 1000 | 0.6ms | ~1.7ms |
| 4ms (192 frames) | 250 | 0.6ms | ~2.7ms |

### Latency Breakdown (ptime=0.125ms, best case)

| Stage | Time |
|---|---|
| Source ptime buffering | 0.125ms |
| Network (LAN) | 0.1ms |
| Ethernet L2 hook (parse + convert) | 0.1ms |
| Stream buffer | 0.06ms |
| DMA ring (2 x 16 frames) | 0.33ms |
| **Total** | **~0.72ms** |

## Features

- **PTP (IEEE 1588v2)** clock synchronization using ESP32-P4 EMAC hardware timestamping
  - Slave mode: synchronizes to an external PTP grandmaster
  - Grandmaster mode: becomes PTP GM when no other master is present
  - PI controller with adaptive gain for fast lock and stable tracking
- **RTP audio streaming** with ISR-driven DMA playback
  - Source (TX) and Sink (RX) streams
  - L16, L24, L32, AM824 codec support with MTConvert function pointer dispatch
  - Channels: 1-8 per stream with channel routing support
  - Sample rates: 44.1kHz, 48kHz
  - Packet time: 0.125ms to 4ms (6 to 192 frames)
  - Handles 250 to 8000+ packets per second
  - DSCP/QoS marking for network prioritization
  - Ethernet MAC hook for L2 RTP interception (bypasses lwIP entirely)
  - IEEE 802.3x flow control prevents EMAC multicast frame drops
- **ISR-driven playback** -- zero task scheduling overhead
  - DMA ISR reads directly from stream buffer at up to 3000Hz
  - Sample-hold on underrun (smooth decay instead of silence clicks)
  - 2 DMA descriptors at 16 frames = 0.33ms ring latency
- **Flexible output routing** per node
  - I2S direct output to DAC (ISR-driven, default)
  - User callback with int32 sample buffers
  - FreeRTOS StreamBuffer for application-level processing
- **SAP (RFC 2974)** session announcement and discovery
  - Periodic multicast announcements for source streams
  - Remote source discovery with auto-subscribe callback
- **SDP** generation and parsing (AES67-compliant)
  - PTP reference clock attributes (`a=ts-refclk`)
  - Media clock attributes (`a=mediaclk`)
  - Fractional ptime support for sub-millisecond packet times
- **mDNS/DNS-SD** service advertisement (`_ravenna._udp`)
- **I2S audio** driver with APLL clock
  - Standard I2S Philips mode, 32-bit slots
  - APLL for exact 48kHz (18.432MHz MCLK)
  - Adaptive clock recovery via APLL SDM registers
- **Optimized for real-time**
  - -O2 compiler optimization
  - IRAM on all hot-path conversion functions
  - lwIP IRAM optimizations
  - IPv6 disabled (saves 39KB flash, reduces packet processing)
  - All tasks pinned to core 0 for cache coherency
  - Quiet logging: only reports errors and first lock

## Hardware

The example targets the **ESP32-P4-NANO** board with:

| Function | GPIO | Notes |
|---|---|---|
| Ethernet MDC | 31 | |
| Ethernet MDIO | 52 | |
| Ethernet REF_CLK | 50 | External 50MHz crystal |
| Ethernet PHY_RST | 51 | |
| I2S MCLK | 13 | 18.432MHz via APLL |
| I2S BCLK | 36 | External DAC (onboard: 12) |
| I2S LRCLK | 33 | External DAC (onboard: 10) |
| I2S DOUT | 32 | External DAC (onboard: 9) |
| I2S DIN | 11 | Codec ADC input |
| PA Enable | 53 | Active high |
| I2C SCL | 8 | ES8311 control |
| I2C SDA | 7 | ES8311 control |

- Ethernet PHY: IP101 GRI via RMII
- Onboard audio codec: ES8311 + NS4150B amplifier
- External I2S DAC supported (pin remapping in main.c)

## Build

Requires ESP-IDF v5.5+ with the ESP32-P4 target.

```bash
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

## Component Architecture

```
components/aes67/
  include/
    aes67.h             Top-level init/lifecycle API
    aes67_config.h      Configuration structs, stream params, codec utilities
    aes67_ptp.h         PTP clock synchronization
    aes67_rtp.h         RTP packet engine
    aes67_sap.h         SAP announcement/discovery
    aes67_sdp.h         SDP parser/generator
    aes67_session.h     Session manager (sources/sinks)
    aes67_audio.h       I2S audio driver with APLL and ISR playback
    aes67_convert.h     Sample format conversion (MTConvert pattern)
    aes67_net.h         Network utilities
  src/
    aes67.c             Node lifecycle and output routing
    aes67_ptp.c         PTP wrapping ptpd + esp_eth_time
    aes67_rtp.c         RTP engine with per-stream jitter buffers
    aes67_sap.c         SAP multicast announcements
    aes67_sdp.c         SDP text generation/parsing
    aes67_session.c     Stream orchestration
    aes67_audio.c       I2S DMA driver with ISR-driven playback
    aes67_convert.c     L16/L24/L32/AM824 format conversion (IRAM)
    aes67_net.c         Multicast socket helpers
    aes67_mdns.c        mDNS service registration
```

## Configuration

All parameters are configurable via `idf.py menuconfig` under "AES67 RAVENNA Component":

- Max sources/sinks (default: 4 each)
- Max channels per stream (default: 8)
- Sample rate (default: 48000 Hz)
- Packet time (default: 1000us)
- Word length / codec (default: 3 = L24)
- Default channel count (default: 2)
- RTP port (default: 5004)
- PTP domain (default: 0)
- Jitter buffer depth (default: 4ms)
- PSRAM usage for buffers (`use_psram`; `rtp_rings_psram` additionally moves the per-stream RTP rings, off by default)
- I2S pin assignments
- Task priorities

## Usage

```c
#include "aes67.h"
#include "aes67_session.h"

/* Configure and start */
aes67_config_t cfg = AES67_CONFIG_DEFAULT();
cfg.net.eth_handle = my_eth_handle;

aes67_node_handle_t node;
aes67_node_init(&cfg, &node);
aes67_node_start(node);

/* Add a 2-channel L24 source */
aes67_session_handle_t session;
aes67_node_get_session(node, &session);

uint8_t id;
aes67_session_add_source(session, "My Source", 2, AES67_CODEC_L24, &id);

/* Add a sink from discovered SDP */
aes67_session_add_sink(session, "My Sink", remote_sdp_text, &id);
```

### Callback output mode

```c
void my_audio_cb(const int32_t *samples, uint32_t frame_count,
                 uint8_t channels, uint8_t word_length,
                 uint32_t sample_rate, void *user_data) {
    /* Process int32 left-justified samples */
}

aes67_config_t cfg = AES67_CONFIG_DEFAULT();
cfg.output_mode = AES67_OUTPUT_CALLBACK;
cfg.audio_cb = my_audio_cb;
cfg.audio_cb_user_data = NULL;
```

## Data Flow

```
Playback (ISR-driven, default):
  Network -> Ethernet L2 hook -> convert L16/L24/L32 to int32
          -> StreamBuffer -> DMA ISR (up to 3000Hz) -> I2S -> DAC

  The DMA ISR fires when a descriptor finishes playing, reads from
  the stream buffer, and fills the next descriptor. Two descriptors
  alternate: one plays while the other is filled.

Capture:
  ADC -> I2S RX DMA -> capture ring buffer -> RTP TX -> network

Timing:
  PTP clock -> hardware timer ISR -> audio frame task (TIC) -> RTP TX
  APLL clock -> I2S DMA -> 48kHz sample-accurate playback
```

## Key Design Decisions

- **Ethernet L2 hook** intercepts RTP packets before lwIP, avoiding all TCP/IP stack overhead
- **ISR-driven playback** eliminates task scheduling jitter that caused 10% throughput loss with task-based approach
- **IEEE 802.3x flow control** prevents the ESP32-P4 EMAC from dropping multicast frames under load (8% loss without it)
- **Sample-hold on underrun** produces smooth decay instead of audible silence clicks
- **StreamBuffer byte-stream** handles any packet time (0.125ms to 4ms) transparently by accumulating bytes
- **All tasks on core 0** avoids cross-core L1 cache visibility issues on ESP32-P4 dual RISC-V
- **DMA descriptors sized to 16 frames** (0.33ms) balances latency vs stability across all ptimes
