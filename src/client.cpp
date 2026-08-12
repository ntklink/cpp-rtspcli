#include "rtspcli/rtspcli.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "depack.hpp"
#include "rtp.hpp"
#include "sdp.hpp"

namespace rtspcli {

using detail::depacketizer;
using detail::out_frame;
using detail::rtp_packet;
using detail::sdp_media;
using steady_clock = std::chrono::steady_clock;

const char* to_string(codec_type c) {
  switch (c) {
    case codec_type::h264: return "H264";
    case codec_type::h265: return "H265";
    case codec_type::mjpeg: return "MJPEG";
    case codec_type::aac: return "AAC";
    case codec_type::pcmu: return "PCMU";
    case codec_type::pcma: return "PCMA";
    case codec_type::l16: return "L16";
    default: return "Unknown";
  }
}

namespace {

std::once_flag g_curl_init;
void ensure_curl_init() {
  std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_ALL); });
}

bool iequals_n(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}

// fetch a header value by name (case-insensitive)
std::string header_value(const std::string& hdrs, const std::string& name) {
  size_t pos = 0;
  while (pos < hdrs.size()) {
    size_t eol = hdrs.find('\n', pos);
    if (eol == std::string::npos) eol = hdrs.size();
    size_t len = eol - pos;
    if (len > name.size() && hdrs[pos + name.size()] == ':' &&
        iequals_n(hdrs.c_str() + pos, name.c_str(), name.size())) {
      std::string v = hdrs.substr(pos + name.size() + 1, len - name.size() - 1);
      size_t a = v.find_first_not_of(" \t\r");
      size_t b = v.find_last_not_of(" \t\r");
      return (a == std::string::npos) ? std::string() : v.substr(a, b - a + 1);
    }
    pos = eol + 1;
  }
  return {};
}

// strip user:pass@ from the URL (request-line URIs must not carry credentials)
std::string strip_credentials(const std::string& url) {
  size_t p = url.find("://");
  if (p == std::string::npos) return url;
  size_t slash = url.find('/', p + 3);
  size_t limit = (slash == std::string::npos) ? url.size() : slash;
  size_t at = url.rfind('@', limit);
  if (at == std::string::npos || at < p + 3) return url;
  return url.substr(0, p + 3) + url.substr(at + 1);
}

// RFC 2326 control URL resolution: absolute as-is, "*"/empty -> base, else concatenate
std::string resolve_control(const std::string& base, const std::string& control) {
  if (control.empty() || control == "*") return base;
  if (control.rfind("rtsp://", 0) == 0 || control.rfind("rtsps://", 0) == 0) return control;
  if (!base.empty() && base.back() == '/') return base + control;
  return base + "/" + control;
}

std::string url_path(const std::string& url) {
  size_t p = url.find("://");
  if (p == std::string::npos) return url;
  size_t slash = url.find('/', p + 3);
  return (slash == std::string::npos) ? std::string() : url.substr(slash);
}

bool url_match(const std::string& a, const std::string& b) {
  if (a == b) return true;
  auto ends = [](const std::string& s, const std::string& t) {
    return !t.empty() && s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
  };
  if (ends(a, b) || ends(b, a)) return true;
  std::string pa = url_path(a), pb = url_path(b);
  return !pa.empty() && pa == pb;
}

}  // namespace

// ---------------------------------------------------------------------------

struct client::impl {
  struct stream_ctx {
    stream_info info;
    sdp_media sdp;
    std::string control_url;
    std::unique_ptr<depacketizer> depack;
    int ch_rtp = -1, ch_rtcp = -1;
    bool setup_ok = false;
    // PTS: 32-bit RTP timestamp unwrapping + RTP-Info anchor
    bool ts_init = false;
    int64_t ts_ext = 0;
    int64_t ts_anchor = 0;
    int64_t rtp_info_time = -1;
  };

  options opt;
  CURL* curl = nullptr;
  bool opened = false;
  std::vector<stream_ctx> streams;
  std::vector<stream_info> infos;
  std::array<int, 256> chan_map{};  // interleaved channel -> stream index

  frame_handler frame_cb;
  error_handler error_cb;
  std::thread thr;
  std::atomic<bool> stop_flag{false};
  std::atomic<bool> run_flag{false};

  mutable std::mutex err_mu;
  std::string err;

  std::string req_url;   // URL with credentials stripped
  std::string base_url;  // Content-Base
  std::string agg_url;   // aggregate control URL for PLAY/TEARDOWN
  std::string header_buf, body_buf;
  std::vector<uint8_t> ibuf;  // interleaved data reassembly buffer
  uint64_t ibuf_total = 0;    // total interleaved bytes received
  std::vector<out_frame> frames_tmp;  // reused by handle_channel to avoid churn
  bool has_get_parameter = false;
  int keepalive_ms = 25000;
  steady_clock::time_point last_data, last_keepalive;

  ~impl() { do_stop(); }

  void set_err(const std::string& e) {
    std::lock_guard<std::mutex> lk(err_mu);
    err = e;
  }
  std::string get_err() const {
    std::lock_guard<std::mutex> lk(err_mu);
    return err;
  }

  // ---- libcurl callbacks ----
  static size_t on_body(char* p, size_t s, size_t n, void* u) {
    static_cast<impl*>(u)->body_buf.append(p, s * n);
    return s * n;
  }
  static size_t on_header(char* p, size_t s, size_t n, void* u) {
    static_cast<impl*>(u)->header_buf.append(p, s * n);
    return s * n;
  }
  static size_t on_interleave(void* p, size_t s, size_t n, void* u) {
    auto* im = static_cast<impl*>(u);
    const uint8_t* d = static_cast<const uint8_t*>(p);
    im->ibuf_total += s * n;
    im->ibuf.insert(im->ibuf.end(), d, d + s * n);
    im->drain_interleaved();
    return s * n;
  }
  static int on_progress(void* u, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<impl*>(u)->stop_flag.load() ? 1 : 0;
  }

  CURLcode do_request(long req, const std::string& uri, long timeout_ms,
                      const char* transport = nullptr, const char* range = nullptr) {
    header_buf.clear();
    body_buf.clear();
    curl_easy_setopt(curl, CURLOPT_RTSP_STREAM_URI, uri.c_str());
    curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, req);
    curl_easy_setopt(curl, CURLOPT_RTSP_TRANSPORT, transport);
    curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    return curl_easy_perform(curl);
  }

  long status_code() {
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
  }

  // ---- interleaved ($-framed) data parsing ----
  void drain_interleaved() {
    size_t pos = 0;
    const size_t n = ibuf.size();
    while (n - pos >= 4) {
      if (ibuf[pos] != '$') {  // should not happen; defensive resync
        ++pos;
        continue;
      }
      const int chan = ibuf[pos + 1];
      const size_t len = (size_t(ibuf[pos + 2]) << 8) | ibuf[pos + 3];
      if (n - pos < 4 + len) break;
      handle_channel(chan, ibuf.data() + pos + 4, len);
      pos += 4 + len;
    }
    if (pos > 0) ibuf.erase(ibuf.begin(), ibuf.begin() + pos);
  }

  void handle_channel(int chan, const uint8_t* d, size_t n) {
    const int idx = chan_map[chan & 0xFF];
    if (idx < 0) return;
    stream_ctx& s = streams[size_t(idx)];
    if (chan != s.ch_rtp) return;  // RTCP not handled yet
    rtp_packet pkt;
    if (!detail::parse_rtp(d, n, pkt)) return;
    last_data = steady_clock::now();
    frames_tmp.clear();
    s.depack->push(pkt, frames_tmp);
    for (auto& f : frames_tmp) deliver(s, idx, f);
  }

  void deliver(stream_ctx& s, int idx, const out_frame& of) {
    // unwrap the 32-bit timestamp
    if (!s.ts_init) {
      s.ts_ext = int64_t(of.rtp_timestamp);
      s.ts_anchor = (s.rtp_info_time >= 0)
                        ? s.ts_ext + int32_t(uint32_t(s.rtp_info_time) - of.rtp_timestamp)
                        : s.ts_ext;
      s.ts_init = true;
    } else {
      s.ts_ext += int32_t(of.rtp_timestamp - uint32_t(s.ts_ext));
    }
    if (!frame_cb) return;
    frame f;
    f.stream_index = idx;
    f.kind = s.info.kind;
    f.codec = s.info.codec;
    f.data = of.data.data();
    f.size = of.data.size();
    f.rtp_timestamp = of.rtp_timestamp;
    f.key_frame = of.key;
    const int rate = s.info.clock_rate > 0 ? s.info.clock_rate : 90000;
    f.pts_us = (s.ts_ext - s.ts_anchor) * 1000000 / rate;
    frame_cb(f);
  }

  // ---- handshake ----
  bool fail(const std::string& msg) {
    set_err(msg);
    return false;
  }

  bool map_media(const sdp_media& m, stream_info& info) {
    if (m.type == "video") info.kind = media_kind::video;
    else if (m.type == "audio") info.kind = media_kind::audio;
    else return false;

    std::string enc = m.encoding_name;
    int clock = m.clock_rate, chans = m.channels;
    if (enc.empty()) {  // static payload types
      switch (m.payload_type) {
        case 0: enc = "PCMU"; clock = 8000; chans = 1; break;
        case 8: enc = "PCMA"; clock = 8000; chans = 1; break;
        case 10: enc = "L16"; clock = 44100; chans = 2; break;
        case 11: enc = "L16"; clock = 44100; chans = 1; break;
        case 26: enc = "JPEG"; clock = 90000; break;
        default: return false;
      }
    }
    info.encoding_name = enc;
    info.clock_rate = clock > 0 ? clock : (info.kind == media_kind::video ? 90000 : 8000);
    info.channels = chans > 0 ? chans : (info.kind == media_kind::audio ? 1 : 0);

    // comma-separated base64 NALs -> Annex-B
    auto sprop_annexb = [&](std::initializer_list<const char*> keys) {
      std::vector<uint8_t> blob;
      for (const char* k : keys) {
        auto it = m.fmtp.find(k);
        if (it == m.fmtp.end()) continue;
        const std::string& v = it->second;
        size_t pos = 0;
        while (pos <= v.size()) {
          size_t comma = v.find(',', pos);
          std::string part =
              v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
          auto nal = detail::base64_decode(part);
          if (!nal.empty()) {
            blob.insert(blob.end(), {0, 0, 0, 1});
            blob.insert(blob.end(), nal.begin(), nal.end());
          }
          if (comma == std::string::npos) break;
          pos = comma + 1;
        }
      }
      return blob;
    };

    if (enc == "H264") {
      info.codec = codec_type::h264;
      info.codec_data = sprop_annexb({"sprop-parameter-sets"});
    } else if (enc == "H265" || enc == "HEVC") {
      info.codec = codec_type::h265;
      info.codec_data = sprop_annexb({"sprop-vps", "sprop-sps", "sprop-pps"});
    } else if (enc == "JPEG") {
      info.codec = codec_type::mjpeg;
    } else if (enc == "MPEG4-GENERIC") {
      info.codec = codec_type::aac;
      auto it = m.fmtp.find("config");
      if (it != m.fmtp.end()) info.codec_data = detail::hex_decode(it->second);
    } else if (enc == "MP4A-LATM") {
      info.codec = codec_type::aac;
    } else if (enc == "PCMU") {
      info.codec = codec_type::pcmu;
    } else if (enc == "PCMA") {
      info.codec = codec_type::pcma;
    } else if (enc == "L16") {
      info.codec = codec_type::l16;
    } else {
      return false;
    }
    return true;
  }

  bool do_open(const options& o) {
    do_stop();
    ensure_curl_init();
    stop_flag = false;
    opt = o;
    set_err({});
    streams.clear();
    infos.clear();
    chan_map.fill(-1);
    ibuf.clear();

    req_url = strip_credentials(opt.url);
    curl = curl_easy_init();
    if (!curl) return fail("curl_easy_init failed");

    curl_easy_setopt(curl, CURLOPT_URL, opt.url.c_str());
    if (!opt.username.empty()) {
      curl_easy_setopt(curl, CURLOPT_USERNAME, opt.username.c_str());
      curl_easy_setopt(curl, CURLOPT_PASSWORD, opt.password.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)opt.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, opt.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(curl, CURLOPT_INTERLEAVEFUNCTION, &on_interleave);
    curl_easy_setopt(curl, CURLOPT_INTERLEAVEDATA, this);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // OPTIONS
    CURLcode rc = do_request(CURL_RTSPREQ_OPTIONS, req_url, opt.timeout_ms);
    if (rc != CURLE_OK) return fail(std::string("OPTIONS: ") + curl_easy_strerror(rc));
    has_get_parameter = header_value(header_buf, "Public").find("GET_PARAMETER") != std::string::npos;

    // DESCRIBE
    rc = do_request(CURL_RTSPREQ_DESCRIBE, req_url, opt.timeout_ms);
    if (rc != CURLE_OK) return fail(std::string("DESCRIBE: ") + curl_easy_strerror(rc));
    if (status_code() != 200) return fail("DESCRIBE: status " + std::to_string(status_code()));

    base_url = header_value(header_buf, "Content-Base");
    if (base_url.empty()) base_url = header_value(header_buf, "Content-Location");
    if (base_url.empty()) base_url = req_url;

    detail::sdp_session sdp = detail::parse_sdp(body_buf);
    agg_url = resolve_control(base_url, sdp.session_control);

    for (const auto& m : sdp.media) {
      stream_ctx s;
      if (!map_media(m, s.info)) continue;
      if (s.info.kind == media_kind::video && !opt.video) continue;
      if (s.info.kind == media_kind::audio && !opt.audio) continue;
      s.depack = detail::create_depacketizer(s.info, m.fmtp, opt.aac_to_adts);
      if (!s.depack) continue;
      s.sdp = m;
      s.control_url = resolve_control(agg_url, m.control);
      streams.push_back(std::move(s));
    }
    if (streams.empty()) return fail("no supported media in SDP");

    // SETUP: one pair of interleaved channels per stream
    int ch = 0;
    std::string setup_err;
    for (auto& s : streams) {
      char tr[64];
      std::snprintf(tr, sizeof(tr), "RTP/AVP/TCP;unicast;interleaved=%d-%d", ch, ch + 1);
      rc = do_request(CURL_RTSPREQ_SETUP, s.control_url, opt.timeout_ms, tr);
      if (rc != CURLE_OK || status_code() != 200) {
        setup_err = (rc != CURLE_OK) ? curl_easy_strerror(rc)
                                     : "status " + std::to_string(status_code());
        continue;
      }
      s.ch_rtp = ch;
      s.ch_rtcp = ch + 1;
      // the server may rewrite the channel numbers
      std::string trans = header_value(header_buf, "Transport");
      size_t ip = trans.find("interleaved=");
      if (ip != std::string::npos) {
        int a = -1, b = -1;
        if (std::sscanf(trans.c_str() + ip, "interleaved=%d-%d", &a, &b) >= 1 && a >= 0) {
          s.ch_rtp = a;
          s.ch_rtcp = (b >= 0) ? b : a + 1;
        }
      }
      // session timeout -> keepalive period
      std::string sess = header_value(header_buf, "Session");
      size_t tp = sess.find("timeout=");
      if (tp != std::string::npos) {
        int t = std::atoi(sess.c_str() + tp + 8);
        if (t > 0) keepalive_ms = std::max(2000, t * 1000 / 2);
      }
      s.setup_ok = true;
      ch += 2;
    }
    streams.erase(std::remove_if(streams.begin(), streams.end(),
                                 [](const stream_ctx& s) { return !s.setup_ok; }),
                  streams.end());
    if (streams.empty()) return fail("SETUP failed: " + setup_err);

    for (size_t i = 0; i < streams.size(); ++i) {
      streams[i].info.index = int(i);
      chan_map[streams[i].ch_rtp & 0xFF] = int(i);
      chan_map[streams[i].ch_rtcp & 0xFF] = int(i);
      infos.push_back(streams[i].info);
    }
    opened = true;
    return true;
  }

  bool do_play() {
    if (!opened) return fail("play() before successful open()");
    if (run_flag) return true;

    CURLcode rc = do_request(CURL_RTSPREQ_PLAY, agg_url, opt.timeout_ms, nullptr, "npt=0.000-");
    if (rc == CURLE_OK && status_code() != 200) {
      // some servers reject Range; retry once without it
      rc = do_request(CURL_RTSPREQ_PLAY, agg_url, opt.timeout_ms);
    }
    if (rc != CURLE_OK) return fail(std::string("PLAY: ") + curl_easy_strerror(rc));
    if (status_code() != 200) return fail("PLAY: status " + std::to_string(status_code()));

    // RTP-Info: per-stream rtptime anchors for inter-stream sync
    std::string ri = header_value(header_buf, "RTP-Info");
    size_t pos = 0, entry_idx = 0;
    while (pos < ri.size()) {
      size_t comma = ri.find(',', pos);
      std::string entry =
          ri.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      std::string url;
      int64_t rtptime = -1;
      size_t up = entry.find("url=");
      if (up != std::string::npos) {
        size_t end = entry.find(';', up);
        url = entry.substr(up + 4, end == std::string::npos ? std::string::npos : end - up - 4);
      }
      size_t rp = entry.find("rtptime=");
      if (rp != std::string::npos) rtptime = std::strtoll(entry.c_str() + rp + 8, nullptr, 10);
      if (rtptime >= 0) {
        bool matched = false;
        for (auto& s : streams) {
          if (!url.empty() && url_match(s.control_url, url)) {
            s.rtp_info_time = rtptime;
            matched = true;
            break;
          }
        }
        if (!matched && entry_idx < streams.size()) streams[entry_idx].rtp_info_time = rtptime;
      }
      ++entry_idx;
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }

    stop_flag = false;
    run_flag = true;
    last_data = last_keepalive = steady_clock::now();
    thr = std::thread([this] { run_loop(); });
    return true;
  }

  void run_loop() {
    std::string reason;
    int empty_spins = 0;
    while (!stop_flag) {
      const uint64_t before = ibuf_total;
      header_buf.clear();
      body_buf.clear();
      curl_easy_setopt(curl, CURLOPT_RTSP_REQUEST, (long)CURL_RTSPREQ_RECEIVE);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)opt.idle_timeout_ms);
      CURLcode rc = curl_easy_perform(curl);
      if (stop_flag) break;
      if (rc == CURLE_OPERATION_TIMEDOUT) {
        reason = "stream idle timeout (no data)";
        break;
      }
      if (rc == CURLE_ABORTED_BY_CALLBACK) break;
      if (rc != CURLE_OK) {
        reason = std::string("receive: ") + curl_easy_strerror(rc);
        break;
      }
      // defensive: back off briefly if spinning without data
      if (ibuf_total == before) {
        if (++empty_spins > 50) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          empty_spins = 0;
        }
      } else {
        empty_spins = 0;
      }

      const auto now = steady_clock::now();
      if (now - last_keepalive > std::chrono::milliseconds(keepalive_ms)) {
        last_keepalive = now;
        const long req = has_get_parameter ? CURL_RTSPREQ_GET_PARAMETER : CURL_RTSPREQ_OPTIONS;
        CURLcode krc = do_request(req, agg_url, 5000);
        if (krc != CURLE_OK && !stop_flag) {
          reason = std::string("keepalive: ") + curl_easy_strerror(krc);
          break;
        }
      }
    }
    run_flag = false;
    if (!reason.empty() && !stop_flag) {
      set_err(reason);
      if (error_cb) error_cb(reason);
    }
  }

  void do_stop() {
    stop_flag = true;
    if (thr.joinable()) {
      if (thr.get_id() == std::this_thread::get_id()) {
        thr.detach();  // called from the callback thread (forbidden by docs): avoid self-join
      } else {
        thr.join();
      }
    }
    if (curl) {
      stop_flag = false;  // otherwise the progress callback would abort TEARDOWN itself
      if (opened) do_request(CURL_RTSPREQ_TEARDOWN, agg_url, 3000);  // best effort
      curl_easy_cleanup(curl);
      curl = nullptr;
    }
    opened = false;
    run_flag = false;
  }
};

// ---------------------------------------------------------------------------

client::client() : impl_(new impl) {}
client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

void client::on_frame(frame_handler cb) { impl_->frame_cb = std::move(cb); }
void client::on_error(error_handler cb) { impl_->error_cb = std::move(cb); }

bool client::open(const options& opt) { return impl_->do_open(opt); }
bool client::play() { return impl_->do_play(); }
void client::stop() { impl_->do_stop(); }

bool client::running() const { return impl_->run_flag.load(); }
const std::vector<stream_info>& client::streams() const { return impl_->infos; }
std::string client::last_error() const { return impl_->get_err(); }

}  // namespace rtspcli
