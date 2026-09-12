#ifndef DOS_COMMON_WIRE_H_
#define DOS_COMMON_WIRE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Minimal big-endian encoding helpers plus a bounds-checked reader and a
// table-free CRC-32 (IEEE 802.3). Used by the append-only, CRC-framed on-disk
// formats (WAL, Raft log). Header-only so it can be shared without a link dep.
namespace dos {
namespace wire {

inline void PutU32(std::string& out, uint32_t v) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}

inline void PutU64(std::string& out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  }
}

inline void PutStr(std::string& out, std::string_view s) {
  PutU32(out, static_cast<uint32_t>(s.size()));
  out.append(s.data(), s.size());
}

// Standard CRC-32, computed on the fly (no static table, so there is no
// initialization-order or threading concern).
inline uint32_t Crc32(const void* data, std::size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= bytes[i];
    for (int b = 0; b < 8; ++b) {
      const uint32_t mask = -(crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

// Reads big-endian integers / length-prefixed strings from a buffer, refusing
// to read past the end.
class Reader {
public:
  Reader(const char* p, std::size_t n) : p_(p), n_(n) {}

  bool U32(uint32_t* v) {
    if (pos_ + 4 > n_) {
      return false;
    }
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) {
      r = (r << 8) | static_cast<uint8_t>(p_[pos_++]);
    }
    *v = r;
    return true;
  }
  bool U64(uint64_t* v) {
    if (pos_ + 8 > n_) {
      return false;
    }
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) {
      r = (r << 8) | static_cast<uint8_t>(p_[pos_++]);
    }
    *v = r;
    return true;
  }
  bool Str(std::string* s) {
    uint32_t len = 0;
    if (!U32(&len)) {
      return false;
    }
    if (pos_ + len > n_) {
      return false;
    }
    s->assign(p_ + pos_, len);
    pos_ += len;
    return true;
  }
  std::size_t pos() const { return pos_; }

private:
  const char* p_;
  std::size_t n_;
  std::size_t pos_ = 0;
};

} // namespace wire
} // namespace dos

#endif // DOS_COMMON_WIRE_H_
