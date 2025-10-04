/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ScriptFromCalleeToken_h
#define jit_ScriptFromCalleeToken_h

#include "mozilla/Assertions.h"

#include "jit/CalleeToken.h"
#include "js/TypeDecls.h"
#include "vm/JSFunction.h"

namespace js::jit {

static inline JSScript* ScriptFromCalleeToken(CalleeToken token) {
  switch (GetCalleeTokenTag(token)) {
    case CalleeToken_Script:
      return CalleeTokenToScript(token);
    case CalleeToken_Function:
    case CalleeToken_FunctionConstructing:
      return CalleeTokenToFunction(token)->nonLazyScript();
  }
  MOZ_CRASH("invalid callee token tag");
}

JSScript* MaybeForwardedScriptFromCalleeToken(CalleeToken token);

} /* namespace js::jit */

#endif /* jit_ScriptFromCalleeToken_h */
