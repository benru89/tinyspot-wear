#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Just enough protobuf for Spotify's HTTP APIs, without generated code.
namespace tinyspot::pb {

// Walks one message level. Length-delimited fields come out of next() as
// bytes; varints are available through lastVarint.
class Reader {
 public:
  explicit Reader(std::string_view msg)
      : p((const uint8_t*)msg.data()), end(p + msg.size()) {}

  // Returns false at the end or on malformed input.
  bool next(uint32_t& field, uint32_t& wire, std::string_view& bytes) {
    if (p >= end) return false;
    uint64_t key;
    if (!varint(key)) return false;
    field = (uint32_t)(key >> 3);
    wire = (uint32_t)(key & 7);
    switch (wire) {
      case 0: return varint(lastVarint);
      case 1: return (p += 8) <= end;
      case 5: return (p += 4) <= end;
      case 2: {
        uint64_t len;
        if (!varint(len) || len > (uint64_t)(end - p)) return false;
        bytes = std::string_view((const char*)p, len);
        p += len;
        return true;
      }
      default: return false;
    }
  }

  uint64_t lastVarint = 0;

 private:
  bool varint(uint64_t& out) {
    out = 0;
    for (int shift = 0; p < end && shift < 64; shift += 7) {
      uint8_t b = *p++;
      out |= (uint64_t)(b & 0x7f) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  }

  const uint8_t* p;
  const uint8_t* end;
};

// First length-delimited occurrence of a field, empty if absent.
inline std::string_view field(std::string_view msg, uint32_t wanted) {
  Reader r(msg);
  uint32_t f, w;
  std::string_view v;
  while (r.next(f, w, v)) {
    if (f == wanted && w == 2) return v;
  }
  return {};
}

// First varint occurrence of a field, 0 if absent.
inline uint64_t varintField(std::string_view msg, uint32_t wanted) {
  Reader r(msg);
  uint32_t f, w;
  std::string_view v;
  while (r.next(f, w, v)) {
    if (f == wanted && w == 0) return r.lastVarint;
  }
  return 0;
}

class Writer {
 public:
  Writer& varint(uint32_t field, uint64_t value) {
    raw((uint64_t)field << 3);
    raw(value);
    return *this;
  }
  Writer& bytes(uint32_t field, std::string_view value) {
    raw(((uint64_t)field << 3) | 2);
    raw(value.size());
    out.append(value.data(), value.size());
    return *this;
  }
  Writer& message(uint32_t field, const Writer& m) { return bytes(field, m.out); }

  std::string out;

 private:
  void raw(uint64_t v) {
    while (v >= 0x80) {
      out.push_back((char)(v | 0x80));
      v >>= 7;
    }
    out.push_back((char)v);
  }
};

}  // namespace tinyspot::pb
