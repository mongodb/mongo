/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SelfHosting_h_
#define vm_SelfHosting_h_

#include "jsapi.h"
#include "NamespaceImports.h"

#include "vm/Stack.h"

namespace js {

/*
 * Check whether the given JSFunction is a self-hosted function whose
 * self-hosted name is the given name.
 */
bool IsSelfHostedFunctionWithName(JSFunction* fun, JSAtom* name);

/*
 * Returns the name of the cloned function's binding in the self-hosted global.
 *
 * This returns a non-null value only when this is a top level function
 * declaration in the self-hosted global.
 */
PropertyName* GetClonedSelfHostedFunctionName(const JSFunction* fun);

/*
 * Same as GetClonedSelfHostedFunctionName, but `fun` is guaranteed to be an
 * extended function.
 *
 * This function is supposed to be used off-thread, especially the JIT
 * compilation thread, that cannot access JSFunction.flags_, because of
 * a race condition.
 *
 * See Also: WrappedFunction.isExtended_
 */
PropertyName* GetClonedSelfHostedFunctionNameOffMainThread(JSFunction* fun);

constexpr char ExtendedUnclonedSelfHostedFunctionNamePrefix = '$';

/*
 * Uncloned self-hosted functions with `$` prefix are allocated as
 * extended function, to store the original name in `_SetCanonicalName`.
 */
bool IsExtendedUnclonedSelfHostedFunctionName(JSAtom* name);

void SetUnclonedSelfHostedCanonicalName(JSFunction* fun, JSAtom* name);

bool IsCallSelfHostedNonGenericMethod(NativeImpl impl);

bool ReportIncompatibleSelfHostedMethod(JSContext* cx, const CallArgs& args);

/* Get the compile options used when compiling self hosted code. */
void FillSelfHostingCompileOptions(JS::CompileOptions& options);

#ifdef DEBUG
/*
 * Calls a self-hosted function by name.
 *
 * This function is only available in debug mode, because it always atomizes
 * its |name| parameter. Use the alternative function below in non-debug code.
 */
bool CallSelfHostedFunction(JSContext* cx, char const* name, HandleValue thisv,
                            const AnyInvokeArgs& args, MutableHandleValue rval);
#endif

/*
 * Calls a self-hosted function by name.
 */
bool CallSelfHostedFunction(JSContext* cx, HandlePropertyName name,
                            HandleValue thisv, const AnyInvokeArgs& args,
                            MutableHandleValue rval);

bool intrinsic_NewArrayIterator(JSContext* cx, unsigned argc, JS::Value* vp);

bool intrinsic_NewStringIterator(JSContext* cx, unsigned argc, JS::Value* vp);

bool intrinsic_NewRegExpStringIterator(JSContext* cx, unsigned argc,
                                       JS::Value* vp);

} /* namespace js */

#endif /* vm_SelfHosting_h_ */
