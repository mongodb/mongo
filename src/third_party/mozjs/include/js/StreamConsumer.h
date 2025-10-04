/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_StreamConsumer_h
#define js_StreamConsumer_h

#include "mozilla/Attributes.h"
#include "mozilla/RefCountType.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

#include "js/AllocPolicy.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"

namespace JS {

/**
 * The ConsumeStreamCallback is called from an active JSContext, passing a
 * StreamConsumer that wishes to consume the given host object as a stream of
 * bytes with the given MIME type. On failure, the embedding must report the
 * appropriate error on 'cx'. On success, the embedding must call
 * consumer->consumeChunk() repeatedly on any thread until exactly one of:
 *  - consumeChunk() returns false
 *  - the embedding calls consumer->streamEnd()
 *  - the embedding calls consumer->streamError()
 * before JS_DestroyContext(cx) or JS::ShutdownAsyncTasks(cx) is called.
 *
 * Note: consumeChunk(), streamEnd() and streamError() may be called
 * synchronously by ConsumeStreamCallback.
 *
 * When streamEnd() is called, the embedding may optionally pass an
 * OptimizedEncodingListener*, indicating that there is a cache entry associated
 * with this stream that can store an optimized encoding of the bytes that were
 * just streamed at some point in the future by having SpiderMonkey call
 * storeOptimizedEncoding(). Until the optimized encoding is ready, SpiderMonkey
 * will hold an outstanding refcount to keep the listener alive.
 *
 * After storeOptimizedEncoding() is called, on cache hit, the embedding
 * may call consumeOptimizedEncoding() instead of consumeChunk()/streamEnd().
 * The embedding must ensure that the GetOptimizedEncodingBuildId() (see
 * js/BuildId.h) at the time when an optimized encoding is created is the same
 * as when it is later consumed.
 */

class OptimizedEncodingListener {
 protected:
  virtual ~OptimizedEncodingListener() = default;

 public:
  // SpiderMonkey will hold an outstanding reference count as long as it holds
  // a pointer to OptimizedEncodingListener.
  virtual MozExternalRefCountType MOZ_XPCOM_ABI AddRef() = 0;
  virtual MozExternalRefCountType MOZ_XPCOM_ABI Release() = 0;

  // SpiderMonkey may optionally call storeOptimizedEncoding() after it has
  // finished processing a streamed resource.
  virtual void storeOptimizedEncoding(const uint8_t* bytes, size_t length) = 0;
};

class JS_PUBLIC_API StreamConsumer {
 protected:
  // AsyncStreamConsumers are created and destroyed by SpiderMonkey.
  StreamConsumer() = default;
  virtual ~StreamConsumer() = default;

 public:
  // Called by the embedding as each chunk of bytes becomes available.
  // If this function returns 'false', the stream must drop all pointers to
  // this StreamConsumer.
  virtual bool consumeChunk(const uint8_t* begin, size_t length) = 0;

  // Called by the embedding when the stream reaches end-of-file, passing the
  // listener described above.
  virtual void streamEnd(OptimizedEncodingListener* listener = nullptr) = 0;

  // Called by the embedding when there is an error during streaming. The
  // given error code should be passed to the ReportStreamErrorCallback on the
  // main thread to produce the semantically-correct rejection value.
  virtual void streamError(size_t errorCode) = 0;

  // Called by the embedding *instead of* consumeChunk()/streamEnd() if an
  // optimized encoding is available from a previous streaming of the same
  // contents with the same optimized build id.
  virtual void consumeOptimizedEncoding(const uint8_t* begin,
                                        size_t length) = 0;

  // Provides optional stream attributes such as base or source mapping URLs.
  // Necessarily called before consumeChunk(), streamEnd(), streamError() or
  // consumeOptimizedEncoding(). The caller retains ownership of the strings.
  virtual void noteResponseURLs(const char* maybeUrl,
                                const char* maybeSourceMapUrl) = 0;
};

enum class MimeType { Wasm };

using ConsumeStreamCallback = bool (*)(JSContext*, JS::HandleObject, MimeType,
                                       StreamConsumer*);

using ReportStreamErrorCallback = void (*)(JSContext*, size_t);

extern JS_PUBLIC_API void InitConsumeStreamCallback(
    JSContext* cx, ConsumeStreamCallback consume,
    ReportStreamErrorCallback report);

}  // namespace JS

#endif  // js_StreamConsumer_h
