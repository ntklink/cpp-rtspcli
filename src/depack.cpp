#include "depack.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace rtspcli
{
  namespace detail
  {

    namespace
    {

      void append_start_code(std::vector<uint8_t> &v)
      {
        static const uint8_t sc[4] = {0, 0, 0, 1};
        v.insert(v.end(), sc, sc + 4);
      }

      // iterate over each NAL in an Annex-B stream
      template <typename F>
      void for_each_nal(const std::vector<uint8_t> &buf, F &&fn)
      {
        size_t i = 0, n = buf.size();
        size_t start = std::string::npos;
        while (i + 3 <= n)
        {
          if (buf[i] == 0 && buf[i + 1] == 0 &&
              (buf[i + 2] == 1 || (i + 4 <= n && buf[i + 2] == 0 && buf[i + 3] == 1)))
          {
            size_t next = i + (buf[i + 2] == 1 ? 3 : 4);
            if (start != std::string::npos && i > start)
              fn(buf.data() + start, i - start);
            start = next;
            i = next;
          }
          else
          {
            ++i;
          }
        }
        if (start != std::string::npos && n > start)
          fn(buf.data() + start, n - start);
      }

      // Byte length of a leading access unit delimiter (start code included) in an
      // AU assembled by the depacketizers below, or 0 if the AU does not start
      // with one. Parameter sets must be inserted after it: an AUD has to be the
      // first NAL of its access unit. Only 4-byte start codes are ever written
      // into an AU, and emulation prevention rules out 00 00 00 01 inside a NAL.
      size_t leading_aud_size(const std::vector<uint8_t> &au, bool h265)
      {
        if (au.size() < 5)
          return 0;
        const bool aud = h265 ? ((au[4] >> 1) & 0x3F) == 35 : (au[4] & 0x1F) == 9;
        if (!aud)
          return 0;
        static const uint8_t sc[4] = {0, 0, 0, 1};
        auto it = std::search(au.begin() + 5, au.end(), sc, sc + 4);
        return size_t(it - au.begin());
      }

      // Prepends the cached parameter sets to an IRAP access unit (after its AUD).
      std::vector<uint8_t> with_param_sets(const std::vector<uint8_t> &au,
                                           const std::vector<uint8_t> &params, bool h265)
      {
        const size_t at = leading_aud_size(au, h265);
        std::vector<uint8_t> v;
        v.reserve(au.size() + params.size());
        v.insert(v.end(), au.begin(), au.begin() + at);
        v.insert(v.end(), params.begin(), params.end());
        v.insert(v.end(), au.begin() + at, au.end());
        return v;
      }

      struct bit_reader
      {
        const uint8_t *d;
        size_t nbits;
        size_t pos = 0;
        bit_reader(const uint8_t *data, size_t bytes) : d(data), nbits(bytes * 8) {}
        bool avail(size_t bits) const { return pos + bits <= nbits; }
        uint32_t read(int bits)
        {
          uint32_t v = 0;
          for (int i = 0; i < bits && pos < nbits; ++i, ++pos)
            v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1);
          return v;
        }
      };

      int fmtp_int(const std::map<std::string, std::string> &fmtp, const char *key, int dflt)
      {
        auto it = fmtp.find(key);
        if (it == fmtp.end() || it->second.empty())
          return dflt;
        return std::atoi(it->second.c_str());
      }

      // ---------------------------------------------------------------- H264 (RFC 6184)

      class h264_depack : public depacketizer
      {
      public:
        explicit h264_depack(const std::vector<uint8_t> &param_sets)
        {
          for_each_nal(param_sets, [this](const uint8_t *p, size_t n)
                       { cache_param(p, n); });
        }

        void push(const rtp_packet &pkt, std::vector<out_frame> &out) override
        {
          if (pkt.payload_size < 1)
            return;
          const bool gap = have_seq && uint16_t(pkt.seq - last_seq) != 1;
          last_seq = pkt.seq;
          have_seq = true;
          if (gap)
          {
            fu.clear();
            fu_active = false;
          }
          if (au_active && pkt.timestamp != au_ts)
          {
            // The previous AU never saw its marker packet, so the loss covers
            // at least its tail; it may also cover the head of this one.
            if (gap)
              damaged = true;
            flush(out);
          }
          if (gap)
            damaged = true;
          au_ts = pkt.timestamp;
          au_active = true;

          const uint8_t *p = pkt.payload;
          const size_t n = pkt.payload_size;
          const uint8_t t = p[0] & 0x1F;
          if (t >= 1 && t <= 23)
          {
            add_nal(p, n);
          }
          else if (t == 24)
          { // STAP-A
            size_t off = 1;
            while (off + 2 <= n)
            {
              size_t sz = (size_t(p[off]) << 8) | p[off + 1];
              off += 2;
              if (sz == 0 || off + sz > n)
                break;
              add_nal(p + off, sz);
              off += sz;
            }
          }
          else if (t == 28 && n >= 2)
          { // FU-A
            const uint8_t ind = p[0], hdr = p[1];
            const bool s_bit = hdr & 0x80, e_bit = hdr & 0x40;
            if (s_bit)
            {
              fu.clear();
              fu.push_back(uint8_t((ind & 0xE0) | (hdr & 0x1F)));
              fu.insert(fu.end(), p + 2, p + n);
              fu_active = true;
            }
            else if (fu_active)
            {
              fu.insert(fu.end(), p + 2, p + n);
            }
            if (e_bit && fu_active)
            {
              add_nal(fu.data(), fu.size());
              fu.clear();
              fu_active = false;
            }
          }
          if (pkt.marker)
            flush(out);
        }

      private:
        void add_nal(const uint8_t *p, size_t n)
        {
          if (n == 0)
            return;
          const uint8_t t = p[0] & 0x1F;
          if (t == 7 || t == 8)
            cache_param(p, n);
          if (t == 7)
            au_has_sps = true;
          if (t == 5)
            au_has_idr = true;
          append_start_code(au);
          au.insert(au.end(), p, p + n);
        }

        void cache_param(const uint8_t *p, size_t n)
        {
          if (n == 0)
            return;
          const uint8_t t = p[0] & 0x1F;
          if (t == 7)
            sps.assign(p, p + n);
          else if (t == 8)
            pps.assign(p, p + n);
          else
            return;
          param_blob.clear();
          for (const auto *v : {&sps, &pps})
          {
            if (v->empty())
              continue;
            append_start_code(param_blob);
            param_blob.insert(param_blob.end(), v->begin(), v->end());
          }
        }

        void flush(std::vector<out_frame> &out)
        {
          if (!au.empty() && !damaged)
          {
            out_frame f;
            f.rtp_timestamp = au_ts;
            f.key = au_has_idr;
            if (au_has_idr && !au_has_sps && !param_blob.empty())
              f.data = with_param_sets(au, param_blob, false);
            else
            {
              f.data = std::move(au);
            }
            out.push_back(std::move(f));
          }
          au.clear();
          au_has_idr = au_has_sps = damaged = au_active = false;
        }

        std::vector<uint8_t> au, fu, sps, pps, param_blob;
        uint32_t au_ts = 0;
        uint16_t last_seq = 0;
        bool have_seq = false, fu_active = false, au_active = false;
        bool au_has_idr = false, au_has_sps = false, damaged = false;
      };

      // ---------------------------------------------------------------- H265 (RFC 7798)

      class h265_depack : public depacketizer
      {
      public:
        explicit h265_depack(const std::vector<uint8_t> &param_sets)
        {
          for_each_nal(param_sets, [this](const uint8_t *p, size_t n)
                       { cache_param(p, n); });
        }

        void push(const rtp_packet &pkt, std::vector<out_frame> &out) override
        {
          if (pkt.payload_size < 2)
            return;
          const bool gap = have_seq && uint16_t(pkt.seq - last_seq) != 1;
          last_seq = pkt.seq;
          have_seq = true;
          if (gap)
          {
            fu.clear();
            fu_active = false;
          }
          if (au_active && pkt.timestamp != au_ts)
          {
            // The previous AU never saw its marker packet, so the loss covers
            // at least its tail; it may also cover the head of this one.
            if (gap)
              damaged = true;
            flush(out);
          }
          if (gap)
            damaged = true;
          au_ts = pkt.timestamp;
          au_active = true;

          const uint8_t *p = pkt.payload;
          const size_t n = pkt.payload_size;
          const uint8_t t = (p[0] >> 1) & 0x3F;
          if (t == 48)
          { // Aggregation Packet
            size_t off = 2;
            while (off + 2 <= n)
            {
              size_t sz = (size_t(p[off]) << 8) | p[off + 1];
              off += 2;
              if (sz == 0 || off + sz > n)
                break;
              add_nal(p + off, sz);
              off += sz;
            }
          }
          else if (t == 49 && n >= 3)
          { // Fragmentation Unit
            const uint8_t fuh = p[2];
            const bool s_bit = fuh & 0x80, e_bit = fuh & 0x40;
            const uint8_t ft = fuh & 0x3F;
            if (s_bit)
            {
              fu.clear();
              fu.push_back(uint8_t((p[0] & 0x81) | (ft << 1)));
              fu.push_back(p[1]);
              fu.insert(fu.end(), p + 3, p + n);
              fu_active = true;
            }
            else if (fu_active)
            {
              fu.insert(fu.end(), p + 3, p + n);
            }
            if (e_bit && fu_active)
            {
              add_nal(fu.data(), fu.size());
              fu.clear();
              fu_active = false;
            }
          }
          else if (t < 48)
          {
            add_nal(p, n);
          }
          if (pkt.marker)
            flush(out);
        }

      private:
        void add_nal(const uint8_t *p, size_t n)
        {
          if (n < 2)
            return;
          const uint8_t t = (p[0] >> 1) & 0x3F;
          if (t >= 32 && t <= 34)
            cache_param(p, n);
          if (t == 33)
            au_has_sps = true;
          if (t >= 16 && t <= 23)
            au_has_irap = true; // IRAP: BLA/IDR/CRA
          append_start_code(au);
          au.insert(au.end(), p, p + n);
        }

        void cache_param(const uint8_t *p, size_t n)
        {
          if (n < 2)
            return;
          const uint8_t t = (p[0] >> 1) & 0x3F;
          if (t == 32)
            vps.assign(p, p + n);
          else if (t == 33)
            sps.assign(p, p + n);
          else if (t == 34)
            pps.assign(p, p + n);
          else
            return;
          param_blob.clear();
          for (const auto *v : {&vps, &sps, &pps})
          {
            if (v->empty())
              continue;
            append_start_code(param_blob);
            param_blob.insert(param_blob.end(), v->begin(), v->end());
          }
        }

        void flush(std::vector<out_frame> &out)
        {
          if (!au.empty() && !damaged)
          {
            out_frame f;
            f.rtp_timestamp = au_ts;
            f.key = au_has_irap;
            if (au_has_irap && !au_has_sps && !param_blob.empty())
              f.data = with_param_sets(au, param_blob, true);
            else
            {
              f.data = std::move(au);
            }
            out.push_back(std::move(f));
          }
          au.clear();
          au_has_irap = au_has_sps = damaged = au_active = false;
        }

        std::vector<uint8_t> au, fu, vps, sps, pps, param_blob;
        uint32_t au_ts = 0;
        uint16_t last_seq = 0;
        bool have_seq = false, fu_active = false, au_active = false;
        bool au_has_irap = false, au_has_sps = false, damaged = false;
      };

      // ------------------------------------------------- AAC mpeg4-generic (RFC 3640)

      class aac_depack : public depacketizer
      {
      public:
        aac_depack(const std::map<std::string, std::string> &fmtp, const std::vector<uint8_t> &config,
                   int sample_rate, int channels, bool adts)
        {
          size_len = fmtp_int(fmtp, "sizelength", 13);
          index_len = fmtp_int(fmtp, "indexlength", 3);
          index_delta_len = fmtp_int(fmtp, "indexdeltalength", 3);
          parse_asc(config);
          if (!asc_valid)
            guess_asc(sample_rate, channels);
          use_adts = adts && asc_valid && aot >= 1 && aot <= 4 && freq_idx != 15;
        }

        void push(const rtp_packet &pkt, std::vector<out_frame> &out) override
        {
          const uint8_t *p = pkt.payload;
          const size_t n = pkt.payload_size;
          if (n < 2)
            return;
          const size_t hdr_bits = (size_t(p[0]) << 8) | p[1];
          const size_t hdr_bytes = (hdr_bits + 7) / 8;
          if (2 + hdr_bytes > n)
            return;

          std::vector<size_t> sizes;
          {
            bit_reader br(p + 2, hdr_bytes);
            size_t remain = hdr_bits;
            bool first = true;
            while (true)
            {
              const size_t idx_bits = first ? size_t(index_len) : size_t(index_delta_len);
              if (remain < size_t(size_len) + idx_bits)
                break;
              sizes.push_back(br.read(size_len));
              br.read(int(idx_bits));
              remain -= size_t(size_len) + idx_bits;
              first = false;
            }
          }
          const uint8_t *data = p + 2 + hdr_bytes;
          size_t dn = n - 2 - hdr_bytes;

          if (!pkt.marker)
          { // fragmented AU (one AU per packet, last fragment has marker=1)
            if (frag.empty())
              frag_ts = pkt.timestamp;
            frag.insert(frag.end(), data, data + dn);
            return;
          }
          if (!frag.empty())
          {
            frag.insert(frag.end(), data, data + dn);
            emit(frag.data(), frag.size(), frag_ts, out);
            frag.clear();
            return;
          }
          uint32_t ts = pkt.timestamp;
          if (sizes.empty())
          { // no AU headers (sizelength=0): the payload is a single AU
            emit(data, dn, ts, out);
            return;
          }
          size_t off = 0;
          for (size_t sz : sizes)
          {
            if (off >= dn)
              break;
            if (sz == 0 || off + sz > dn)
              sz = dn - off; // tolerate bad sizes, trust actual data
            emit(data + off, sz, ts, out);
            off += sz;
            ts += samples_per_frame;
          }
        }

      private:
        void parse_asc(const std::vector<uint8_t> &cfg)
        {
          if (cfg.empty())
            return;
          bit_reader br(cfg.data(), cfg.size());
          aot = int(br.read(5));
          if (aot == 31)
            aot = 32 + int(br.read(6));
          freq_idx = int(br.read(4));
          if (freq_idx == 15)
            br.read(24);
          chan_cfg = int(br.read(4));
          if (br.avail(1))
            samples_per_frame = br.read(1) ? 960 : 1024; // frameLengthFlag
          asc_valid = true;
        }

        // Some servers omit fmtp "config". Assume AAC-LC and take the sample rate
        // (RTP clock) and channel count from a=rtpmap, so ADTS still works.
        void guess_asc(int sample_rate, int channels)
        {
          static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000, 7350};
          for (int i = 0; i < 13; ++i)
          {
            // channel_configuration 1..6 is that many channels and 7 is 7.1 (8
            // channels); 7 channels has no configuration and needs a PCE.
            if (rates[i] == sample_rate && ((channels >= 1 && channels <= 6) || channels == 8))
            {
              aot = 2;
              freq_idx = i;
              chan_cfg = channels == 8 ? 7 : channels;
              asc_valid = true;
              return;
            }
          }
        }

        void emit(const uint8_t *d, size_t n, uint32_t ts, std::vector<out_frame> &out)
        {
          if (n == 0)
            return;
          out_frame f;
          f.rtp_timestamp = ts;
          f.key = true;
          if (use_adts)
          {
            const size_t len = n + 7;
            uint8_t h[7];
            h[0] = 0xFF;
            h[1] = 0xF1; // MPEG-4, no CRC
            h[2] = uint8_t((((aot - 1) & 3) << 6) | ((freq_idx & 0xF) << 2) | ((chan_cfg >> 2) & 1));
            h[3] = uint8_t(((chan_cfg & 3) << 6) | ((len >> 11) & 3));
            h[4] = uint8_t((len >> 3) & 0xFF);
            h[5] = uint8_t(((len & 7) << 5) | 0x1F);
            h[6] = 0xFC;
            f.data.assign(h, h + 7);
          }
          f.data.insert(f.data.end(), d, d + n);
          out.push_back(std::move(f));
        }

        int size_len = 13, index_len = 3, index_delta_len = 3;
        int aot = 2, freq_idx = 4, chan_cfg = 2;
        uint32_t samples_per_frame = 1024;
        bool asc_valid = false, use_adts = false;
        std::vector<uint8_t> frag;
        uint32_t frag_ts = 0;
      };

      // ------------------------------------------------------ AAC LATM (RFC 3016)

      class latm_depack : public depacketizer
      {
      public:
        void push(const rtp_packet &pkt, std::vector<out_frame> &out) override
        {
          if (buf.empty())
            ts = pkt.timestamp;
          buf.insert(buf.end(), pkt.payload, pkt.payload + pkt.payload_size);
          if (pkt.marker)
          {
            out.push_back({std::move(buf), ts, true});
            buf.clear();
          }
        }

      private:
        std::vector<uint8_t> buf;
        uint32_t ts = 0;
      };

      // ------------------------------------------------------ G.711 / raw audio

      class raw_audio_depack : public depacketizer
      {
      public:
        void push(const rtp_packet &pkt, std::vector<out_frame> &out) override
        {
          if (pkt.payload_size == 0)
            return;
          out_frame f;
          f.rtp_timestamp = pkt.timestamp;
          f.key = true;
          f.data.assign(pkt.payload, pkt.payload + pkt.payload_size);
          out.push_back(std::move(f));
        }
      };

      // ---------------------------------------------------------- MJPEG (RFC 2435)

      // standard JPEG quantization tables (ITU-T T.81 K.1/K.2), zigzag order
      const uint8_t jpeg_luma_quant[64] = {
          16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40,
          26, 24, 22, 22, 24, 49, 35, 37, 29, 40, 58, 51, 61, 60, 57, 51,
          56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56, 80, 109, 81, 87,
          95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99};
      const uint8_t jpeg_chroma_quant[64] = {
          17, 18, 18, 24, 21, 24, 47, 26, 26, 47, 99, 66, 56, 66, 99, 99,
          99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
          99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
          99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

      // standard JPEG Huffman tables (ITU-T T.81 K.3)
      const uint8_t lum_dc_lens[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
      const uint8_t lum_dc_syms[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
      const uint8_t lum_ac_lens[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
      const uint8_t lum_ac_syms[162] = {
          0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
          0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
          0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
          0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
          0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
          0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
          0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
          0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
          0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
          0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
          0xf9, 0xfa};
      const uint8_t chm_dc_lens[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
      const uint8_t chm_dc_syms[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
      const uint8_t chm_ac_lens[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
      const uint8_t chm_ac_syms[162] = {
          0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
          0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
          0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
          0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
          0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
          0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
          0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
          0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
          0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
          0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
          0xf9, 0xfa};

      void make_quant_tables(int q, uint8_t *lqt, uint8_t *cqt)
      {
        if (q < 1)
          q = 1;
        if (q > 99)
          q = 99;
        const int s = (q < 50) ? 5000 / q : 200 - q * 2;
        for (int i = 0; i < 64; ++i)
        {
          int l = (s * jpeg_luma_quant[i] + 50) / 100;
          int c = (s * jpeg_chroma_quant[i] + 50) / 100;
          lqt[i] = uint8_t(l < 1 ? 1 : (l > 255 ? 255 : l));
          cqt[i] = uint8_t(c < 1 ? 1 : (c > 255 ? 255 : c));
        }
      }

      void make_jpeg_header(std::vector<uint8_t> &o, int type, int w, int h,
                            const uint8_t *lqt, const uint8_t *cqt, uint16_t dri)
      {
        auto p8 = [&o](uint8_t v)
        { o.push_back(v); };
        auto p16 = [&o](uint16_t v)
        { o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v & 0xFF)); };
        auto marker = [&](uint8_t m)
        { p8(0xFF); p8(m); };
        auto dqt = [&](const uint8_t *t, uint8_t id)
        {
          marker(0xDB);
          p16(67);
          p8(id);
          o.insert(o.end(), t, t + 64);
        };
        auto dht = [&](const uint8_t *lens, const uint8_t *syms, size_t nsym, uint8_t cls, uint8_t id)
        {
          marker(0xC4);
          p16(uint16_t(3 + 16 + nsym));
          p8(uint8_t((cls << 4) | id));
          o.insert(o.end(), lens, lens + 16);
          o.insert(o.end(), syms, syms + nsym);
        };

        marker(0xD8); // SOI
        // APP0 / JFIF
        marker(0xE0);
        p16(16);
        p8('J');
        p8('F');
        p8('I');
        p8('F');
        p8(0);
        p8(1);
        p8(1);
        p8(0);
        p16(1);
        p16(1);
        p8(0);
        p8(0);
        dqt(lqt, 0);
        dqt(cqt, 1);
        if (dri)
        {
          marker(0xDD);
          p16(4);
          p16(dri);
        }
        // SOF0: 3 components, luma sampling from type (even=4:2:2, odd=4:2:0)
        marker(0xC0);
        p16(17);
        p8(8);
        p16(uint16_t(h));
        p16(uint16_t(w));
        p8(3);
        p8(1);
        p8((type & 1) ? 0x22 : 0x21);
        p8(0);
        p8(2);
        p8(0x11);
        p8(1);
        p8(3);
        p8(0x11);
        p8(1);
        dht(lum_dc_lens, lum_dc_syms, sizeof(lum_dc_syms), 0, 0);
        dht(lum_ac_lens, lum_ac_syms, sizeof(lum_ac_syms), 1, 0);
        dht(chm_dc_lens, chm_dc_syms, sizeof(chm_dc_syms), 0, 1);
        dht(chm_ac_lens, chm_ac_syms, sizeof(chm_ac_syms), 1, 1);
        // SOS
        marker(0xDA);
        p16(12);
        p8(3);
        p8(1);
        p8(0x00);
        p8(2);
        p8(0x11);
        p8(3);
        p8(0x11);
        p8(0);
        p8(63);
        p8(0);
      }

      class jpeg_depack : public depacketizer
      {
      public:
        void push(const rtp_packet &pkt, std::vector<out_frame> &out) override
        {
          const uint8_t *p = pkt.payload;
          const size_t n = pkt.payload_size;
          if (n < 8)
            return;
          const uint32_t off = (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
          uint8_t type = p[4];
          const uint8_t q = p[5];
          const int w = p[6] * 8, h = p[7] * 8;
          size_t hdr = 8;
          uint16_t dri = 0;
          if (type >= 64 && type <= 127)
          { // Restart Marker header
            if (n < hdr + 4)
              return;
            dri = rd16(p + hdr);
            hdr += 4;
            type = uint8_t(type - 64);
          }

          if (off == 0)
          {
            uint8_t lqt[64], cqt[64];
            if (q >= 128)
            { // in-band tables (Quantization Table header)
              if (n < hdr + 4)
                return;
              const uint16_t qlen = rd16(p + hdr + 2);
              hdr += 4;
              if (qlen > 0)
              {
                if (n < hdr + qlen)
                  return;
                if (qlen >= 128)
                {
                  std::memcpy(lqt, p + hdr, 64);
                  std::memcpy(cqt, p + hdr + 64, 64);
                }
                else if (qlen >= 64)
                {
                  std::memcpy(lqt, p + hdr, 64);
                  std::memcpy(cqt, p + hdr, 64);
                }
                else
                {
                  return;
                }
                hdr += qlen;
                std::memcpy(cached_lqt[q].data(), lqt, 64);
                std::memcpy(cached_cqt[q].data(), cqt, 64);
                has_cached[q] = true;
              }
              else
              { // qlen==0: reuse previously cached tables for this Q
                if (!has_cached[q])
                  return;
                std::memcpy(lqt, cached_lqt[q].data(), 64);
                std::memcpy(cqt, cached_cqt[q].data(), 64);
              }
            }
            else
            {
              make_quant_tables(q, lqt, cqt);
            }
            buf.clear();
            make_jpeg_header(buf, type, w, h, lqt, cqt, dri);
            valid = true;
            ts = pkt.timestamp;
            data_off = 0;
          }
          else
          {
            if (!valid || pkt.timestamp != ts || off != data_off)
            { // lost/misordered packet, drop frame
              valid = false;
              return;
            }
          }
          buf.insert(buf.end(), p + hdr, p + n);
          data_off = off + uint32_t(n - hdr);

          if (pkt.marker && valid)
          {
            const size_t fn = buf.size();
            if (fn < 2 || buf[fn - 2] != 0xFF || buf[fn - 1] != 0xD9)
            {
              buf.push_back(0xFF);
              buf.push_back(0xD9); // EOI
            }
            out.push_back({std::move(buf), ts, true});
            buf.clear();
            valid = false;
          }
        }

      private:
        std::vector<uint8_t> buf;
        uint32_t ts = 0, data_off = 0;
        bool valid = false;
        std::array<std::array<uint8_t, 64>, 256> cached_lqt{}, cached_cqt{};
        std::array<bool, 256> has_cached{};
      };

    } // namespace

    std::unique_ptr<depacketizer> create_depacketizer(const stream_info &info,
                                                      const std::map<std::string, std::string> &fmtp,
                                                      bool aac_to_adts)
    {
      switch (info.codec)
      {
      case codec_type::h264:
        return std::make_unique<h264_depack>(info.codec_data);
      case codec_type::h265:
        return std::make_unique<h265_depack>(info.codec_data);
      case codec_type::mjpeg:
        return std::make_unique<jpeg_depack>();
      case codec_type::aac:
        if (info.encoding_name == "MP4A-LATM")
          return std::make_unique<latm_depack>();
        return std::make_unique<aac_depack>(fmtp, info.codec_data, info.clock_rate, info.channels,
                                            aac_to_adts);
      case codec_type::pcmu:
      case codec_type::pcma:
      case codec_type::l16:
        return std::make_unique<raw_audio_depack>();
      default:
        return nullptr;
      }
    }

  } // namespace detail
} // namespace rtspcli
