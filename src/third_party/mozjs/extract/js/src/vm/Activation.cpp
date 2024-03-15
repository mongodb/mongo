/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Activation-inl.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "gc/GC.h"            // js::gc::AutoSuppressGC
#include "jit/CalleeToken.h"  // js::jit::CalleeToken{IsFunction,To{Function,Script}}
#include "js/RootingAPI.h"  // JS::Rooted
#include "js/Value.h"       // JS::Value
#include "vm/JSContext.h"   // JSContext, js::TlsContext
#include "vm/Stack.h"       // js::InterpreterFrame

#include "vm/Compartment-inl.h"  // JS::Compartment::wrap

using namespace js;

using JS::ObjectOrNullValue;
using JS::Rooted;
using JS::UndefinedValue;
using JS::Value;

Value ActivationEntryMonitor::asyncStack(JSContext* cx) {
  Rooted<Value> stack(cx, ObjectOrNullValue(cx->asyncStackForNewActivations()));
  if (!cx->compartment()->wrap(cx, &stack)) {
    cx->clearPendingException();
    return UndefinedValue();
  }
  return stack;
}

void ActivationEntryMonitor::init(JSContext* cx, InterpreterFrame* entryFrame) {
  // The InterpreterFrame is not yet part of an Activation, so it won't
  // be traced if we trigger GC here. Suppress GC to avoid this.
  gc::AutoSuppressGC suppressGC(cx);
  Rooted<Value> stack(cx, asyncStack(cx));
  const char* asyncCause = cx->asyncCauseForNewActivations;
  if (entryFrame->isFunctionFrame()) {
    entryMonitor_->Entry(cx, &entryFrame->callee(), stack, asyncCause);
  } else {
    entryMonitor_->Entry(cx, entryFrame->script(), stack, asyncCause);
  }
}

void ActivationEntryMonitor::init(JSContext* cx, jit::CalleeToken entryToken) {
  // The CalleeToken is not traced at this point and we also don't want
  // a GC to discard the code we're about to enter, so we suppress GC.
  gc::AutoSuppressGC suppressGC(cx);
  RootedValue stack(cx, asyncStack(cx));
  const char* asyncCause = cx->asyncCauseForNewActivations;
  if (jit::CalleeTokenIsFunction(entryToken)) {
    entryMonitor_->Entry(cx_, jit::CalleeTokenToFunction(entryToken), stack,
                         asyncCause);
  } else {
    entryMonitor_->Entry(cx_, jit::CalleeTokenToScript(entryToken), stack,
                         asyncCause);
  }
}

void Activation::registerProfiling() {
  MOZ_ASSERT(isProfiling());
  cx_->profilingActivation_ = this;
}

void Activation::unregisterProfiling() {
  MOZ_ASSERT(isProfiling());
  MOZ_ASSERT(cx_->profilingActivation_ == this);
  cx_->profilingActivation_ = prevProfiling_;
}

ActivationIterator::ActivationIterator(JSContext* cx)
    : activation_(cx->activation_) {
  MOZ_ASSERT(cx == TlsContext.get());
}

ActivationIterator& ActivationIterator::operator++() {
  MOZ_ASSERT(activation_);
  activation_ = activation_->prev();
  return *this;
}
