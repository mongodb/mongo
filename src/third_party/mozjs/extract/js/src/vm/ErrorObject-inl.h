/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ErrorObject_inl_h
#define vm_ErrorObject_inl_h

#include "vm/ErrorObject.h"

#include "js/ColumnNumber.h"  // JS::ColumnNumberOneOrigin

#include "vm/JSAtomState.h"
#include "vm/JSContext.h"

inline JSString* js::ErrorObject::fileName(JSContext* cx) const {
  Value val = getReservedSlot(FILENAME_SLOT);
  return val.isString() ? val.toString() : cx->names().empty_;
}

inline uint32_t js::ErrorObject::sourceId() const {
  Value val = getReservedSlot(SOURCEID_SLOT);
  return val.isInt32() ? val.toInt32() : 0;
}

inline uint32_t js::ErrorObject::lineNumber() const {
  Value val = getReservedSlot(LINENUMBER_SLOT);
  return val.isInt32() ? val.toInt32() : 0;
}

inline JS::ColumnNumberOneOrigin js::ErrorObject::columnNumber() const {
  Value val = getReservedSlot(COLUMNNUMBER_SLOT);
  // If Error object's `columnNumber` property is modified from JS code,
  // COLUMNNUMBER_SLOT slot can contain non-int32 value.
  // Use column number 1 as fallback value for such case.
  return val.isInt32() ? JS::ColumnNumberOneOrigin(val.toInt32())
                       : JS::ColumnNumberOneOrigin();
}

inline JSObject* js::ErrorObject::stack() const {
  // If the stack was a CCW, it might have been turned into a dead object proxy
  // by NukeCrossCompartmentWrapper. Return nullptr in this case.
  JSObject* obj = getReservedSlot(STACK_SLOT).toObjectOrNull();
  if (obj && obj->canUnwrapAs<SavedFrame>()) {
    return obj;
  }
  return nullptr;
}

#endif /* vm_ErrorObject_inl_h */
