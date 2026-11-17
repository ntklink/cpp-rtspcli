# rtspcli

Minimal C++17 RTSP client library, designed for embedding into other projects.

- **Transport**: RTSP handshake via libcurl (Basic/Digest auth, session keepalive), RTP over TCP interleaved
- **Payloads**: H264, H265/HEVC, AAC (mpeg4-generic / LATM), G.711 PCMU/PCMA, L16 PCM, MJPEG (RFC 2435 header reconstruction)
- **Output**: ready-to-decode frames — Annex-B video with SPS/PPS on key frames, AAC with ADTS headers, complete JPEG images
- **Dependency**: libcurl only (with rtsp protocol enabled)

## Quick start

```cpp
#include <rtspcli/rtspcli.hpp>

rtspcli::client c;
rtspcli::options o;
o.url = "rtsp://admin:12345@192.168.1.64:554/stream1";

c.on_frame([](const rtspcli::frame& f) {
  // f.data/f.size: one complete frame (valid only inside the callback)
  // f.pts_us:      microsecond timestamp, shared base across streams (A/V sync)
  // f.key_frame:   video key frame flag
});
c.on_error([](const std::string& why) { /* stream lost; reconnect elsewhere */ });

if (!c.open(o)) { /* c.last_error() */ }   // handshake; c.streams() now valid
if (!c.play())  { /* c.last_error() */ }   // start background receiving
// ...
c.stop();
```

`open()` exposes `streams()` (codec, clock rate, channels, SPS/PPS/AudioSpecificConfig)
before playback, so decoders can be initialized up front.

## Build & integrate

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

As a subdirectory or via FetchContent:

```cmake
add_subdirectory(cpp-rtspcli)
target_link_libraries(your_target PRIVATE rtspcli::rtspcli)
```

Cross-compile for RK3576 (buildroot toolchain, libcurl provided by the sysroot):

```bash
cmake -B build-rk3576 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/opt/rockchip/rk3576/host/share/buildroot/toolchainfile.cmake
cmake --build build-rk3576 -j
```

## Example

```bash
# pull for 10 s, write each track to dump_0.h264 / dump_1.aac / ...
./build/rtsp_dump rtsp://192.168.20.84/live_stream/7 10 dump
ffplay dump_0.h264
```

## Options

| Field | Default | Description |
|---|---|---|
| `url` | — | supports `rtsp://user:pass@host/path` |
| `username` / `password` | empty | credentials (override URL) |
| `timeout_ms` | 5000 | per-step handshake timeout |
| `idle_timeout_ms` | 10000 | no-data watchdog during playback |
| `video` / `audio` | true | select which media to pull |
| `aac_to_adts` | true | wrap AAC frames with ADTS headers |

## Notes

- Frame callbacks run on an internal thread; keep them light (enqueue, don't decode inline). Never call `stop()` from a callback.
- `interrupt()` is thread-safe and aborts a blocking `open()`/`play()` within about a second (e.g. for a fast shutdown); call `stop()` afterwards before reusing the client.
- Pull-only (PLAY), TCP interleaved only (no UDP). No PAUSE/seek/RECORD.
- Inter-stream sync uses the RTP-Info anchor from PLAY; RTCP SR is not parsed.
- L16 frames are 16-bit signed big-endian PCM as received from the wire.
