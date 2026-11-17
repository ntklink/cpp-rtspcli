#include "sdp.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace rtspcli
{
  namespace detail
  {

    namespace
    {

      std::string trim(const std::string &s)
      {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
          return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
      }

      std::string to_upper(std::string s)
      {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                       { return std::toupper(c); });
        return s;
      }
      std::string to_lower(std::string s)
      {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                       { return std::tolower(c); });
        return s;
      }

      struct rtpmap_entry
      {
        std::string encoding;
        int clock_rate = 0;
        int channels = 0;
      };

      struct raw_media
      {
        std::string type;
        std::vector<int> pts;
        std::string control;
        std::map<int, rtpmap_entry> rtpmap;
        std::map<int, std::string> fmtp;
      };

      // "96 H264/90000" or "97 mpeg4-generic/44100/2"
      bool parse_rtpmap_line(const std::string &v, int &pt, rtpmap_entry &out)
      {
        std::istringstream is(v);
        std::string rest;
        if (!(is >> pt) || !(is >> rest))
          return false;
        size_t s1 = rest.find('/');
        out.encoding = to_upper(rest.substr(0, s1));
        if (s1 != std::string::npos)
        {
          size_t s2 = rest.find('/', s1 + 1);
          out.clock_rate = std::atoi(rest.substr(s1 + 1, s2 - s1 - 1).c_str());
          if (s2 != std::string::npos)
            out.channels = std::atoi(rest.substr(s2 + 1).c_str());
        }
        return true;
      }

      std::map<std::string, std::string> parse_fmtp_params(const std::string &s)
      {
        std::map<std::string, std::string> out;
        size_t pos = 0;
        while (pos < s.size())
        {
          size_t sep = s.find(';', pos);
          std::string item = trim(s.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos));
          if (!item.empty())
          {
            size_t eq = item.find('=');
            if (eq == std::string::npos)
              out[to_lower(trim(item))] = "";
            else
              out[to_lower(trim(item.substr(0, eq)))] = trim(item.substr(eq + 1));
          }
          if (sep == std::string::npos)
            break;
          pos = sep + 1;
        }
        return out;
      }

      bool known_encoding(const std::string &e)
      {
        return e == "H264" || e == "H265" || e == "HEVC" || e == "JPEG" || e == "MPEG4-GENERIC" ||
               e == "MP4A-LATM" || e == "PCMU" || e == "PCMA" || e == "L16";
      }

      sdp_media finalize(const raw_media &raw)
      {
        sdp_media m;
        m.type = raw.type;
        m.control = raw.control;
        // prefer a payload type we can handle
        int chosen = -1;
        for (int pt : raw.pts)
        {
          auto it = raw.rtpmap.find(pt);
          if (it != raw.rtpmap.end() && known_encoding(it->second.encoding))
          {
            chosen = pt;
            break;
          }
          if (it == raw.rtpmap.end() && (pt == 0 || pt == 8 || pt == 10 || pt == 11 || pt == 26))
          {
            chosen = pt;
            break;
          }
        }
        if (chosen < 0 && !raw.pts.empty())
          chosen = raw.pts.front();
        m.payload_type = chosen;
        auto it = raw.rtpmap.find(chosen);
        if (it != raw.rtpmap.end())
        {
          m.encoding_name = it->second.encoding;
          m.clock_rate = it->second.clock_rate;
          m.channels = it->second.channels;
        }
        auto fit = raw.fmtp.find(chosen);
        if (fit != raw.fmtp.end())
          m.fmtp = parse_fmtp_params(fit->second);
        return m;
      }

    } // namespace

    sdp_session parse_sdp(const std::string &text)
    {
      sdp_session sdp;
      std::vector<raw_media> medias;
      raw_media *cur = nullptr;

      size_t pos = 0;
      while (pos < text.size())
      {
        size_t eol = text.find('\n', pos);
        std::string line = trim(text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
        if (line.size() < 2 || line[1] != '=')
          continue;
        const char kind = line[0];
        const std::string val = line.substr(2);

        if (kind == 'm')
        {
          // m=video 0 RTP/AVP 96 97
          medias.emplace_back();
          cur = &medias.back();
          std::istringstream is(val);
          std::string proto;
          int port;
          is >> cur->type >> port >> proto;
          int pt;
          while (is >> pt)
            cur->pts.push_back(pt);
        }
        else if (kind == 'a')
        {
          size_t colon = val.find(':');
          std::string key = (colon == std::string::npos) ? val : val.substr(0, colon);
          std::string av = (colon == std::string::npos) ? "" : val.substr(colon + 1);
          if (key == "control")
          {
            if (cur)
              cur->control = trim(av);
            else
              sdp.session_control = trim(av);
          }
          else if (cur && key == "rtpmap")
          {
            int pt;
            rtpmap_entry rm;
            if (parse_rtpmap_line(av, pt, rm))
              cur->rtpmap[pt] = rm;
          }
          else if (cur && key == "fmtp")
          {
            std::istringstream is(av);
            int pt;
            if (is >> pt)
            {
              std::string rest;
              std::getline(is, rest);
              cur->fmtp[pt] = trim(rest);
            }
          }
        }
      }

      for (const auto &raw : medias)
        sdp.media.push_back(finalize(raw));
      return sdp;
    }

    std::vector<uint8_t> base64_decode(const std::string &s)
    {
      auto val = [](char c) -> int
      {
        if (c >= 'A' && c <= 'Z')
          return c - 'A';
        if (c >= 'a' && c <= 'z')
          return c - 'a' + 26;
        if (c >= '0' && c <= '9')
          return c - '0' + 52;
        if (c == '+')
          return 62;
        if (c == '/')
          return 63;
        return -1;
      };
      std::vector<uint8_t> out;
      uint32_t acc = 0; // only the low 14 bits are ever needed; masking keeps the shift defined
      int bits = 0;
      for (char c : s)
      {
        int v = val(c);
        if (v < 0)
        {
          if (c == '=')
            break;
          continue; // skip whitespace
        }
        acc = ((acc << 6) | uint32_t(v)) & 0x3FFF;
        bits += 6;
        if (bits >= 8)
        {
          bits -= 8;
          out.push_back(uint8_t((acc >> bits) & 0xFF));
        }
      }
      return out;
    }

    std::vector<uint8_t> hex_decode(const std::string &s)
    {
      auto val = [](char c) -> int
      {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      std::vector<uint8_t> out;
      int hi = -1;
      for (char c : s)
      {
        int v = val(c);
        if (v < 0)
          continue;
        if (hi < 0)
          hi = v;
        else
        {
          out.push_back(uint8_t((hi << 4) | v));
          hi = -1;
        }
      }
      return out;
    }

  } // namespace detail
} // namespace rtspcli
