/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Xdr_h
#define vm_Xdr_h

#include "mozilla/MaybeOneOf.h"
#include "mozilla/Utf8.h"

#include <type_traits>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "NamespaceImports.h"

#include "js/CompileOptions.h"
#include "js/Transcoding.h"
#include "js/TypeDecls.h"
#include "vm/JSAtom.h"

namespace js {

struct SourceExtent;

namespace frontend {
struct CompilationStencil;
struct ExtensibleCompilationStencil;
struct CompilationStencilMerger;
struct CompilationInput;
}  // namespace frontend

class LifoAlloc;

enum XDRMode { XDR_ENCODE, XDR_DECODE };

template <typename T>
using XDRResultT = mozilla::Result<T, JS::TranscodeResult>;
using XDRResult = XDRResultT<mozilla::Ok>;

class XDRBufferBase {
 public:
  explicit XDRBufferBase(JSContext* cx, size_t cursor = 0)
      : context_(cx), cursor_(cursor) {}

  JSContext* cx() const { return context_; }

  size_t cursor() const { return cursor_; }

 protected:
  JSContext* const context_;
  size_t cursor_;
};

template <XDRMode mode>
class XDRBuffer;

template <>
class XDRBuffer<XDR_ENCODE> : public XDRBufferBase {
 public:
  XDRBuffer(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRBufferBase(cx, cursor), buffer_(buffer) {}

  uint8_t* write(size_t n) {
    MOZ_ASSERT(n != 0);
    if (!buffer_.growByUninitialized(n)) {
      ReportOutOfMemory(cx());
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
        ReportOutOfMemory(cx());
        return false;
      }
      cursor_ += padding;
    }
    return true;
  }

#ifdef DEBUG
  bool isAligned32() { return cursor_ % 4 == 0; }
#endif

  const uint8_t* read(size_t n) {
    MOZ_CRASH("Should never read in encode mode");
    return nullptr;
  }

  const uint8_t* peek(size_t n) {
    MOZ_CRASH("Should never read in encode mode");
    return nullptr;
  }

 private:
  JS::TranscodeBuffer& buffer_;
};

template <>
class XDRBuffer<XDR_DECODE> : public XDRBufferBase {
 public:
  XDRBuffer(JSContext* cx, const JS::TranscodeRange& range)
      : XDRBufferBase(cx), buffer_(range) {}

  XDRBuffer(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRBufferBase(cx, cursor), buffer_(buffer.begin(), buffer.length()) {}

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

#ifdef DEBUG
  bool isAligned32() { return cursor_ % 4 == 0; }
#endif

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
  bool validateResultCode(JSContext* cx, JS::TranscodeResult code) const;
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
  XDRState(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : mainBuf(cx, buffer, cursor), buf(&mainBuf) {}

  template <typename RangeType>
  XDRState(JSContext* cx, const RangeType& range)
      : mainBuf(cx, range), buf(&mainBuf) {}

  // No default copy constructor or copying assignment, because |buf|
  // is an internal pointer.
  XDRState(const XDRState&) = delete;
  XDRState& operator=(const XDRState&) = delete;

  virtual ~XDRState() = default;

  JSContext* cx() const { return mainBuf.cx(); }

  virtual bool isMultiDecode() const { return false; }

  virtual bool hasOptions() const { return false; }
  virtual const JS::ReadOnlyCompileOptions& options() {
    MOZ_CRASH("does not have options");
  }
  virtual bool hasScriptSourceObjectOut() const { return false; }
  virtual ScriptSourceObject** scriptSourceObjectOut() {
    MOZ_CRASH("does not have scriptSourceObjectOut.");
  }

  template <typename T = mozilla::Ok>
  XDRResultT<T> fail(JS::TranscodeResult code) {
#ifdef DEBUG
    MOZ_ASSERT(code != JS::TranscodeResult::Ok);
    MOZ_ASSERT(validateResultCode(cx(), code));
    setResultCode(code);
#endif
    return mozilla::Err(code);
  }

  XDRResult align32() {
    if (!buf->align32()) {
      return fail(JS::TranscodeResult::Throw);
    }
    return Ok();
  }

#ifdef DEBUG
  bool isAligned32() { return buf->isAligned32(); }
#endif

  XDRResult readData(const uint8_t** pptr, size_t length) {
    const uint8_t* ptr = buf->read(length);
    if (!ptr) {
      return fail(JS::TranscodeResult::Failure_BadDecode);
    }
    *pptr = ptr;
    return Ok();
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
    return Ok();
  }

  // Peek uint32_t data.
  XDRResult peekUint32(uint32_t* n) {
    MOZ_ASSERT(mode == XDR_DECODE);
    const uint8_t* ptr = buf->peek(sizeof(*n));
    if (!ptr) {
      return fail(JS::TranscodeResult::Failure_BadDecode);
    }
    *n = *reinterpret_cast<const uint32_t*>(ptr);
    return Ok();
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
    return Ok();
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
    return Ok();
  }

 public:
  XDRResult codeUint16(uint16_t* n) { return codeUintImpl(n); }

  XDRResult codeUint32(uint32_t* n) { return codeUintImpl(n); }

  XDRResult codeUint64(uint64_t* n) { return codeUintImpl(n); }

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
    return Ok();
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
    return Ok();
  }

  XDRResult codeMarker(uint32_t magic) {
    uint32_t actual = magic;
    MOZ_TRY(codeUint32(&actual));
    if (actual != magic) {
      // Fail in debug, but only soft-fail in release
      MOZ_ASSERT(false, "Bad XDR marker");
      return fail(JS::TranscodeResult::Failure_BadDecode);
    }
    return Ok();
  }

  XDRResult codeBytes(void* bytes, size_t len) {
    if (len == 0) {
      return Ok();
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
    return Ok();
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
    return Ok();
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

  XDRResult codeModuleObject(MutableHandleModuleObject modp);
  XDRResult codeScript(MutableHandleScript scriptp);
};

using XDREncoder = XDRState<XDR_ENCODE>;
using XDRDecoderBase = XDRState<XDR_DECODE>;

class XDRDecoder : public XDRDecoderBase {
 public:
  XDRDecoder(JSContext* cx, const JS::ReadOnlyCompileOptions* options,
             JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRDecoderBase(cx, buffer, cursor), options_(options) {
    MOZ_ASSERT(options);
  }

  template <typename RangeType>
  XDRDecoder(JSContext* cx, const JS::ReadOnlyCompileOptions* options,
             const RangeType& range)
      : XDRDecoderBase(cx, range), options_(options) {
    MOZ_ASSERT(options);
  }

  bool hasOptions() const override { return true; }
  const JS::ReadOnlyCompileOptions& options() override { return *options_; }

 private:
  const JS::ReadOnlyCompileOptions* options_;
};

class XDROffThreadDecoder : public XDRDecoder {
  ScriptSourceObject** sourceObjectOut_;
  bool isMultiDecode_;

 public:
  enum class Type {
    Single,
    Multi,
  };

  // Note, when providing an JSContext, where isJSContext is false,
  // then the initialization of the ScriptSourceObject would remain
  // incomplete. Thus, the sourceObjectOut must be used to finish the
  // initialization with ScriptSourceObject::initFromOptions after the
  // decoding.
  //
  // When providing a sourceObjectOut pointer, you have to ensure that it is
  // marked by the GC to avoid dangling pointers.
  XDROffThreadDecoder(JSContext* cx, const JS::ReadOnlyCompileOptions* options,
                      Type type, ScriptSourceObject** sourceObjectOut,
                      const JS::TranscodeRange& range)
      : XDRDecoder(cx, options, range),
        sourceObjectOut_(sourceObjectOut),
        isMultiDecode_(type == Type::Multi) {
    MOZ_ASSERT(sourceObjectOut);
    MOZ_ASSERT(*sourceObjectOut == nullptr);
  }

  bool isMultiDecode() const override { return isMultiDecode_; }

  bool hasScriptSourceObjectOut() const override { return true; }
  ScriptSourceObject** scriptSourceObjectOut() override {
    return sourceObjectOut_;
  }
};

/*
 * The structure of the Stencil XDR buffer is:
 *
 * 1. Header
 *   a. Version
 *   b. ScriptSource
 *   d. Alignment padding
 * 2. Stencil
 *   a. CompilationStencil
 */

/*
 * The stencil decoder accepts `range` as input.
 *
 * The decoded stencils are outputted to the default-initialized
 * `stencil` parameter of `codeStencil` method.
 *
 * The decoded stencils borrow the input `buffer`/`range`, and the consumer
 * has to keep the buffer alive while the decoded stencils are alive.
 */
class XDRStencilDecoder : public XDRDecoderBase {
 public:
  XDRStencilDecoder(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor)
      : XDRDecoderBase(cx, buffer, cursor) {
    MOZ_ASSERT(JS::IsTranscodingBytecodeAligned(buffer.begin()));
    MOZ_ASSERT(JS::IsTranscodingBytecodeOffsetAligned(cursor));
  }

  XDRStencilDecoder(JSContext* cx, const JS::TranscodeRange& range)
      : XDRDecoderBase(cx, range) {
    MOZ_ASSERT(JS::IsTranscodingBytecodeAligned(range.begin().get()));
  }

  XDRResult codeStencil(frontend::CompilationInput& input,
                        frontend::CompilationStencil& stencil);

  bool hasOptions() const override { return !!options_; }
  const JS::ReadOnlyCompileOptions& options() override {
    MOZ_ASSERT(options_);
    return *options_;
  }

 private:
  const JS::ReadOnlyCompileOptions* options_ = nullptr;
};

class XDRStencilEncoder : public XDREncoder {
 public:
  XDRStencilEncoder(JSContext* cx, JS::TranscodeBuffer& buffer)
      : XDREncoder(cx, buffer, buffer.length()) {
    // NOTE: If buffer is empty, buffer.begin() doesn't point valid buffer.
    MOZ_ASSERT_IF(!buffer.empty(),
                  JS::IsTranscodingBytecodeAligned(buffer.begin()));
    MOZ_ASSERT(JS::IsTranscodingBytecodeOffsetAligned(buffer.length()));
  }

 private:
  XDRResult codeStencil(const JS::ReadOnlyCompileOptions* options,
                        const RefPtr<ScriptSource>& source,
                        const frontend::CompilationStencil& stencil);

 public:
  XDRResult codeStencil(const frontend::CompilationInput& input,
                        const frontend::CompilationStencil& stencil);

  XDRResult codeStencil(const RefPtr<ScriptSource>& source,
                        const frontend::CompilationStencil& stencil);
};

class XDRIncrementalStencilEncoder {
  frontend::CompilationStencilMerger* merger_ = nullptr;

 public:
  XDRIncrementalStencilEncoder() = default;

  ~XDRIncrementalStencilEncoder();

  XDRResult linearize(JSContext* cx, JS::TranscodeBuffer& buffer,
                      js::ScriptSource* ss);

  XDRResult setInitial(
      JSContext* cx, const JS::ReadOnlyCompileOptions& options,
      UniquePtr<frontend::ExtensibleCompilationStencil>&& initial);
  XDRResult addDelazification(
      JSContext* cx, const frontend::CompilationStencil& delazification);
};

template <XDRMode mode>
XDRResult XDRAtomOrNull(XDRState<mode>* xdr, js::MutableHandleAtom atomp);

template <XDRMode mode>
XDRResult XDRAtom(XDRState<mode>* xdr, js::MutableHandleAtom atomp);

} /* namespace js */

#endif /* vm_Xdr_h */
