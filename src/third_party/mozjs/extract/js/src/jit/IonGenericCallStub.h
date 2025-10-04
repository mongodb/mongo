/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonGenericCallStub_h
#define jit_IonGenericCallStub_h

#include "jit/Registers.h"

namespace js::jit {

#ifndef JS_USE_LINK_REGISTER
static constexpr Register IonGenericCallReturnAddrReg = CallTempReg0;
#endif

static constexpr Register IonGenericCallCalleeReg = CallTempReg1;
static constexpr Register IonGenericCallArgcReg = CallTempReg2;
static constexpr Register IonGenericCallScratch = CallTempReg3;
static constexpr Register IonGenericCallScratch2 = CallTempReg4;
static constexpr Register IonGenericCallScratch3 = CallTempReg5;

#ifdef JS_CODEGEN_ARM
// We need a second scratch register that does not alias `lr` or
// any of the registers the ABI uses to pass arguments.
static_assert(CallTempReg0 == CallTempNonArgRegs[0]);
static constexpr Register IonGenericSecondScratchReg = CallTempReg0;
#endif

}  // namespace js::jit

#endif /* jit_IonGenericCallStub_h */
