/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AutoJitContextAlloc_h
#define jit_AutoJitContextAlloc_h

#include "mozilla/Assertions.h"

#include "ds/LifoAlloc.h"
#include "jit/JitAllocPolicy.h"
#include "jit/JitContext.h"

namespace js::jit {

class AutoJitContextAlloc {
  TempAllocator tempAlloc_;
  JitContext* jcx_;
  TempAllocator* prevAlloc_;

 public:
  explicit AutoJitContextAlloc(LifoAlloc* lifoAlloc)
      : tempAlloc_(lifoAlloc), jcx_(GetJitContext()), prevAlloc_(jcx_->temp) {
    jcx_->temp = &tempAlloc_;
  }

  ~AutoJitContextAlloc() {
    MOZ_ASSERT(jcx_->temp == &tempAlloc_);
    jcx_->temp = prevAlloc_;
  }
};

}  // namespace js::jit

#endif /* jit_AutoJitContextAlloc_h */
