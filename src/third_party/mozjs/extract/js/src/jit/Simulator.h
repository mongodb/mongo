/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Simulator_h
#define jit_Simulator_h

#if defined(JS_SIMULATOR_ARM)
#  include "jit/arm/Simulator-arm.h"
#elif defined(JS_SIMULATOR_ARM64)
#  include "jit/arm64/vixl/Simulator-vixl.h"
#elif defined(JS_SIMULATOR_MIPS32)
#  include "jit/mips32/Simulator-mips32.h"
#elif defined(JS_SIMULATOR_MIPS64)
#  include "jit/mips64/Simulator-mips64.h"
#elif defined(JS_SIMULATOR_LOONG64)
#  include "jit/loong64/Simulator-loong64.h"
#elif defined(JS_SIMULATOR_RISCV64)
#  include "jit/riscv64/Simulator-riscv64.h"
#elif defined(JS_SIMULATOR)
#  error "Unexpected simulator platform"
#endif

#if defined(JS_SIMULATOR_ARM64)
namespace js::jit {
using Simulator = vixl::Simulator;
}
#endif

#endif /* jit_Simulator_h */
