/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Promise_inl_h
#define builtin_Promise_inl_h

#include "js/Promise.h"  // JS::PromiseState

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "js/RootingAPI.h"     // JS::Handle
#include "vm/JSContext.h"      // JSContext
#include "vm/PromiseObject.h"  // js::PromiseObject

namespace js {

/**
 * Given a settled (i.e. fulfilled or rejected, not pending) promise, sets
 * |promise.[[PromiseIsHandled]]| to true and removes it from the list of
 * unhandled rejected promises.
 *
 * NOTE: If you need to set |promise.[[PromiseIsHandled]]| on a pending promise,
 *       use |PromiseObject::setHandled()| directly.
 */
inline void SetSettledPromiseIsHandled(
    JSContext* cx, JS::Handle<PromiseObject*> unwrappedPromise) {
  MOZ_ASSERT(unwrappedPromise->state() != JS::PromiseState::Pending);
  unwrappedPromise->setHandled();
  cx->runtime()->removeUnhandledRejectedPromise(cx, unwrappedPromise);
}

}  // namespace js

#endif  // builtin_Promise_inl_h
