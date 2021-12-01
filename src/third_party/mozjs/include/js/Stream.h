/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JSAPI functions and callbacks related to WHATWG Stream objects.
 *
 * Much of the API here mirrors the JS API of ReadableStream and associated
 * classes, e.g. ReadableStreamDefaultReader, ReadableStreamBYOBReader,
 * ReadableStreamDefaultController, ReadableByteStreamController, and
 * ReadableStreamBYOBRequest.
 *
 * There are some crucial differences, though: Functionality that's exposed
 * as methods/accessors on controllers in JS is exposed as functions taking
 * ReadableStream instances instead. This is because an analysis of how
 * the API would be used showed that all functions that'd take controllers
 * would do so by first getting the controller from the stream instance it's
 * associated with and then call the function taking it. I.e., it would purely
 * add boilerplate without any gains in ease of use of the API.
 *
 * It would probably still make sense to factor the API the same as the JS API
 * if we had to keep any API stability guarantees: the JS API won't change, so
 * we could be sure that the C++ API could stay the same, too. Given that we
 * don't guarantee API stability, this concern isn't too pressing.
 *
 * Some functions exposed here deal with ReadableStream instances that have an
 * embedding-provided underlying source. These instances are largely similar
 * to byte streams as created using |new ReadableStream({type: "bytes"})|:
 * They enable users to acquire ReadableStreamBYOBReaders and only vend chunks
 * that're typed array instances.
 *
 * When creating an "external readable stream" using
 * JS::NewReadableExternalSourceStreamObject, an underlying source and a set
 * of flags can be passed to be stored on the stream. The underlying source is
 * treated as an opaque void* pointer by the JS engine: it's purely meant as
 * a reference to be used by the embedding to identify whatever actual source
 * it uses to supply data for the stream. Similarly, the flags aren't
 * interpreted by the JS engine, but are passed to some of the callbacks below
 * and can be retrieved using JS::ReadableStreamGetEmbeddingFlags.
 *
 * External readable streams are optimized to allow the embedding to interact
 * with them with a minimum of overhead: chunks aren't enqueued as individual
 * typed array instances; instead, the embedding only updates the amount of
 * data available using ReadableStreamUpdateDataAvailableFromSource.
 * When content requests data by reading from a reader,
 * WriteIntoReadRequestBufferCallback is invoked, asking the embedding to
 * write data directly into the buffer we're about to hand to content.
 *
 * Additionally, ReadableStreamGetExternalUnderlyingSource can be used to
 * get the void* pointer to the underlying source. This is equivalent to
 * acquiring a reader for the stream in that it locks the stream until it
 * is released again using JS::ReadableStreamReleaseExternalUnderlyingSource.
 *
 * Embeddings are expected to detect situations where an API exposed to JS
 * takes a ReadableStream to read from that has an external underlying source.
 * In those situations, it might be preferable to directly perform data
 * transfers from the stream's underlying source to whatever sink the
 * embedding uses, assuming that such direct transfers can be performed
 * more efficiently.
 *
 * An example of such an optimized operation might be a ServiceWorker piping a
 * fetch Response body to a TextDecoder: instead of writing chunks of data
 * into JS typed array buffers only to immediately read from them again, the
 * embedding can presumably directly feed the incoming data to the
 * TextDecoder's underlying implementation.
 */

#ifndef js_Stream_h
#define js_Stream_h

#include "jstypes.h"

#include "js/TypeDecls.h"

namespace JS {

/**
 * Invoked whenever a reader desires more data from a ReadableStream's
 * embedding-provided underlying source.
 *
 * The given |desiredSize| is the absolute size, not a delta from the previous
 * desired size.
 */
typedef void
(* RequestReadableStreamDataCallback)(JSContext* cx, HandleObject stream,
                                      void* underlyingSource, uint8_t flags, size_t desiredSize);

/**
 * Invoked to cause the embedding to fill the given |buffer| with data from
 * the given embedding-provided underlying source.
 *
 * This can only happen after the embedding has updated the amount of data
 * available using JS::ReadableStreamUpdateDataAvailableFromSource. If at
 * least one read request is pending when
 * JS::ReadableStreamUpdateDataAvailableFromSource is called,
 * the WriteIntoReadRequestBufferCallback is invoked immediately from under
 * the call to JS::WriteIntoReadRequestBufferCallback. If not, it is invoked
 * if and when a new read request is made.
 *
 * Note: This callback *must not cause GC*, because that could potentially
 * invalidate the |buffer| pointer.
 */
typedef void
(* WriteIntoReadRequestBufferCallback)(JSContext* cx, HandleObject stream,
                                       void* underlyingSource, uint8_t flags, void* buffer,
                                       size_t length, size_t* bytesWritten);

/**
 * Invoked in reaction to the ReadableStream being canceled to allow the
 * embedding to free the underlying source.
 *
 * This is equivalent to calling |cancel| on non-external underlying sources
 * provided to the ReadableStream constructor in JavaScript.
 *
 * The given |reason| is the JS::Value that was passed as an argument to
 * ReadableStream#cancel().
 *
 * The returned JS::Value will be used to resolve the Promise returned by
 * ReadableStream#cancel().
 */
typedef Value
(* CancelReadableStreamCallback)(JSContext* cx, HandleObject stream,
                                 void* underlyingSource, uint8_t flags, HandleValue reason);

/**
 * Invoked in reaction to a ReadableStream with an embedding-provided
 * underlying source being closed.
 */
typedef void
(* ReadableStreamClosedCallback)(JSContext* cx, HandleObject stream, void* underlyingSource,
                                 uint8_t flags);

/**
 * Invoked in reaction to a ReadableStream with an embedding-provided
 * underlying source being errored with the
 * given reason.
 */
typedef void
(* ReadableStreamErroredCallback)(JSContext* cx, HandleObject stream, void* underlyingSource,
                                  uint8_t flags, HandleValue reason);

/**
 * Invoked in reaction to a ReadableStream with an embedding-provided
 * underlying source being finalized. Only the underlying source is passed
 * as an argument, while the ReadableStream itself is not to prevent the
 * embedding from operating on a JSObject that might not be in a valid state
 * anymore.
 *
 * Note: the ReadableStream might be finalized on a background thread. That
 * means this callback might be invoked from an arbitrary thread, which the
 * embedding must be able to handle.
 */
typedef void
(* ReadableStreamFinalizeCallback)(void* underlyingSource, uint8_t flags);

/**
 * Sets runtime-wide callbacks to use for interacting with embedding-provided
 * hooks for operating on ReadableStream instances.
 *
 * See the documentation for the individual callback types for details.
 */
extern JS_PUBLIC_API(void)
SetReadableStreamCallbacks(JSContext* cx,
                           RequestReadableStreamDataCallback dataRequestCallback,
                           WriteIntoReadRequestBufferCallback writeIntoReadRequestCallback,
                           CancelReadableStreamCallback cancelCallback,
                           ReadableStreamClosedCallback closedCallback,
                           ReadableStreamErroredCallback erroredCallback,
                           ReadableStreamFinalizeCallback finalizeCallback);

extern JS_PUBLIC_API(bool)
HasReadableStreamCallbacks(JSContext* cx);

/**
 * Returns a new instance of the ReadableStream builtin class in the current
 * compartment, configured as a default stream.
 * If a |proto| is passed, that gets set as the instance's [[Prototype]]
 * instead of the original value of |ReadableStream.prototype|.
 */
extern JS_PUBLIC_API(JSObject*)
NewReadableDefaultStreamObject(JSContext* cx, HandleObject underlyingSource = nullptr,
                               HandleFunction size = nullptr, double highWaterMark = 1,
                               HandleObject proto = nullptr);

/**
 * Returns a new instance of the ReadableStream builtin class in the current
 * compartment, configured as a byte stream.
 * If a |proto| is passed, that gets set as the instance's [[Prototype]]
 * instead of the original value of |ReadableStream.prototype|.
 */
extern JS_PUBLIC_API(JSObject*)
NewReadableByteStreamObject(JSContext* cx, HandleObject underlyingSource = nullptr,
                            double highWaterMark = 0, HandleObject proto = nullptr);

/**
 * Returns a new instance of the ReadableStream builtin class in the current
 * compartment, with the right slot layout. If a |proto| is passed, that gets
 * set as the instance's [[Prototype]] instead of the original value of
 * |ReadableStream.prototype|.
 *
 * The instance is optimized for operating as a byte stream backed by an
 * embedding-provided underlying source, using the callbacks set via
 * |JS::SetReadableStreamCallbacks|.
 *
 * The given |flags| will be passed to all applicable callbacks and can be
 * used to disambiguate between different types of stream sources the
 * embedding might support.
 *
 * Note: the embedding is responsible for ensuring that the pointer to the
 * underlying source stays valid as long as the stream can be read from.
 * The underlying source can be freed if the tree is canceled or errored.
 * It can also be freed if the stream is destroyed. The embedding is notified
 * of that using ReadableStreamFinalizeCallback.
 */
extern JS_PUBLIC_API(JSObject*)
NewReadableExternalSourceStreamObject(JSContext* cx, void* underlyingSource,
                                      uint8_t flags = 0, HandleObject proto = nullptr);

/**
 * Returns the flags that were passed to NewReadableExternalSourceStreamObject
 * when creating the given stream.
 *
 * Asserts that the given stream has an embedding-provided underlying source.
 */
extern JS_PUBLIC_API(uint8_t)
ReadableStreamGetEmbeddingFlags(const JSObject* stream);

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
 * Asserts that the stream has an embedding-provided underlying source.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamGetExternalUnderlyingSource(JSContext* cx, HandleObject stream, void** source);

/**
 * Releases the embedding-provided underlying source of the given |stream|,
 * returning the stream into an unlocked state.
 *
 * Asserts that the stream was locked through
 * ReadableStreamGetExternalUnderlyingSource.
 *
 * Asserts that the stream has an embedding-provided underlying source.
 */
extern JS_PUBLIC_API(void)
ReadableStreamReleaseExternalUnderlyingSource(JSObject* stream);

/**
 * Update the amount of data available at the underlying source of the given
 * |stream|.
 *
 * Can only be used for streams with an embedding-provided underlying source.
 * The JS engine will use the given value to satisfy read requests for the
 * stream by invoking the JS::WriteIntoReadRequestBuffer callback.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamUpdateDataAvailableFromSource(JSContext* cx, HandleObject stream,
                                            uint32_t availableData);

/**
 * Returns true if the given object is an unwrapped ReadableStream object,
 * false otherwise.
 */
extern JS_PUBLIC_API(bool)
IsReadableStream(const JSObject* obj);

/**
 * Returns true if the given object is an unwrapped
 * ReadableStreamDefaultReader or ReadableStreamBYOBReader object,
 * false otherwise.
 */
extern JS_PUBLIC_API(bool)
IsReadableStreamReader(const JSObject* obj);

/**
 * Returns true if the given object is an unwrapped
 * ReadableStreamDefaultReader object, false otherwise.
 */
extern JS_PUBLIC_API(bool)
IsReadableStreamDefaultReader(const JSObject* obj);

/**
 * Returns true if the given object is an unwrapped
 * ReadableStreamBYOBReader object, false otherwise.
 */
extern JS_PUBLIC_API(bool)
IsReadableStreamBYOBReader(const JSObject* obj);

enum class ReadableStreamMode {
    Default,
    Byte,
    ExternalSource
};

/**
 * Returns the stream's ReadableStreamMode. If the mode is |Byte| or
 * |ExternalSource|, it's possible to acquire a BYOB reader for more optimized
 * operations.
 *
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(ReadableStreamMode)
ReadableStreamGetMode(const JSObject* stream);

enum class ReadableStreamReaderMode {
    Default,
    BYOB
};

/**
 * Returns true if the given ReadableStream is readable, false if not.
 *
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamIsReadable(const JSObject* stream);

/**
 * Returns true if the given ReadableStream is locked, false if not.
 *
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamIsLocked(const JSObject* stream);

/**
 * Returns true if the given ReadableStream is disturbed, false if not.
 *
 * Asserts that |stream| is an ReadableStream instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamIsDisturbed(const JSObject* stream);

/**
 * Cancels the given ReadableStream with the given reason and returns a
 * Promise resolved according to the result.
 *
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(JSObject*)
ReadableStreamCancel(JSContext* cx, HandleObject stream, HandleValue reason);

/**
 * Creates a reader of the type specified by the mode option and locks the
 * stream to the new reader.
 *
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(JSObject*)
ReadableStreamGetReader(JSContext* cx, HandleObject stream, ReadableStreamReaderMode mode);

/**
 * Tees the given ReadableStream and stores the two resulting streams in
 * outparams. Returns false if the operation fails, e.g. because the stream is
 * locked.
 *
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamTee(JSContext* cx, HandleObject stream,
                  MutableHandleObject branch1Stream, MutableHandleObject branch2Stream);

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
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(void)
ReadableStreamGetDesiredSize(JSObject* stream, bool* hasValue, double* value);

/**
 * Closes the given ReadableStream.
 *
 * Throws a TypeError and returns false if the closing operation fails.
 *
 * Note: This is semantically equivalent to the |close| method on
 * the stream controller's prototype in JS. We expose it with the stream
 * itself as a target for simplicity.
 *
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamClose(JSContext* cx, HandleObject stream);

/**
 * Returns true if the given ReadableStream reader is locked, false otherwise.
 *
 * Asserts that |reader| is an unwrapped ReadableStreamDefaultReader or
 * ReadableStreamBYOBReader instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamReaderIsClosed(const JSObject* reader);

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
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamEnqueue(JSContext* cx, HandleObject stream, HandleValue chunk);

/**
 * Enqueues the given buffer as a chunk in the given ReadableStream.
 *
 * Throws a TypeError and returns false if the enqueing operation fails.
 *
 * Note: This is semantically equivalent to the |enqueue| method on
 * the stream controller's prototype in JS. We expose it with the stream
 * itself as a target for simplicity. Additionally, the JS version only
 * takes typed arrays and ArrayBufferView instances as arguments, whereas
 * this takes an ArrayBuffer, obviating the need to wrap it into a typed
 * array.
 *
 * Asserts that |stream| is an unwrapped ReadableStream instance and |buffer|
 * an unwrapped ArrayBuffer instance.
 */
extern JS_PUBLIC_API(bool)
ReadableByteStreamEnqueueBuffer(JSContext* cx, HandleObject stream, HandleObject buffer);

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
 * Asserts that |stream| is an unwrapped ReadableStream instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamError(JSContext* cx, HandleObject stream, HandleValue error);

/**
 * Cancels the given ReadableStream reader's associated stream.
 *
 * Throws a TypeError and returns false if the given reader isn't active.
 *
 * Asserts that |reader| is an unwrapped ReadableStreamDefaultReader or
 * ReadableStreamBYOBReader instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamReaderCancel(JSContext* cx, HandleObject reader, HandleValue reason);

/**
 * Cancels the given ReadableStream reader's associated stream.
 *
 * Throws a TypeError and returns false if the given reader has pending
 * read or readInto (for default or byob readers, respectively) requests.
 *
 * Asserts that |reader| is an unwrapped ReadableStreamDefaultReader or
 * ReadableStreamBYOBReader instance.
 */
extern JS_PUBLIC_API(bool)
ReadableStreamReaderReleaseLock(JSContext* cx, HandleObject reader);

/**
 * Requests a read from the reader's associated ReadableStream and returns the
 * resulting PromiseObject.
 *
 * Returns a Promise that's resolved with the read result once available or
 * rejected immediately if the stream is errored or the operation failed.
 *
 * Asserts that |reader| is an unwrapped ReadableStreamDefaultReader instance.
 */
extern JS_PUBLIC_API(JSObject*)
ReadableStreamDefaultReaderRead(JSContext* cx, HandleObject reader);

/**
 * Requests a read from the reader's associated ReadableStream into the given
 * ArrayBufferView and returns the resulting PromiseObject.
 *
 * Returns a Promise that's resolved with the read result once available or
 * rejected immediately if the stream is errored or the operation failed.
 *
 * Asserts that |reader| is an unwrapped ReadableStreamDefaultReader and
 * |view| an unwrapped typed array or DataView instance.
 */
extern JS_PUBLIC_API(JSObject*)
ReadableStreamBYOBReaderRead(JSContext* cx, HandleObject reader, HandleObject view);

} // namespace JS

#endif // js_Realm_h
