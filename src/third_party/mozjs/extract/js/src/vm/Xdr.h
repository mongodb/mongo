/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Xdr_h
#define vm_Xdr_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_CRASH
#include "mozilla/MaybeOneOf.h"  // mozilla::MaybeOneOf
#include "mozilla/Try.h"         // MOZ_TRY
#include "mozilla/Utf8.h"        // mozilla::Utf8Unit

#include <stddef.h>     // size_t
#include <stdint.h>     // uint8_t, uint16_t, uint32_t, uint64_t
#include <string.h>     // memcpy
#include <type_traits>  // std::enable_if_t

#include "js/AllocPolicy.h"  // ReportOutOfMemory
#include "js/Transcoding.h"  // JS::TranscodeResult, JS::TranscodeBuffer, JS::TranscodeRange, IsTranscodingBytecodeAligned, IsTranscodingBytecodeOffsetAligned
#include "js/TypeDecls.h"    // JS::Latin1Char
#include "js/UniquePtr.h"    // UniquePtr
#include "js/Utility.h"      // JS::FreePolicy

struct JSContext;

namespace js {

enum XDRMode { XDR_ENCODE, XDR_DECODE };

template <typename T>
using XDRResultT = mozilla::Result<T, JS::TranscodeResult>;
using XDRResult = XDRResultT<mozilla::Ok>;

class XDRBufferBase {
 public:
  explicit XDRBufferBase(FrontendContext* fc, size_t cursor = 0)
      : fc_(fc), cursor_(cursor) {}

  FrontendContext* fc() const { return fc_; }

  size_t cursor() const { return cursor_; }

 protected:
  FrontendContext* const fc_;
  size_t cursor_;
};

template <XDRMode mode>
class XDRBuffer;

template <>
class XDRBuffer<XDR_ENCODE> : public XDRBufferBase {
 public:
  XDRBuffer(FrontendContext* fc, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRBufferBase(fc, cursor), buffer_(buffer) {}

  uint8_t* write(size_t n) {
    MOZ_ASSERT(n != 0);
    if (!buffer_.growByUninitialized(n)) {
      ReportOutOfMemory(fc());
      return nullptr;
    }
    uint8_t* ptr = &buffer_[cursor_];
    cursor_ += n;
    return ptr;
  }

  bool align32() {
    size_t extra = cursor_ % 4;
    if (extra) {
      size_t padding = 4 - extra;
      if (!buffer_.appendN(0, padding)) {
        ReportOutOfMemory(fc());
        return false;
      }
      cursor_ += padding;
    }
    return true;
  }

  bool isAligned32() { return cursor_ % 4 == 0; }

  const uint8_t* read(size_t n) {
    MOZ_CRASH("Should never read in encode mode");
    return nullptr;
  }

  const uint8_t* peek(size_t n) {
    MOZ_CRASH("Should never read in encode mode");
    return nullptr;
  }

  uint8_t* bufferAt(size_t cursor) {
    MOZ_ASSERT(cursor < buffer_.length());
    return &buffer_[cursor];
  }

 private:
  JS::TranscodeBuffer& buffer_;
};

template <>
class XDRBuffer<XDR_DECODE> : public XDRBufferBase {
 public:
  XDRBuffer(FrontendContext* fc, const JS::TranscodeRange& range)
      : XDRBufferBase(fc), buffer_(range) {}

  // This isn't used by XDRStencilDecoder.
  // Defined just for XDRState, shared with XDRStencilEncoder.
  XDRBuffer(FrontendContext* fc, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRBufferBase(fc, cursor), buffer_(buffer.begin(), buffer.length()) {}

  bool align32() {
    size_t extra = cursor_ % 4;
    if (extra) {
      size_t padding = 4 - extra;
      cursor_ += padding;

      // Don't let buggy code read past our buffer
      if (cursor_ > buffer_.length()) {
        return false;
      }
    }
    return true;
  }

  bool isAligned32() { return cursor_ % 4 == 0; }

  const uint8_t* read(size_t n) {
    MOZ_ASSERT(cursor_ < buffer_.length());
    const uint8_t* ptr = &buffer_[cursor_];
    cursor_ += n;

    // Don't let buggy code read past our buffer
    if (cursor_ > buffer_.length()) {
      return nullptr;
    }

    return ptr;
  }

  const uint8_t* peek(size_t n) {
    MOZ_ASSERT(cursor_ < buffer_.length());
    const uint8_t* ptr = &buffer_[cursor_];

    // Don't let buggy code read past our buffer
    if (cursor_ + n > buffer_.length()) {
      return nullptr;
    }

    return ptr;
  }

  uint8_t* write(size_t n) {
    MOZ_CRASH("Should never write in decode mode");
    return nullptr;
  }

 private:
  const JS::TranscodeRange buffer_;
};

template <typename CharT>
using XDRTranscodeString =
    mozilla::MaybeOneOf<const CharT*, js::UniquePtr<CharT[], JS::FreePolicy>>;

class XDRCoderBase {
 private:
#ifdef DEBUG
  JS::TranscodeResult resultCode_;
#endif

 protected:
  XDRCoderBase()
#ifdef DEBUG
      : resultCode_(JS::TranscodeResult::Ok)
#endif
  {
  }

 public:
#ifdef DEBUG
  // Record logical failures of XDR.
  JS::TranscodeResult resultCode() const { return resultCode_; }
  void setResultCode(JS::TranscodeResult code) {
    MOZ_ASSERT(resultCode() == JS::TranscodeResult::Ok);
    resultCode_ = code;
  }
  bool validateResultCode(FrontendContext* fc, JS::TranscodeResult code) const;
#endif
};

/*
 * XDR serialization state.  All data is encoded in native endian, except
 * bytecode.
 */
template <XDRMode mode>
class XDRState : public XDRCoderBase {
 protected:
  XDRBuffer<mode> mainBuf;
  XDRBuffer<mode>* buf;

 public:
  XDRState(FrontendContext* fc, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : mainBuf(fc, buffer, cursor), buf(&mainBuf) {}

  template <typename RangeType>
  XDRState(FrontendContext* fc, const RangeType& range)
      : mainBuf(fc, range), buf(&mainBuf) {}

  // No default copy constructor or copying assignment, because |buf|
  // is an internal pointer.
  XDRState(const XDRState&) = delete;
  XDRState& operator=(const XDRState&) = delete;

  ~XDRState() = default;

  FrontendContext* fc() const { return mainBuf.fc(); }

  template <typename T = mozilla::Ok>
  XDRResultT<T> fail(JS::TranscodeResult code) {
#ifdef DEBUG
    MOZ_ASSERT(code != JS::TranscodeResult::Ok);
    MOZ_ASSERT(validateResultCode(fc(), code));
    setResultCode(code);
#endif
    return mozilla::Err(code);
  }

  XDRResult align32() {
    if (!buf->align32()) {
      return fail(JS::TranscodeResult::Throw);
    }
    return mozilla::Ok();
  }

  bool isAligned32() { return buf->isAligned32(); }

  XDRResult readData(const uint8_t** pptr, size_t length) {
    const uint8_t* ptr = buf->read(length);
    if (!ptr) {
      return fail(JS::TranscodeResult::Failure_BadDecode);
    }
    *pptr = ptr;
    return mozilla::Ok();
  }

  // Peek the `sizeof(T)` bytes and return the pointer to `*pptr`.
  // The caller is responsible for aligning the buffer by calling `align32`.
  template <typename T>
  XDRResult peekData(const T** pptr) {
    static_assert(alignof(T) <= 4);
    MOZ_ASSERT(isAligned32());
    const uint8_t* ptr = buf->peek(sizeof(T));
    if (!ptr) {
      return fail(JS::TranscodeResult::Failure_BadDecode);
    }
    *pptr = reinterpret_cast<const T*>(ptr);
    return mozilla::Ok();
  }

  // Peek uint32_t data.
  XDRResult peekUint32(uint32_t* n) {
    MOZ_ASSERT(mode == XDR_DECODE);
    const uint8_t* ptr = buf->peek(sizeof(*n));
    if (!ptr) {
      return fail(JS::TranscodeResult::Failure_BadDecode);
    }
    *n = *reinterpret_cast<const uint32_t*>(ptr);
    return mozilla::Ok();
  }

  XDRResult codeUint8(uint8_t* n) {
    if (mode == XDR_ENCODE) {
      uint8_t* ptr = buf->write(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult::Throw);
      }
      *ptr = *n;
    } else {
      const uint8_t* ptr = buf->read(sizeof(*n));
      if (!ptr) {
        return fail(JS::TranscodeResult::Failure_BadDecode);
      }
      *n = *ptr;
    }
    return mozilla::Ok();
  }

 private:
  template <typename T>
  XDRResult codeUintImpl(T* n) {
    if (mode == XDR_ENCODE) {
      uint8_t* ptr = buf->write(sizeof(T));
      if (!ptr) {
        return fail(JS::TranscodeResult::Throw);
      }
      memcpy(ptr, n, sizeof(T));
    } else {
      const uint8_t* ptr = buf->read(sizeof(T));
      if (!ptr) {
        return fail(JS::TranscodeResult::Failure_BadDecode);
      }
      memcpy(n, ptr, sizeof(T));
    }
    return mozilla::Ok();
  }

 public:
  XDRResult codeUint16(uint16_t* n) { return codeUintImpl(n); }

  XDRResult codeUint32(uint32_t* n) { return codeUintImpl(n); }

  XDRResult codeUint64(uint64_t* n) { return codeUintImpl(n); }

  void codeUint32At(uint32_t* n, size_t cursor) {
    if constexpr (mode == XDR_ENCODE) {
      uint8_t* ptr = buf->bufferAt(cursor);
      memcpy(ptr, n, sizeof(uint32_t));
    } else {
      MOZ_CRASH("not supported.");
    }
  }

  const uint8_t* bufferAt(size_t cursor) const {
    if constexpr (mode == XDR_ENCODE) {
      return buf->bufferAt(cursor);
    }

    MOZ_CRASH("not supported.");
  }

  XDRResult peekArray(size_t n, const uint8_t** p) {
    if constexpr (mode == XDR_DECODE) {
      const uint8_t* ptr = buf->peek(n);
      if (!ptr) {
        return fail(JS::TranscodeResult::Failure_BadDecode);
      }

      *p = ptr;

      return mozilla::Ok();
    }

    MOZ_CRASH("not supported.");
  }

  /*
   * Use SFINAE to refuse any specialization which is not an enum.  Uses of
   * this function do not have to specialize the type of the enumerated field
   * as C++ will extract the parameterized from the argument list.
   */
  template <typename T>
  XDRResult codeEnum32(T* val, std::enable_if_t<std::is_enum_v<T>>* = nullptr) {
    // Mix the enumeration value with a random magic number, such that a
    // corruption with a low-ranged value (like 0) is less likely to cause a
    // miss-interpretation of the XDR content and instead cause a failure.
    const uint32_t MAGIC = 0x21AB218C;
    uint32_t tmp;
    if (mode == XDR_ENCODE) {
      tmp = uint32_t(*val) ^ MAGIC;
    }
    MOZ_TRY(codeUint32(&tmp));
    if (mode == XDR_DECODE) {
      *val = T(tmp ^ MAGIC);
    }
    return mozilla::Ok();
  }

  XDRResult codeDouble(double* dp) {
    union DoublePun {
      double d;
      uint64_t u;
    } pun;
    if (mode == XDR_ENCODE) {
      pun.d = *dp;
    }
    MOZ_TRY(codeUint64(&pun.u));
    if (mode == XDR_DECODE) {
      *dp = pun.d;
    }
    return mozilla::Ok();
  }

  XDRResult codeMarker(uint32_t magic) {
    uint32_t actual = magic;
    MOZ_TRY(codeUint32(&actual));
    if (actual != magic) {
      // Fail in debug, but only soft-fail in release
      MOZ_ASSERT(false, "Bad XDR marker");
      return fail(JS::TranscodeResult::Failure_BadDecode);
    }
    return mozilla::Ok();
  }

  XDRResult codeBytes(void* bytes, size_t len) {
    if (len == 0) {
      return mozilla::Ok();
    }
    if (mode == XDR_ENCODE) {
      uint8_t* ptr = buf->write(len);
      if (!ptr) {
        return fail(JS::TranscodeResult::Throw);
      }
      memcpy(ptr, bytes, len);
    } else {
      const uint8_t* ptr = buf->read(len);
      if (!ptr) {
        return fail(JS::TranscodeResult::Failure_BadDecode);
      }
      memcpy(bytes, ptr, len);
    }
    return mozilla::Ok();
  }

  // While encoding, code the given data to the buffer.
  // While decoding, borrow the buffer and return it to `*data`.
  //
  // The data can have extra bytes after `sizeof(T)`, and the caller should
  // provide the entire data length as `length`.
  //
  // The caller is responsible for aligning the buffer by calling `align32`.
  template <typename T>
  XDRResult borrowedData(T** data, uint32_t length) {
    static_assert(alignof(T) <= 4);
    MOZ_ASSERT(isAligned32());

    if (mode == XDR_ENCODE) {
      MOZ_TRY(codeBytes(*data, length));
    } else {
      const uint8_t* cursor = nullptr;
      MOZ_TRY(readData(&cursor, length));
      *data = reinterpret_cast<T*>(const_cast<uint8_t*>(cursor));
    }
    return mozilla::Ok();
  }

  // Prefer using a variant below that is encoding aware.
  XDRResult codeChars(char* chars, size_t nchars);

  XDRResult codeChars(JS::Latin1Char* chars, size_t nchars);
  XDRResult codeChars(mozilla::Utf8Unit* units, size_t nchars);
  XDRResult codeChars(char16_t* chars, size_t nchars);

  // Transcode null-terminated strings. When decoding, a new buffer is
  // allocated and ownership is returned to caller.
  //
  // NOTE: Throws if string longer than JSString::MAX_LENGTH.
  XDRResult codeCharsZ(XDRTranscodeString<char>& buffer);
  XDRResult codeCharsZ(XDRTranscodeString<char16_t>& buffer);
};

} /* namespace js */

#endif /* vm_Xdr_h */
