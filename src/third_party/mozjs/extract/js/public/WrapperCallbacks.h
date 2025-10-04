/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WrapperCallbacks_h
#define js_WrapperCallbacks_h

#include "js/TypeDecls.h"

/**
 * Callback used to ask the embedding for the cross compartment wrapper handler
 * that implements the desired prolicy for this kind of object in the
 * destination compartment. |obj| is the object to be wrapped. If |existing| is
 * non-nullptr, it will point to an existing wrapper object that should be
 * re-used if possible. |existing| is guaranteed to be a cross-compartment
 * wrapper with a lazily-defined prototype and the correct global. It is
 * guaranteed not to wrap a function.
 */
using JSWrapObjectCallback = JSObject* (*)(JSContext*, JS::HandleObject,
                                           JS::HandleObject);

/**
 * Callback used by the wrap hook to ask the embedding to prepare an object
 * for wrapping in a context. This might include unwrapping other wrappers
 * or even finding a more suitable object for the new compartment. If |origObj|
 * is non-null, then it is the original object we are going to swap into during
 * a transplant.
 */
using JSPreWrapCallback = void (*)(JSContext*, JS::HandleObject,
                                   JS::HandleObject, JS::HandleObject,
                                   JS::HandleObject, JS::MutableHandleObject);

struct JSWrapObjectCallbacks {
  JSWrapObjectCallback wrap;
  JSPreWrapCallback preWrap;
};

#endif  // js_WrapperCallbacks_h
