/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_LOpcodes_mips32_h__
#define jit_mips32_LOpcodes_mips32_h__

#include "jit/shared/LOpcodes-shared.h"

#define LIR_CPU_OPCODE_LIST(_)  \
    _(BoxFloatingPoint)         \
    _(ModMaskI)                 \
    _(UDivOrMod)                \
    _(DivOrModI64)              \
    _(UDivOrModI64)             \
    _(WasmUnalignedLoad)        \
    _(WasmUnalignedStore)       \
    _(WasmUnalignedLoadI64)     \
    _(WasmUnalignedStoreI64)    \
    _(WasmTruncateToInt64)      \
    _(Int64ToFloatingPoint)     \
    _(WasmCompareExchangeI64)   \
    _(WasmAtomicExchangeI64)    \
    _(WasmAtomicBinopI64)       \
    _(WasmAtomicLoadI64)        \
    _(WasmAtomicStoreI64)       \

#endif // jit_mips32_LOpcodes_mips32_h__
