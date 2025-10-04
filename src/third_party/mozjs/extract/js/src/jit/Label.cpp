/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Label.h"

#include "mozilla/Assertions.h"

#include "jit/CompileWrappers.h"
#include "jit/JitContext.h"
#include "js/Utility.h"

namespace js::jit {

#ifdef DEBUG
Label::~Label() {
  // The assertion below doesn't hold if an error occurred.
  JitContext* context = MaybeGetJitContext();
  bool hadError =
      js::oom::HadSimulatedOOM() ||
      (context && context->runtime && context->runtime->hadOutOfMemory()) ||
      (context && !context->runtime && context->hasOOM());
  MOZ_ASSERT_IF(!hadError, !used());
}
#endif

}  // namespace js::jit
