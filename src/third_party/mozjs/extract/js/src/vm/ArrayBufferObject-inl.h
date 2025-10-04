/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayBufferObject_inl_h
#define vm_ArrayBufferObject_inl_h

// Utilities and common inline code for ArrayBufferObject and
// SharedArrayBufferObject.

#include "vm/ArrayBufferObject.h"

#include "vm/SharedArrayObject.h"
#include "vm/SharedMem.h"

namespace js {

inline SharedMem<uint8_t*> ArrayBufferObjectMaybeShared::dataPointerEither() {
  if (this->is<ArrayBufferObject>()) {
    return this->as<ArrayBufferObject>().dataPointerShared();
  }
  return this->as<SharedArrayBufferObject>().dataPointerShared();
}

inline bool ArrayBufferObjectMaybeShared::isDetached() const {
  if (this->is<ArrayBufferObject>()) {
    return this->as<ArrayBufferObject>().isDetached();
  }
  return false;
}

inline bool ArrayBufferObjectMaybeShared::isResizable() const {
  if (this->is<ArrayBufferObject>()) {
    return this->as<ArrayBufferObject>().isResizable();
  }
  return this->as<SharedArrayBufferObject>().isGrowable();
}

inline size_t ArrayBufferObjectMaybeShared::byteLength() const {
  if (this->is<ArrayBufferObject>()) {
    return this->as<ArrayBufferObject>().byteLength();
  }
  return this->as<SharedArrayBufferObject>().byteLength();
}

inline bool ArrayBufferObjectMaybeShared::isPreparedForAsmJS() const {
  if (this->is<ArrayBufferObject>()) {
    return this->as<ArrayBufferObject>().isPreparedForAsmJS();
  }
  return false;
}

inline bool ArrayBufferObjectMaybeShared::isWasm() const {
  if (this->is<ArrayBufferObject>()) {
    return this->as<ArrayBufferObject>().isWasm();
  }
  return this->as<SharedArrayBufferObject>().isWasm();
}

inline bool ArrayBufferObjectMaybeShared::pinLength(bool pin) {
  if (is<ArrayBufferObject>()) {
    return as<ArrayBufferObject>().pinLength(pin);
  }
  return false;  // Cannot pin or unpin shared array buffers.
}

}  // namespace js

#endif  // vm_ArrayBufferObject_inl_h
