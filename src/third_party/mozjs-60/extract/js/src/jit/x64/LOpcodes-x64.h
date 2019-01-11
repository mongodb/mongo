/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_LOpcodes_x64_h
#define jit_x64_LOpcodes_x64_h

#include "jit/shared/LOpcodes-shared.h"

#define LIR_CPU_OPCODE_LIST(_)      \
    _(DivOrModConstantI)            \
    _(DivOrModI64)                  \
    _(UDivOrModI64)                 \
    _(WasmTruncateToInt64)          \
    _(Int64ToFloatingPoint)         \
    _(SimdValueInt32x4)             \
    _(SimdValueFloat32x4)           \
    _(UDivOrMod)                    \
    _(UDivOrModConstant)

#endif /* jit_x64_LOpcodes_x64_h */
