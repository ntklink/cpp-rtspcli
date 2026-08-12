// rtsp_dump - pull a stream and write each media track to a file, for validation
//
//   rtsp_dump <rtsp-url> [seconds] [out-prefix]
//
// Output: <out-prefix>_<index>.<ext>  (h264/h265/mjpeg/aac/latm/pcmu/pcma/l16)
#include <rtspcli/rtspcli.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <string>
#include <thread>

static std::atomic<bool> g_quit{false};
static void on_sigint(int) { g_quit = true; }

static const char *file_ext(const rtspcli::stream_info &si)
{
  switch (si.codec)
  {
  case rtspcli::codec_type::h264:
    return "h264";
  case rtspcli::codec_type::h265:
    return "h265";
  case rtspcli::codec_type::mjpeg:
    return "mjpeg";
  case rtspcli::codec_type::aac:
    return si.encoding_name == "MP4A-LATM" ? "latm" : "aac";
  case rtspcli::codec_type::pcmu:
    return "pcmu";
  case rtspcli::codec_type::pcma:
    return "pcma";
  case rtspcli::codec_type::l16:
    return "l16";
  default:
    return "bin";
  }
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    std::fprintf(stderr, "usage: %s <rtsp-url> [seconds] [out-prefix]\n", argv[0]);
    return 1;
  }
  const std::string url = argv[1];
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 10;
  const std::string prefix = argc > 3 ? argv[3] : "dump";

  std::signal(SIGINT, on_sigint);

  rtspcli::client c;
  rtspcli::options o;
  o.url = url;

  struct stream_stat
  {
    FILE *fp = nullptr;
    uint64_t frames = 0, bytes = 0, keys = 0;
    int64_t last_pts = 0;
  };
  std::map<int, stream_stat> stats;

  c.on_frame([&](const rtspcli::frame &f)
             {
    auto& st = stats[f.stream_index];
    if (st.frames < 5)
      std::printf("  frame: stream=%d %s size=%zu pts=%.3fs key=%d\n", f.stream_index,
                  rtspcli::to_string(f.codec), f.size, f.pts_us / 1e6, f.key_frame);
    if (st.fp) std::fwrite(f.data, 1, f.size, st.fp);
    st.frames++;
    st.bytes += f.size;
    if (f.key_frame) st.keys++;
    st.last_pts = f.pts_us; });
  c.on_error([&](const std::string &why)
             {
    std::fprintf(stderr, "stream error: %s\n", why.c_str());
    g_quit = true; });

  std::printf("opening %s ...\n", url.c_str());
  if (!c.open(o))
  {
    std::fprintf(stderr, "open failed: %s\n", c.last_error().c_str());
    return 2;
  }
  for (const auto &si : c.streams())
  {
    std::printf("stream %d: %s %s codec=%s clock=%d ch=%d codec_data=%zuB\n", si.index,
                si.kind == rtspcli::media_kind::video ? "video" : "audio",
                si.encoding_name.c_str(), rtspcli::to_string(si.codec), si.clock_rate, si.channels,
                si.codec_data.size());
    char path[512];
    std::snprintf(path, sizeof(path), "%s_%d.%s", prefix.c_str(), si.index, file_ext(si));
    stats[si.index].fp = std::fopen(path, "wb");
    std::printf("  -> %s\n", path);
  }

  if (!c.play())
  {
    std::fprintf(stderr, "play failed: %s\n", c.last_error().c_str());
    return 3;
  }
  std::printf("playing for %d seconds (Ctrl+C to stop) ...\n", seconds);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!g_quit && c.running() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

  c.stop();
  for (auto &kv : stats)
  {
    auto &st = kv.second;
    if (st.fp)
      std::fclose(st.fp);
    std::printf("stream %d: %llu frames (%llu key), %.1f KB, last_pts=%.3fs\n", kv.first,
                (unsigned long long)st.frames, (unsigned long long)st.keys, st.bytes / 1024.0,
                st.last_pts / 1e6);
  }
  return 0;
}
