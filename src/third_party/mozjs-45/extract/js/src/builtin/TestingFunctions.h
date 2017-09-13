/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_TestingFunctions_h
#define builtin_TestingFunctions_h

#include "NamespaceImports.h"

namespace js {

bool
DefineTestingFunctions(JSContext* cx, HandleObject obj, bool fuzzingSafe, bool disableOOMFunctions);

bool
testingFunc_assertFloat32(JSContext* cx, unsigned argc, Value* vp);

bool
testingFunc_assertRecoveredOnBailout(JSContext* cx, unsigned argc, Value* vp);

} /* namespace js */

#endif /* builtin_TestingFunctions_h */
