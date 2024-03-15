/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_riscv64_constant_Constant_riscv64_h
#define jit_riscv64_constant_Constant_riscv64_h
#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

#include <stdio.h>

#include "jit/riscv64/constant/Base-constant-riscv.h"
#include "jit/riscv64/constant/Constant-riscv-a.h"
#include "jit/riscv64/constant/Constant-riscv-c.h"
#include "jit/riscv64/constant/Constant-riscv-d.h"
#include "jit/riscv64/constant/Constant-riscv-f.h"
#include "jit/riscv64/constant/Constant-riscv-i.h"
#include "jit/riscv64/constant/Constant-riscv-m.h"
#include "jit/riscv64/constant/Constant-riscv-v.h"
#include "jit/riscv64/constant/Constant-riscv-zicsr.h"
#include "jit/riscv64/constant/Constant-riscv-zifencei.h"

namespace js {
namespace jit {

// A reasonable (ie, safe) buffer size for the disassembly of a single
// instruction.
const int ReasonableBufferSize = 256;

// Difference between address of current opcode and value read from pc
// register.
static constexpr int kPcLoadDelta = 4;

// Bits available for offset field in branches
static constexpr int kBranchOffsetBits = 13;

// Bits available for offset field in jump
static constexpr int kJumpOffsetBits = 21;

// Bits available for offset field in compresed jump
static constexpr int kCJalOffsetBits = 12;

// Bits available for offset field in 4 branch
static constexpr int kCBranchOffsetBits = 9;

// Max offset for b instructions with 12-bit offset field (multiple of 2)
static constexpr int kMaxBranchOffset = (1 << (kBranchOffsetBits - 1)) - 1;

static constexpr int kCBranchOffset = (1 << (kCBranchOffsetBits - 1)) - 1;
// Max offset for jal instruction with 20-bit offset field (multiple of 2)
static constexpr int kMaxJumpOffset = (1 << (kJumpOffsetBits - 1)) - 1;

static constexpr int kCJumpOffset = (1 << (kCJalOffsetBits - 1)) - 1;

static constexpr int kTrampolineSlotsSize = 2 * kInstrSize;

static_assert(kCJalOffsetBits == kOffset12);
static_assert(kCBranchOffsetBits == kOffset9);
static_assert(kJumpOffsetBits == kOffset21);
static_assert(kBranchOffsetBits == kOffset13);
// Vector as used by the original code to allow for minimal modification.
// Functions exactly like a character array with helper methods.
}  // namespace jit
}  // namespace js

#endif  // jit_riscv64_constant_Constant_riscv64_h
