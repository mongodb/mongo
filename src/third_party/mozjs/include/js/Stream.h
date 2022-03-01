/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JSAPI functions and callbacks related to WHATWG Stream objects.
 *
 * Much of the API here mirrors the standard algorithms and standard JS methods
 * of the objects defined in the Streams standard. One difference is that the
 * functionality of the JS controller object is exposed to C++ as functions
 * taking ReadableStream instances instead, for convenience.
 */

#ifndef js_Stream_h
#define js_Stream_h

#include <stddef.h>

#include "jstypes.h"

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

struct JSClass;

namespace JS {

/**
 * Abstract base class for external underlying sources.
 *
 * The term "underlying source" is defined in the Streams spec:
 *   https://streams.spec.whatwg.org/#underlying-source
 *
 * A `ReadableStreamUnderlyingSource` is an underlying source that is
 * implemented in C++ rather than JS. It can be passed to
 * `JS::NewReadableExternalSourceStreamObject` to create a custom,
 * embedding-defined ReadableStream.
 *
 * There are several API difference between this class and the standard API for
 * underlying sources implemented in JS:
 *
 * -   JS underlying sources can be either byte sources or non-byte sources.
 *     External underlying source are always byte sources.
 *
 * -   The C++ API does not bother with controller objects. Instead of using
 *     controller methods, the underlying source directly calls API functions
 *     like JS::ReadableStream{UpdateDataAvailableFromSource,Close,Error}.
 *
 * -   External readable streams are optimized to allow the embedding to
 *     interact with them with a minimum of overhead: chunks aren't enqueued as
 *     individual typed arrays; instead, the embedding only updates the amount
 *     of data available using
 *     JS::ReadableStreamUpdateDataAvailableFromSource. When JS requests data
 *     from a reader, writeIntoReadRequestBuffer is invoked, asking the
 *     embedding to write data directly into the buffer we're about to hand to
 *     JS.
 *
 * -   The C++ API provides extra callbacks onClosed() and onErrored().
 *
 * -   This class has a `finalize()` method, because C++ cares about lifetimes.
 *
 * Additionally, ReadableStreamGetExternalUnderlyingSource can be used to get
 * the pointer to the underlying source. This locks the stream until it is
 * released again using JS::ReadableStreamReleaseExternalUnderlyingSource.
 *
 * Embeddings can use this to optimize away the JS `ReadableStream` overhead
 * when an embedding-defined C++ stream is passed to an embedding-defined C++
 * consumer. For example, consider a ServiceWorker piping a `fetch` Response
 * body to a TextDecoder. Instead of copying chunks of data into JS typed array
 * buffers and creating a Promise per chunk, only to immediately resolve the
 * Promises and read the data out again, the embedding can directly feed the
 * incoming data to the TextDecoder.
 *
 * Compartment safety: All methods (except `finalize`) receive `cx` and
 * `stream` arguments. SpiderMonkey enters the realm of the stream object
 * before invoking these methods, so `stream` is never a wrapper. Other
 * arguments may be wrappers.
 */
class JS_PUBLIC_API ReadableStreamUnderlyingSource {
 public:
  virtual ~ReadableStreamUnderlyingSource() = default;

  /**
   * Invoked whenever a reader desires more data from this source.
   *
   * The given `desiredSize` is the absolute size, not a delta from the
   * previous desired size.
   */
  virtual void requestData(JSContext* cx, HandleObject stream,
                           size_t desiredSize) = 0;

  /**
   * Invoked to cause the embedding to fill the given `buffer` with data from
   * this underlying source.
   *
   * This is called only after the embedding has updated the amount of data
   * available using JS::ReadableStreamUpdateDataAvailableFromSource. If at
   * least one read request is pending when
   * JS::ReadableStreamUpdateDataAvailableFromSource is called, this method
   * is invoked immediately from under the call to
   * JS::ReadableStreamUpdateDataAvailableFromSource. If not, it is invoked
   * if and when a new read request is made.
   *
   * Note: This method *must not cause GC*, because that could potentially
   * invalidate the `buffer` pointer.
   */
  virtual void writeIntoReadRequestBuffer(JSContext* cx, HandleObject stream,
                                          void* buffer, size_t length,
                                          size_t* bytesWritten) = 0;

  /**
   * Invoked in reaction to the ReadableStream being canceled. This is
   * equivalent to the `cancel` method on non-external underlying sources
   * provided to the ReadableStream constructor in JavaScript.
   *
   * The underlying source may free up some resources in this method, but
   * `*this` must not be destroyed until `finalize()` is called.
   *
   * The given `reason` is the JS::Value that was passed as an argument to
   * ReadableStream#cancel().
   *
   * The returned JS::Value will be used to resolve the Promise returned by
   * ReadableStream#cancel().
   */
  virtual Value cancel(JSContext* cx, HandleObject stream,
                       HandleValue reason) = 0;

  /**
   * Invoked when the associated ReadableStream becomes closed.
   *
   * The underlying source may free up some resources in this method, but
   * `*this` must not be destroyed until `finalize()` is called.
   */
  virtual void onClosed(JSContext* cx, HandleObject stream) = 0;

  /**
   * Invoked when the associated ReadableStream becomes errored.
   *
   * The underlying source may free up some resources in this method, but
   * `*this` must not be destroyed until `finalize()` is called.
   */
  virtual void onErrored(JSContext* cx, HandleObject stream,
                         HandleValue reason) = 0;

  /**
   * Invoked when the associated ReadableStream object is finalized. The
   * stream object is not passed as an argument, as it might not be in a
   * valid state anymore.
   *
   * Note: Finalization can happen on a background thread, so the embedding
   * must be prepared for `finalize()` to be invoked from any thread.
   */
  virtual void finalize() = 0;
};

/**
 * Returns a new instance of the ReadableStream builtin class in the current
 * compartment, configured as a default stream.
 * If a |proto| is passed, that gets set as the instance's [[Prototype]]
 * instead of the original value of |ReadableStream.prototype|.
 */
extern JS_PUBLIC_API JSObject* NewReadableDefaultStreamObject(
    JSContext* cx, HandleObject underlyingSource = nullptr,
    HandleFunction size = nullptr, double highWaterMark = 1,
    HandleObject proto = nullptr);

/**
 * Returns a new instance of the ReadableStream builtin class in the current
 * compartment.
 *
 * The instance is a byte stream backed by an embedding-provided underlying
 * source, using the virtual methods of `underlyingSource` as callbacks. The
 * embedding must ensure that `*underlyingSource` lives as long as the new
 * stream object. The JS engine will call the finalize() method when the stream
 * object is destroyed.
 *
 * `nsISupportsObject_alreadyAddreffed` is an optional pointer that can be used
 * to make the new stream participate in Gecko's cycle collection. Here are the
 * rules for using this parameter properly:
 *
 * -   `*underlyingSource` must not be a cycle-collected object. (It would lead
 *     to memory leaks as the cycle collector would not be able to collect
 *     cycles containing that object.)
 *
 * -   `*underlyingSource` must not contain nsCOMPtrs that point to cycle-
 *     collected objects. (Same reason.)
 *
 * -   `*underlyingSource` may contain a pointer to a single cycle-collected
 *     object.
 *
 * -   The pointer may be stored in `*underlyingSource` as a raw pointer.
 *
 * -   The pointer to the nsISupports interface of the same object must be
 *     passed as the `nsISupportsObject_alreadyAddreffed` parameter to this
 *     function. (This is how the cycle collector knows about it, so omitting
 *     this would again cause leaks.)
 *
 * If `proto` is non-null, it is used as the instance's [[Prototype]] instead
 * of the original value of `ReadableStream.prototype`.
 */
extern JS_PUBLIC_API JSObject* NewReadableExternalSourceStreamObject(
    JSContext* cx, ReadableStreamUnderlyingSource* underlyingSource,
    void* nsISupportsObject_alreadyAddreffed = nullptr,
    HandleObject proto = nullptr);

/**
 * Returns the embedding-provided underlying source of the given |stream|.
 *
 * Can be used to optimize operations if both the underlying source and the
 * intended sink are embedding-provided. In that case it might be
 * preferrable to pipe data directly from source to sink without interacting
 * with the stream at all.
 *
 * Locks the stream until ReadableStreamReleaseExternalUnderlyingSource is
 * called.
 *
 * Throws an exception if the stream is locked, i.e. if a reader has been
 * acquired for the stream, or if ReadableStreamGetExternalUnderlyingSource
 * has been used previously without releasing the external source again.
 *
 * Throws an exception if the stream isn't readable, i.e if it is errored or
 * closed. This is different from ReadableStreamGetReader because we don't
 * have a Promise to resolve/reject, which a reader provides.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 *
 * Asserts that the stream has an embedding-provided underlying source.
 */
extern JS_PUBLIC_API bool ReadableStreamGetExternalUnderlyingSource(
    JSContext* cx, HandleObject stream,
    ReadableStreamUnderlyingSource** source);

/**
 * Releases the embedding-provided underlying source of the given |stream|,
 * returning the stream into an unlocked state.
 *
 * Asserts that the stream was locked through
 * ReadableStreamGetExternalUnderlyingSource.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 *
 * Asserts that the stream has an embedding-provided underlying source.
 */
extern JS_PUBLIC_API bool ReadableStreamReleaseExternalUnderlyingSource(
    JSContext* cx, HandleObject stream);

/**
 * Update the amount of data available at the underlying source of the given
 * |stream|.
 *
 * Can only be used for streams with an embedding-provided underlying source.
 * The JS engine will use the given value to satisfy read requests for the
 * stream by invoking the writeIntoReadRequestBuffer method.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamUpdateDataAvailableFromSource(
    JSContext* cx, HandleObject stream, uint32_t availableData);

/**
 * Break the cycle between this object and the
 * nsISupportsObject_alreadyAddreffed passed in
 * NewReadableExternalSourceStreamObject().
 */
extern JS_PUBLIC_API void ReadableStreamReleaseCCObject(JSObject* stream);

/**
 * Returns true if the given object is a ReadableStream object or an
 * unwrappable wrapper for one, false otherwise.
 */
extern JS_PUBLIC_API bool IsReadableStream(JSObject* obj);

/**
 * Returns true if the given object is a ReadableStreamDefaultReader or
 * ReadableStreamBYOBReader object or an unwrappable wrapper for one, false
 * otherwise.
 */
extern JS_PUBLIC_API bool IsReadableStreamReader(JSObject* obj);

/**
 * Returns true if the given object is a ReadableStreamDefaultReader object
 * or an unwrappable wrapper for one, false otherwise.
 */
extern JS_PUBLIC_API bool IsReadableStreamDefaultReader(JSObject* obj);

enum class ReadableStreamMode { Default, Byte, ExternalSource };

/**
 * Returns the stream's ReadableStreamMode. If the mode is |Byte| or
 * |ExternalSource|, it's possible to acquire a BYOB reader for more optimized
 * operations.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamGetMode(JSContext* cx,
                                                HandleObject stream,
                                                ReadableStreamMode* mode);

enum class ReadableStreamReaderMode { Default };

/**
 * Returns true if the given ReadableStream is readable, false if not.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamIsReadable(JSContext* cx,
                                                   HandleObject stream,
                                                   bool* result);

/**
 * Returns true if the given ReadableStream is locked, false if not.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamIsLocked(JSContext* cx,
                                                 HandleObject stream,
                                                 bool* result);

/**
 * Returns true if the given ReadableStream is disturbed, false if not.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamIsDisturbed(JSContext* cx,
                                                    HandleObject stream,
                                                    bool* result);

/**
 * Cancels the given ReadableStream with the given reason and returns a
 * Promise resolved according to the result.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API JSObject* ReadableStreamCancel(JSContext* cx,
                                                    HandleObject stream,
                                                    HandleValue reason);

/**
 * Creates a reader of the type specified by the mode option and locks the
 * stream to the new reader.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one. The returned object will always be created in the
 * current cx compartment.
 */
extern JS_PUBLIC_API JSObject* ReadableStreamGetReader(
    JSContext* cx, HandleObject stream, ReadableStreamReaderMode mode);

/**
 * Tees the given ReadableStream and stores the two resulting streams in
 * outparams. Returns false if the operation fails, e.g. because the stream is
 * locked.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamTee(JSContext* cx, HandleObject stream,
                                            MutableHandleObject branch1Stream,
                                            MutableHandleObject branch2Stream);

/**
 * Retrieves the desired combined size of additional chunks to fill the given
 * ReadableStream's queue. Stores the result in |value| and sets |hasValue| to
 * true on success, returns false on failure.
 *
 * If the stream is errored, the call will succeed but no value will be stored
 * in |value| and |hasValue| will be set to false.
 *
 * Note: This is semantically equivalent to the |desiredSize| getter on
 * the stream controller's prototype in JS. We expose it with the stream
 * itself as a target for simplicity.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamGetDesiredSize(JSContext* cx,
                                                       JSObject* stream,
                                                       bool* hasValue,
                                                       double* value);

/**
 * Close the given ReadableStream. This is equivalent to `controller.close()`
 * in JS.
 *
 * This can fail with or without an exception pending under a variety of
 * circumstances. On failure, the stream may or may not be closed, and
 * downstream consumers may or may not have been notified.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamClose(JSContext* cx,
                                              HandleObject stream);

/**
 * Returns true if the given ReadableStream reader is locked, false otherwise.
 *
 * Asserts that |reader| is a ReadableStreamDefaultReader or
 * ReadableStreamBYOBReader object or an unwrappable wrapper for one.
 */
extern JS_PUBLIC_API bool ReadableStreamReaderIsClosed(JSContext* cx,
                                                       HandleObject reader,
                                                       bool* result);

/**
 * Enqueues the given chunk in the given ReadableStream.
 *
 * Throws a TypeError and returns false if the enqueing operation fails.
 *
 * Note: This is semantically equivalent to the |enqueue| method on
 * the stream controller's prototype in JS. We expose it with the stream
 * itself as a target for simplicity.
 *
 * If the ReadableStream has an underlying byte source, the given chunk must
 * be a typed array or a DataView. Consider using
 * ReadableByteStreamEnqueueBuffer.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamEnqueue(JSContext* cx,
                                                HandleObject stream,
                                                HandleValue chunk);

/**
 * Errors the given ReadableStream, causing all future interactions to fail
 * with the given error value.
 *
 * Throws a TypeError and returns false if the erroring operation fails.
 *
 * Note: This is semantically equivalent to the |error| method on
 * the stream controller's prototype in JS. We expose it with the stream
 * itself as a target for simplicity.
 *
 * Asserts that |stream| is a ReadableStream object or an unwrappable wrapper
 * for one.
 */
extern JS_PUBLIC_API bool ReadableStreamError(JSContext* cx,
                                              HandleObject stream,
                                              HandleValue error);

/**
 * C++ equivalent of `reader.cancel(reason)`
 * (both <https://streams.spec.whatwg.org/#default-reader-cancel> and
 * <https://streams.spec.whatwg.org/#byob-reader-cancel>).
 *
 * `reader` must be a stream reader created using `JS::ReadableStreamGetReader`
 * or an unwrappable wrapper for one. (This function is meant to support using
 * C++ to read from streams. It's not meant to allow C++ code to operate on
 * readers created by scripts.)
 */
extern JS_PUBLIC_API bool ReadableStreamReaderCancel(JSContext* cx,
                                                     HandleObject reader,
                                                     HandleValue reason);

/**
 * C++ equivalent of `reader.releaseLock()`
 * (both <https://streams.spec.whatwg.org/#default-reader-release-lock> and
 * <https://streams.spec.whatwg.org/#byob-reader-release-lock>).
 *
 * `reader` must be a stream reader created using `JS::ReadableStreamGetReader`
 * or an unwrappable wrapper for one.
 */
extern JS_PUBLIC_API bool ReadableStreamReaderReleaseLock(JSContext* cx,
                                                          HandleObject reader);

/**
 * C++ equivalent of the `reader.read()` method on default readers
 * (<https://streams.spec.whatwg.org/#default-reader-read>).
 *
 * The result is a new Promise object, or null on OOM.
 *
 * `reader` must be the result of calling `JS::ReadableStreamGetReader` with
 * `ReadableStreamReaderMode::Default` mode, or an unwrappable wrapper for such
 * a reader.
 */
extern JS_PUBLIC_API JSObject* ReadableStreamDefaultReaderRead(
    JSContext* cx, HandleObject reader);

class JS_PUBLIC_API WritableStreamUnderlyingSink {
 public:
  virtual ~WritableStreamUnderlyingSink() = default;

  /**
   * Invoked when the associated WritableStream object is finalized. The
   * stream object is not passed as an argument, as it might not be in a
   * valid state anymore.
   *
   * Note: Finalization can happen on a background thread, so the embedding
   * must be prepared for `finalize()` to be invoked from any thread.
   */
  virtual void finalize() = 0;
};

// ReadableStream.prototype.pipeTo SUPPORT

/**
 * The signature of a function that, when passed an |AbortSignal| instance, will
 * return the value of its "aborted" flag.
 *
 * This function will be called while |signal|'s realm has been entered.
 */
using AbortSignalIsAborted = bool (*)(JSObject* signal);

/**
 * Dictate embedder-specific details necessary to implement certain aspects of
 * the |ReadableStream.prototype.pipeTo| function.  This should be performed
 * exactly once, for a single context associated with a |JSRuntime|.
 *
 * The |ReadableStream.prototype.pipeTo| function accepts a |signal| argument
 * that may be used to abort the piping operation.  This argument must be either
 * |undefined| (in other words, the piping operation can't be aborted) or an
 * |AbortSignal| instance (that may be aborted using the signal's associated
 * |AbortController|).  |AbortSignal| is defined by WebIDL and the DOM in the
 * web embedding.  Therefore, embedders must use this function to specify how
 * such objects can be recognized and how to perform various essential actions
 * upon them.
 *
 * The provided |isAborted| function will be called with an unwrapped
 * |AbortSignal| instance, while that instance's realm has been entered.
 *
 * If this function isn't called, and a situation arises where an "is this an
 * |AbortSignal|?" question must be asked, that question will simply be answered
 * "no".
 */
extern JS_PUBLIC_API void InitPipeToHandling(const JSClass* abortSignalClass,
                                             AbortSignalIsAborted isAborted,
                                             JSContext* cx);

}  // namespace JS

#endif  // js_Stream_h
