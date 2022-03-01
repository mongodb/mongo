/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Base stream controller inlines. */

#ifndef builtin_streams_StreamController_inl_h
#define builtin_streams_StreamController_inl_h

#include "builtin/streams/StreamController.h"  // js::StreamController
#include "builtin/streams/ReadableStreamController.h"  // js::Readable{ByteStream,StreamDefault}Controller
#include "builtin/streams/WritableStreamDefaultController.h"  // js::WritableStreamDefaultController
#include "vm/JSObject.h"                                      // JSObject

template <>
inline bool JSObject::is<js::StreamController>() const {
  return is<js::ReadableStreamDefaultController>() ||
         is<js::ReadableByteStreamController>() ||
         is<js::WritableStreamDefaultController>();
}

#endif  // builtin_streams_ReadableStreamController_inl_h
