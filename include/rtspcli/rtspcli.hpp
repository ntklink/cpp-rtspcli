// rtspcli - minimal C++17 RTSP client library (RTP over TCP interleaved)
//
// Usage:
//   rtspcli::client c;
//   rtspcli::options o;
//   o.url = "rtsp://user:pass@192.168.1.64:554/stream1";
//   c.on_frame([](const rtspcli::frame& f) { /* decode/store; data valid only inside callback */ });
//   c.on_error([](const std::string& why) { /* stream lost, schedule reconnect */ });
//   if (!c.open(o) || !c.play()) { /* see c.last_error() */ }
//   ... c.stop();
//
// Threading: open()/play()/stop() run synchronously on the caller thread; frame
// callbacks come from an internal receive thread (a few early frames may be
// delivered on the thread calling play()). Never call stop() from a callback.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rtspcli {

enum class media_kind { video, audio };

enum class codec_type {
  h264,   // Annex-B stream, SPS/PPS prepended to key frames when missing
  h265,   // Annex-B stream, VPS/SPS/PPS prepended to key frames when missing
  mjpeg,  // each frame is a complete JPEG image
  aac,    // ADTS header added by default (options::aac_to_adts); raw LATM for MP4A-LATM
  pcmu,   // G.711 mu-law
  pcma,   // G.711 A-law
  l16,    // linear PCM, 16-bit signed, big-endian (network byte order)
  unknown,
};

const char* to_string(codec_type c);

struct stream_info {
  int index = -1;  // matches frame::stream_index
  media_kind kind = media_kind::video;
  codec_type codec = codec_type::unknown;
  std::string encoding_name;  // raw SDP encoding name, e.g. "H264", "MPEG4-GENERIC", "MP4A-LATM"
  int clock_rate = 0;         // RTP clock rate (90000 for video, sample rate for audio)
  int channels = 0;           // audio channel count, 0 for video
  // Out-of-band codec parameters:
  //   H264: Annex-B SPS+PPS;  H265: Annex-B VPS+SPS+PPS;
  //   AAC:  AudioSpecificConfig;  empty otherwise
  std::vector<uint8_t> codec_data;
};

struct frame {
  int stream_index = -1;
  media_kind kind = media_kind::video;
  codec_type codec = codec_type::unknown;
  const uint8_t* data = nullptr;  // valid only during the callback; copy to keep
  size_t size = 0;
  int64_t pts_us = 0;          // microseconds since playback start, shared across streams
  uint32_t rtp_timestamp = 0;  // raw RTP timestamp
  bool key_frame = false;      // video key frame; always true for audio
};

struct options {
  std::string url;                 // rtsp://[user:pass@]host[:port]/path
  std::string username, password;  // may also be embedded in the URL; these take precedence
  int timeout_ms = 5000;           // per-step handshake timeout
  int idle_timeout_ms = 10000;     // no data for this long during playback -> on_error
  bool video = true;               // request video streams
  bool audio = true;               // request audio streams
  bool aac_to_adts = true;         // wrap AAC frames with ADTS headers (self-contained)
  std::string user_agent = "rtspcli/1.0";
};

class client {
public:
  using frame_handler = std::function<void(const frame&)>;
  using error_handler = std::function<void(const std::string& reason)>;

  client();
  ~client();  // implies stop()
  client(client&&) noexcept;
  client& operator=(client&&) noexcept;
  client(const client&) = delete;
  client& operator=(const client&) = delete;

  // Set callbacks before play().
  void on_frame(frame_handler cb);
  void on_error(error_handler cb);

  // Synchronously performs OPTIONS/DESCRIBE/SETUP. streams() is valid on success,
  // so decoders can be initialized before playback starts.
  bool open(const options& opt);

  // Sends PLAY and starts the background receive thread.
  bool play();

  // TEARDOWN and stop the receive thread. Idempotent; never call from a callback.
  void stop();

  bool running() const;  // whether the receive thread is alive
  const std::vector<stream_info>& streams() const;
  std::string last_error() const;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace rtspcli
