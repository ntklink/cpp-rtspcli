#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rtspcli {
namespace detail {

struct sdp_media {
  std::string type;           // "video" / "audio"
  int payload_type = -1;      // selected payload type
  std::string encoding_name;  // upper-case, e.g. "H264"; may be empty for static PTs
  int clock_rate = 0;
  int channels = 0;
  std::string control;                      // a=control (may be relative)
  std::map<std::string, std::string> fmtp;  // fmtp key/value pairs of the selected PT (keys lower-case)
};

struct sdp_session {
  std::string session_control;  // session-level a=control
  std::vector<sdp_media> media;
};

sdp_session parse_sdp(const std::string& text);

std::vector<uint8_t> base64_decode(const std::string& s);
std::vector<uint8_t> hex_decode(const std::string& s);

}  // namespace detail
}  // namespace rtspcli
