# rtspcli

简约的 C++17 RTSP 客户端库，专为嵌入到其他项目而设计。

- **传输**：RTSP 握手基于 libcurl（自动处理 Basic/Digest 认证、会话管理、keepalive），RTP over TCP interleaved 收流（防火墙友好，IPC 摄像机普遍支持）
- **载荷**：H264 (RFC 6184)、H265/HEVC (RFC 7798)、AAC (RFC 3640 mpeg4-generic / MP4A-LATM)、G.711 PCMU/PCMA、L16 线性 PCM (16-bit 大端)、MJPEG (RFC 2435，自动重建 JPEG 头)
- **产出**：视频输出 Annex-B 完整帧（关键帧自动补 SPS/PPS），AAC 默认带 ADTS 头，MJPEG 输出完整 JPEG 图像——都可以直接喂给解码器或落盘
- **依赖**：仅 libcurl（须启用 rtsp 协议，Linux 发行版自带的都启用了）
- **接口**：单一头文件 `rtspcli/rtspcli.hpp`，pimpl 隔离实现，无第三方类型泄漏

## 快速上手

```cpp
#include <rtspcli/rtspcli.hpp>

rtspcli::client c;
rtspcli::options o;
o.url = "rtsp://admin:12345@192.168.1.64:554/stream1";  // 凭证可写在 URL 里

c.on_frame([](const rtspcli::frame& f) {
  // f.data/f.size: 一帧完整数据(仅回调期间有效, 需保留请拷贝)
  // f.pts_us:      微秒时间戳, 各路流之间可直接用于音画同步
  // f.key_frame:   视频关键帧标记
});
c.on_error([](const std::string& why) {
  // 断流/超时时回调一次, 可在这里安排重连(不要直接在回调里调 stop)
});

if (!c.open(o)) { /* 握手失败: c.last_error() */ }

for (const auto& si : c.streams()) {
  // open 成功后即可拿到流信息, 便于先初始化解码器:
  // si.codec / si.clock_rate / si.channels / si.codec_data(SPS/PPS 等带外参数)
}

if (!c.play()) { /* ... */ }
// ... 收流中, 帧从内部线程回调 ...
c.stop();  // TEARDOWN + 停线程; 析构时也会自动调用
```

## 集成到你的项目

作为子目录（或 git submodule）：

```cmake
add_subdirectory(cpp-rtspcli)
target_link_libraries(your_target PRIVATE rtspcli::rtspcli)
```

或 FetchContent：

```cmake
include(FetchContent)
FetchContent_Declare(rtspcli GIT_REPOSITORY <repo-url> GIT_TAG main)
FetchContent_MakeAvailable(rtspcli)
target_link_libraries(your_target PRIVATE rtspcli::rtspcli)
```

也支持 `cmake --install`，之后 `find_package(rtspcli)` 使用。

## 自带示例

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 拉流 10 秒, 各路媒体分别写入 dump_0.h264 / dump_1.aac ...
./build/rtsp_dump rtsp://192.168.20.84/live_stream/7 10 dump
ffplay dump_0.h264   # 直接可播
```

## API 说明

| 接口 | 说明 |
|---|---|
| `open(options)` | 同步完成 OPTIONS/DESCRIBE/SETUP，成功后 `streams()` 可用 |
| `play()` | 发送 PLAY，启动后台收流线程 |
| `stop()` | TEARDOWN 并停止线程，幂等 |
| `running()` | 收流线程是否存活（断流后变为 false） |
| `streams()` | 各路流的编码信息与带外参数 |
| `last_error()` | 最近一次错误描述 |

`options` 常用字段：

| 字段 | 默认 | 说明 |
|---|---|---|
| `url` | — | 支持 `rtsp://user:pass@host/path` |
| `username`/`password` | 空 | 单独指定凭证（优先于 URL） |
| `timeout_ms` | 5000 | 握手每步超时 |
| `idle_timeout_ms` | 10000 | 播放中持续无数据判定断流 |
| `video`/`audio` | true | 按需拉流（如只要视频） |
| `aac_to_adts` | true | AAC 帧加 ADTS 头（可直接存 .aac / 喂解码器） |

## 线程模型与注意事项

- `open/play/stop` 在调用者线程同步执行；帧回调来自内部收流线程，回调里做重活会阻塞收流，建议只做入队。
- 不要在回调内调用 `stop()`；断流后在自己的线程里 `stop()` + 重新 `open()` 即可实现重连。
- `frame::data` 仅回调期间有效。
- PTS 基于 PLAY 响应的 RTP-Info 基准计算，各路流共用起点，可直接做音画同步；无 RTP-Info 时以各流首包为 0。

## 范围与限制

- 仅拉流（PLAY），不支持推流（RECORD/ANNOUNCE）与 PAUSE/seek。
- 传输固定为 RTP over TCP interleaved，不支持 UDP（IPC 场景 TCP 更可靠，无丢包乱序问题）。
- 音画同步精度依赖服务器的 RTP-Info；未解析 RTCP SR。
- MP4A-LATM 音频按完整 audioMuxElement 原样输出（可喂 ffmpeg 的 `aac_latm` 解码器），不做 LATM 解复用。

## 已验证

- 真机 IPC（H264 1080p + AAC-LC 16kHz；L16 16kHz 纯音频流，Digest 认证）
- mediamtx：H265+PCMU、MJPEG（含量化表重建）、Digest 认证（含错误密码 401 路径）
- 落盘裸流均通过 `ffmpeg -f null` 零错误解码；70 秒长跑零丢帧、保活正常
