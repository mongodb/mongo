/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "threading/Mutex.h"

#include "jsutil.h"

using namespace js;

#ifdef DEBUG

MOZ_THREAD_LOCAL(js::Mutex::MutexVector*) js::Mutex::HeldMutexStack;

/* static */ bool
js::Mutex::Init()
{
  return HeldMutexStack.init();
}

/* static */ void
js::Mutex::ShutDown()
{
  js_delete(HeldMutexStack.get());
  HeldMutexStack.set(nullptr);
}

/* static */ js::Mutex::MutexVector&
js::Mutex::heldMutexStack()
{
  MOZ_ASSERT(js::IsInitialized());
  auto stack = HeldMutexStack.get();
  if (!stack) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    stack = js_new<MutexVector>();
    if (!stack)
      oomUnsafe.crash("js::Mutex::heldMutexStack");
    HeldMutexStack.set(stack);
  }
  return *stack;
}

void
js::Mutex::lock()
{
  auto& stack = heldMutexStack();
  if (!stack.empty()) {
    const Mutex& prev = *stack.back();
    if (id_.order <= prev.id_.order) {
      fprintf(stderr,
              "Attempt to acquire mutex %s with order %d while holding %s with order %d\n",
              id_.name, id_.order, prev.id_.name, prev.id_.order);
      MOZ_CRASH("Mutex ordering violation");
    }
  }

  MutexImpl::lock();

  AutoEnterOOMUnsafeRegion oomUnsafe;
  if (!stack.append(this))
    oomUnsafe.crash("js::Mutex::lock");
}

void
js::Mutex::unlock()
{
  auto& stack = heldMutexStack();
  MOZ_ASSERT(stack.back() == this);
  MutexImpl::unlock();
  stack.popBack();
}

bool
js::Mutex::ownedByCurrentThread() const
{
  auto& stack = heldMutexStack();
  for (size_t i = 0; i < stack.length(); i++) {
    if (stack[i] == this)
      return true;
  }
  return false;
}

#endif
