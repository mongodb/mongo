/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_TestingFunctions_h
#define builtin_TestingFunctions_h

#include "NamespaceImports.h"  // JSContext, JSFunction, HandleObject, HandleValue, Value

namespace js {

[[nodiscard]] bool InitTestingFunctions();

[[nodiscard]] bool DefineTestingFunctions(JSContext* cx, HandleObject obj,
                                          bool fuzzingSafe,
                                          bool disableOOMFunctions);

[[nodiscard]] bool testingFunc_assertFloat32(JSContext* cx, unsigned argc,
                                             Value* vp);

[[nodiscard]] bool testingFunc_assertRecoveredOnBailout(JSContext* cx,
                                                        unsigned argc,
                                                        Value* vp);

[[nodiscard]] bool testingFunc_serialize(JSContext* cx, unsigned argc,
                                         Value* vp);

extern JSScript* TestingFunctionArgumentToScript(JSContext* cx, HandleValue v,
                                                 JSFunction** funp = nullptr);

} /* namespace js */

#endif /* builtin_TestingFunctions_h */
