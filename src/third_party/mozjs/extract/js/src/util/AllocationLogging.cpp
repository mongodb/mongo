/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/AllocationLogging.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

static JS::LogCtorDtor sLogCtor = nullptr;
static JS::LogCtorDtor sLogDtor = nullptr;

void JS::SetLogCtorDtorFunctions(LogCtorDtor ctor, LogCtorDtor dtor) {
  MOZ_ASSERT(!sLogCtor && !sLogDtor);
  MOZ_ASSERT(ctor && dtor);
  sLogCtor = ctor;
  sLogDtor = dtor;
}

void JS::LogCtor(void* self, const char* type, uint32_t sz) {
  if (LogCtorDtor fun = sLogCtor) {
    fun(self, type, sz);
  }
}

void JS::LogDtor(void* self, const char* type, uint32_t sz) {
  if (LogCtorDtor fun = sLogDtor) {
    fun(self, type, sz);
  }
}
