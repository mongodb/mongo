/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* SpiderMonkey TraceLogger APIs. */

#ifndef js_TraceLoggerAPI_h
#define js_TraceLoggerAPI_h

#include "jstypes.h"

#include "js/TypeDecls.h"

namespace mozilla {
class JSONWriteFunc;
class TimeStamp;
};  // namespace mozilla

namespace JS {

// Used to lock any tracelogger activities, and consequently, will also block
// any further JS execution when a thread hits an atomic tracelogger activity
// such as payload creation.
class AutoTraceLoggerLockGuard {
 public:
  AutoTraceLoggerLockGuard();
  ~AutoTraceLoggerLockGuard();
};

// An implementation type must be defined in order to gather data using the
// TraceLoggerCollectorBuffer. Each implementation must define the type that is
// being collected in the buffer, along with a static method that is used to
// actually write into the buffer from the tracelogger.
struct TraceLoggerDictionaryImpl {
  using ImplType = char;
  static size_t NextChunk(JSContext* cx, size_t* dataIndex, ImplType buffer[],
                          size_t bufferSize);
};

struct TraceLoggerIdImpl {
  using ImplType = uint32_t;
  static size_t NextChunk(JSContext* cx, size_t* dataIndex, ImplType buffer[],
                          size_t bufferSize);
};

struct TraceLoggerLineNoImpl {
  using ImplType = int32_t;
  static size_t NextChunk(JSContext* cx, size_t* dataIndex, ImplType buffer[],
                          size_t bufferSize);
};

struct TraceLoggerColNoImpl {
  using ImplType = int32_t;
  static size_t NextChunk(JSContext* cx, size_t* dataIndex, ImplType buffer[],
                          size_t bufferSize);
};

struct TraceLoggerTimeStampImpl {
  using ImplType = mozilla::TimeStamp;
  static size_t NextChunk(JSContext* cx, size_t* dataIndex, ImplType buffer[],
                          size_t bufferSize);
};

struct TraceLoggerDurationImpl {
  using ImplType = double;
  static size_t NextChunk(JSContext* cx, size_t* dataIndex, ImplType buffer[],
                          size_t bufferSize);
};

// Buffer that is used to retrieve tracelogger data in fixed size chunks so that
// allocation of a large array is not necessary.  The TraceLoggerCollectorBuffer
// class will manage an internal state which points to the next data index being
// collected.  Each call to NextChunk will also clobber the internal buffer used
// to store the data.
template <class T>
class TraceLoggerCollectorBuffer {
  using ImplType = typename T::ImplType;

 public:
  class Iterator {
   public:
    Iterator(ImplType* buffer, size_t index)
        : iteratorIndex(index), buf(buffer) {}

    Iterator operator++() {
      iteratorIndex++;
      return *this;
    }

    bool operator!=(const Iterator& other) const {
      return iteratorIndex != other.iteratorIndex;
    }

    ImplType operator*() const { return buf[iteratorIndex]; }

   private:
    size_t iteratorIndex;
    ImplType* buf;
  };

  explicit TraceLoggerCollectorBuffer(AutoTraceLoggerLockGuard& lockGuard,
                                      JSContext* cx = nullptr,
                                      size_t length = 4096)
      : cx_(cx), length_(length), dataIndex_(0), bufferIndex_(0) {
    buffer_ = js_pod_malloc<ImplType>(length);
  }

  ~TraceLoggerCollectorBuffer() { js_free(buffer_); }

  Iterator begin() const { return Iterator(buffer_, 0); }

  Iterator end() const { return Iterator(buffer_, bufferIndex_); }

  ImplType* internalBuffer() const { return buffer_; }

  bool NextChunk() {
    bufferIndex_ = T::NextChunk(cx_, &dataIndex_, buffer_, length_);
    return (bufferIndex_ != 0) ? true : false;
  }

 private:
  JSContext* cx_;
  size_t length_;
  size_t dataIndex_;
  size_t bufferIndex_;
  ImplType* buffer_;
};

#ifdef JS_TRACE_LOGGING

// Initialize the trace logger.  This must be called before using any of the
// other trace logging functions.
extern JS_PUBLIC_API bool InitTraceLogger();

// Return whether the trace logger is supported in this browser session.
extern JS_PUBLIC_API bool TraceLoggerSupported();

// Begin trace logging events.  This will activate some of the
// textId's for various events and set the global option
// JSJITCOMPILER_ENABLE_TRACELOGGER to true.
// This does nothing except return if the trace logger is already active.
extern JS_PUBLIC_API void StartTraceLogger(JSContext* cx);

// Stop trace logging events.  All textId's will be set to false, and the
// global JSJITCOMPILER_ENABLE_TRACELOGGER will be set to false.
// This does nothing except return if the trace logger is not active.
extern JS_PUBLIC_API void StopTraceLogger(JSContext* cx);

// Clear and free any event data that was recorded by the trace logger.
extern JS_PUBLIC_API void ResetTraceLogger(void);

// Spew trace logger statistics.
extern JS_PUBLIC_API void SpewTraceLoggerThread(JSContext* cx);

// Spew trace logger statistics.
extern JS_PUBLIC_API void SpewTraceLoggerForCurrentProcess();

#else
// Define empty inline functions for when trace logging compilation is not
// enabled.  TraceLogging.cpp will not be built in that case so we need to
// provide something for any routines that reference these.
inline bool InitTraceLogger() { return true; }
inline bool TraceLoggerSupported() { return false; }
inline void StartTraceLogger(JSContext* cx) {}
inline void StopTraceLogger(JSContext* cx) {}
inline void ResetTraceLogger(void) {}
inline void SpewTraceLoggerThread(JSContext* cx) {}
inline void SpewTraceLoggerForCurrentProcess() {}
inline size_t TraceLoggerDictionaryImpl::NextChunk(JSContext* cx,
                                                   size_t* dataIndex,
                                                   ImplType buffer[],
                                                   size_t bufferSize) {
  return 0;
}
inline size_t TraceLoggerIdImpl::NextChunk(JSContext* cx, size_t* dataIndex,
                                           ImplType buffer[],
                                           size_t bufferSize) {
  return 0;
}
inline size_t TraceLoggerTimeStampImpl::NextChunk(JSContext* cx,
                                                  size_t* dataIndex,
                                                  ImplType buffer[],
                                                  size_t bufferSize) {
  return 0;
}
inline size_t TraceLoggerDurationImpl::NextChunk(JSContext* cx,
                                                 size_t* dataIndex,
                                                 ImplType buffer[],
                                                 size_t bufferSize) {
  return 0;
}
inline size_t TraceLoggerLineNoImpl::NextChunk(JSContext* cx, size_t* dataIndex,
                                               ImplType buffer[],
                                               size_t bufferSize) {
  return 0;
}
inline size_t TraceLoggerColNoImpl::NextChunk(JSContext* cx, size_t* dataIndex,
                                              ImplType buffer[],
                                              size_t bufferSize) {
  return 0;
}
inline AutoTraceLoggerLockGuard::AutoTraceLoggerLockGuard() {}
inline AutoTraceLoggerLockGuard::~AutoTraceLoggerLockGuard() {}
#endif
using TraceLoggerDictionaryBuffer =
    TraceLoggerCollectorBuffer<JS::TraceLoggerDictionaryImpl>;
using TraceLoggerIdBuffer = TraceLoggerCollectorBuffer<JS::TraceLoggerIdImpl>;
using TraceLoggerTimeStampBuffer =
    TraceLoggerCollectorBuffer<JS::TraceLoggerTimeStampImpl>;
using TraceLoggerDurationBuffer =
    TraceLoggerCollectorBuffer<JS::TraceLoggerDurationImpl>;
using TraceLoggerLineNoBuffer =
    TraceLoggerCollectorBuffer<JS::TraceLoggerLineNoImpl>;
using TraceLoggerColNoBuffer =
    TraceLoggerCollectorBuffer<JS::TraceLoggerColNoImpl>;
};  // namespace JS

#endif /* js_TraceLoggerAPI_h */
