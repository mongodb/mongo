/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ErrorObject_inl_h
#define vm_ErrorObject_inl_h

#include "vm/ErrorObject.h"

#include "vm/JSAtomState.h"
#include "vm/JSContext.h"

inline JSString* js::ErrorObject::fileName(JSContext* cx) const {
  Value val = getReservedSlot(FILENAME_SLOT);
  return val.isString() ? val.toString() : cx->names().empty;
}

inline uint32_t js::ErrorObject::sourceId() const {
  Value val = getReservedSlot(SOURCEID_SLOT);
  return val.isInt32() ? val.toInt32() : 0;
}

inline uint32_t js::ErrorObject::lineNumber() const {
  Value val = getReservedSlot(LINENUMBER_SLOT);
  return val.isInt32() ? val.toInt32() : 0;
}

inline uint32_t js::ErrorObject::columnNumber() const {
  Value val = getReservedSlot(COLUMNNUMBER_SLOT);
  return val.isInt32() ? val.toInt32() : 0;
}

inline JSObject* js::ErrorObject::stack() const {
  return getReservedSlot(STACK_SLOT).toObjectOrNull();
}

#endif /* vm_ErrorObject_inl_h */
