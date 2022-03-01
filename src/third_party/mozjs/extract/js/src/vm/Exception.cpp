/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Exception.h"

#include "jsapi.h"  // AssertHeapIsIdle
#include "vm/JSContext.h"
#include "vm/SavedFrame.h"

using namespace js;

bool JS::StealPendingExceptionStack(JSContext* cx,
                                    JS::ExceptionStack* exceptionStack) {
  if (!GetPendingExceptionStack(cx, exceptionStack)) {
    return false;
  }

  // "Steal" exception by clearing it.
  cx->clearPendingException();
  return true;
}

bool JS::GetPendingExceptionStack(JSContext* cx,
                                  JS::ExceptionStack* exceptionStack) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  MOZ_ASSERT(exceptionStack);
  MOZ_ASSERT(cx->isExceptionPending());

  RootedValue exception(cx);
  if (!cx->getPendingException(&exception)) {
    return false;
  }

  RootedObject stack(cx, cx->getPendingExceptionStack());
  exceptionStack->init(exception, stack);
  return true;
}

void JS::SetPendingExceptionStack(JSContext* cx,
                                  const JS::ExceptionStack& exceptionStack) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  // We don't check the compartments of `exception` and `stack` here,
  // because we're not doing anything with them other than storing
  // them, and stored exception values can be in an abitrary
  // compartment while stored stack values are always the unwrapped
  // object anyway.

  RootedSavedFrame nstack(cx);
  if (exceptionStack.stack()) {
    nstack = &UncheckedUnwrap(exceptionStack.stack())->as<SavedFrame>();
  }
  cx->setPendingException(exceptionStack.exception(), nstack);
}
