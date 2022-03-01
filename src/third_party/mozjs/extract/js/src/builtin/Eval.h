/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Eval_h
#define builtin_Eval_h

#include "NamespaceImports.h"

#include "js/TypeDecls.h"

namespace js {

// The C++ native for 'eval' (ES5 15.1.2.1). The function is named "indirect
// eval" because "direct eval" calls (as defined by the spec) will emit
// JSOp::Eval which in turn calls DirectEval. Thus, even though IndirectEval is
// the callee function object for *all* calls to eval, it is by construction
// only ever called in the case indirect eval.
[[nodiscard]] extern bool IndirectEval(JSContext* cx, unsigned argc, Value* vp);

// Performs a direct eval of |v| (a string containing code, or another value
// that will be vacuously returned), which must correspond to the currently-
// executing stack frame, which must be a script frame.
[[nodiscard]] extern bool DirectEval(JSContext* cx, HandleValue v,
                                     MutableHandleValue vp);

// True iff fun is a built-in eval function.
extern bool IsAnyBuiltinEval(JSFunction* fun);

}  // namespace js

#endif /* builtin_Eval_h */
