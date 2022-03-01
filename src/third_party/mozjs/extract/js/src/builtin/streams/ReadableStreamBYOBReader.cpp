/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Class ReadableStreamBYOBReader.
 *
 * Byte streams and BYOB readers are unimplemented, so this is skeletal -- yet
 * helpful to ensure certain trivial tests of the functionality in wpt, that
 * don't actually test fully-constructed byte streams/BYOB readers, pass.  🙄
 */

#include "jsapi.h"  // JS_ReportErrorNumberASCII

#include "builtin/streams/ReadableStream.h"  // js::ReadableStream
#include "builtin/streams/ReadableStreamReader.h"  // js::CreateReadableStreamBYOBReader, js::ForAuthorCodeBool
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*

using JS::Handle;

/*** 3.7. Class ReadableStreamBYOBReader *********************************/

/**
 * Stream spec, 3.7.3. new ReadableStreamBYOBReader ( stream )
 * Steps 2-5.
 */
[[nodiscard]] JSObject* js::CreateReadableStreamBYOBReader(
    JSContext* cx, Handle<ReadableStream*> unwrappedStream,
    ForAuthorCodeBool forAuthorCode, Handle<JSObject*> proto /* = nullptr */) {
  // Step 2: If ! IsReadableByteStreamController(
  //                  stream.[[readableStreamController]]) is false, throw a
  //         TypeError exception.
  // We don't implement byte stream controllers yet, so always throw here.  Note
  // that JSMSG_READABLESTREAM_BYTES_TYPE_NOT_IMPLEMENTED can't be used here
  // because it's a RangeError (and sadly wpt actually tests this and we have a
  // spurious failure if we don't make this a TypeError).
  JS_ReportErrorNumberASCII(
      cx, GetErrorMessage, nullptr,
      JSMSG_READABLESTREAM_BYOB_READER_FOR_NON_BYTE_STREAM);

  // Step 3: If ! IsReadableStreamLocked(stream) is true, throw a TypeError
  //         exception.
  // Step 4: Perform ! ReadableStreamReaderGenericInitialize(this, stream).
  // Step 5: Set this.[[readIntoRequests]] to a new empty List.
  // Steps 3-5 are presently unreachable.
  return nullptr;
}
