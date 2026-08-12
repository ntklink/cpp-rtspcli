#pragma once

#include <cstddef>
#include <cstdint>

namespace rtspcli {
namespace detail {

struct rtp_packet {
  uint8_t payload_type = 0;
  bool marker = false;
  uint16_t seq = 0;
  uint32_t timestamp = 0;
  uint32_t ssrc = 0;
  const uint8_t* payload = nullptr;
  size_t payload_size = 0;
};

inline uint32_t rd32(const uint8_t* d) {
  return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
}
inline uint16_t rd16(const uint8_t* d) { return uint16_t((d[0] << 8) | d[1]); }

inline bool parse_rtp(const uint8_t* d, size_t n, rtp_packet& out) {
  if (n < 12 || (d[0] >> 6) != 2) return false;
  const bool pad = d[0] & 0x20, ext = d[0] & 0x10;
  const int cc = d[0] & 0x0F;
  out.marker = d[1] & 0x80;
  out.payload_type = d[1] & 0x7F;
  out.seq = rd16(d + 2);
  out.timestamp = rd32(d + 4);
  out.ssrc = rd32(d + 8);
  size_t off = 12 + size_t(cc) * 4;
  if (off > n) return false;
  if (ext) {
    if (off + 4 > n) return false;
    off += 4 + size_t(rd16(d + off + 2)) * 4;
    if (off > n) return false;
  }
  size_t end = n;
  if (pad) {
    const uint8_t p = d[n - 1];
    if (p == 0 || off + p > n) return false;
    end = n - p;
  }
  out.payload = d + off;
  out.payload_size = end - off;
  return true;
}

}  // namespace detail
}  // namespace rtspcli
