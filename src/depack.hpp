#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rtp.hpp"
#include "rtspcli/rtspcli.hpp"

namespace rtspcli {
namespace detail {

struct out_frame {
  std::vector<uint8_t> data;
  uint32_t rtp_timestamp = 0;
  bool key = false;
};

class depacketizer {
public:
  virtual ~depacketizer() = default;
  // feed one RTP packet; complete frames are appended to out
  virtual void push(const rtp_packet& pkt, std::vector<out_frame>& out) = 0;
};

std::unique_ptr<depacketizer> create_depacketizer(const stream_info& info,
                                                  const std::map<std::string, std::string>& fmtp,
                                                  bool aac_to_adts);

}  // namespace detail
}  // namespace rtspcli
