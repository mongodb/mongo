//
// Copyright 2000 - 2003 Google Inc.
//
//

#include "base/logging.h"
#include "util/coding/coder.h"

// An initialization value used when we are allowed to 
unsigned char Encoder::kEmptyBuffer = 0;
const int Encoder::kVarintMax32 = Varint::kMax32;
const int Encoder::kVarintMax64 = Varint::kMax64;

Encoder::Encoder()
  : orig_(NULL),
    buf_(NULL),
    limit_(NULL),
    underlying_buffer_(&kEmptyBuffer) {
}
  
Encoder::~Encoder() {
  if (underlying_buffer_ != &kEmptyBuffer) {
    delete[] underlying_buffer_;
  }
}

int Encoder::varint32_length(uint32 v) {
  return Varint::Length32(v);
}

int Encoder::varint64_length(uint64 v) {
  return Varint::Length64(v);
}

void Encoder::EnsureSlowPath(int N) {
  CHECK(ensure_allowed());
  assert(avail() < N);
  assert(length() == 0 || orig_ == underlying_buffer_);

  // Double buffer size, but make sure we always have at least N extra bytes
  int current_len = length();
  int new_capacity = max(current_len + N, 2 * current_len);

  unsigned char* new_buffer = new unsigned char[new_capacity];
  memcpy(new_buffer, underlying_buffer_, current_len);
  if (underlying_buffer_ != &kEmptyBuffer) {
    delete[] underlying_buffer_;
  }
  underlying_buffer_ = new_buffer;

  orig_ = new_buffer;
  limit_ = new_buffer + new_capacity;
  buf_ = orig_ + current_len;
  CHECK(avail() >= N);
}

void Encoder::RemoveLast(int N) {
  CHECK(length() >= N);
  buf_ -= N;
}

void Encoder::Resize(int N) {
  CHECK(length() >= N);
  buf_ = orig_ + N;
  assert(length() == N);
}

// Special optimized version: does not use Varint
bool Decoder::get_varint32(uint32* v) {
  const char* r = Varint::Parse32WithLimit(
                                   reinterpret_cast<const char*>(buf_),
                                   reinterpret_cast<const char*>(limit_), v);
  if (r == NULL) { return false; }
  buf_ = reinterpret_cast<const unsigned char*>(r);
  return true;
}

// Special optimized version: does not use Varint
bool Decoder::get_varint64(uint64* v) {
  uint64 result = 0;

  if (buf_ + Varint::kMax64 <= limit_) {
    const char* r = Varint::Parse64(reinterpret_cast<const char*>(buf_), v);
    if (r == NULL) {
      return false;
    } else {
      buf_ = reinterpret_cast<const unsigned char*>(r);
      return true;
    }
  } else {
    int shift = 0;        // How much to shift next set of bits
    unsigned char byte;
    do {
      if ((shift >= 64) || (buf_ >= limit_)) {
        // Out of range
        return false;
      }
      
      // Get 7 bits from next byte
      byte = *(buf_++);
      result |= static_cast<uint64>(byte & 127) << shift;
      shift += 7;
    } while ((byte & 128) != 0);
    *v = result;
    return true;
  }
}
