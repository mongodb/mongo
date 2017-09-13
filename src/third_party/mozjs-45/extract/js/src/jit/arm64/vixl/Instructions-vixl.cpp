// Copyright 2015, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "jit/arm64/vixl/Instructions-vixl.h"

#include "jit/arm64/vixl/Assembler-vixl.h"

namespace vixl {


// Floating-point infinity values.
const float16 kFP16PositiveInfinity = 0x7c00;
const float16 kFP16NegativeInfinity = 0xfc00;
const float kFP32PositiveInfinity = rawbits_to_float(0x7f800000);
const float kFP32NegativeInfinity = rawbits_to_float(0xff800000);
const double kFP64PositiveInfinity =
    rawbits_to_double(UINT64_C(0x7ff0000000000000));
const double kFP64NegativeInfinity =
    rawbits_to_double(UINT64_C(0xfff0000000000000));


// The default NaN values (for FPCR.DN=1).
const double kFP64DefaultNaN = rawbits_to_double(UINT64_C(0x7ff8000000000000));
const float kFP32DefaultNaN = rawbits_to_float(0x7fc00000);
const float16 kFP16DefaultNaN = 0x7e00;


static uint64_t RotateRight(uint64_t value,
                            unsigned int rotate,
                            unsigned int width) {
  VIXL_ASSERT(width <= 64);
  rotate &= 63;
  return ((value & ((UINT64_C(1) << rotate) - 1)) <<
          (width - rotate)) | (value >> rotate);
}


static uint64_t RepeatBitsAcrossReg(unsigned reg_size,
                                    uint64_t value,
                                    unsigned width) {
  VIXL_ASSERT((width == 2) || (width == 4) || (width == 8) || (width == 16) ||
              (width == 32));
  VIXL_ASSERT((reg_size == kWRegSize) || (reg_size == kXRegSize));
  uint64_t result = value & ((UINT64_C(1) << width) - 1);
  for (unsigned i = width; i < reg_size; i *= 2) {
    result |= (result << i);
  }
  return result;
}


bool Instruction::IsLoad() const {
  if (Mask(LoadStoreAnyFMask) != LoadStoreAnyFixed) {
    return false;
  }

  if (Mask(LoadStorePairAnyFMask) == LoadStorePairAnyFixed) {
    return Mask(LoadStorePairLBit) != 0;
  } else {
    LoadStoreOp op = static_cast<LoadStoreOp>(Mask(LoadStoreMask));
    switch (op) {
      case LDRB_w:
      case LDRH_w:
      case LDR_w:
      case LDR_x:
      case LDRSB_w:
      case LDRSB_x:
      case LDRSH_w:
      case LDRSH_x:
      case LDRSW_x:
      case LDR_b:
      case LDR_h:
      case LDR_s:
      case LDR_d:
      case LDR_q: return true;
      default: return false;
    }
  }
}


bool Instruction::IsStore() const {
  if (Mask(LoadStoreAnyFMask) != LoadStoreAnyFixed) {
    return false;
  }

  if (Mask(LoadStorePairAnyFMask) == LoadStorePairAnyFixed) {
    return Mask(LoadStorePairLBit) == 0;
  } else {
    LoadStoreOp op = static_cast<LoadStoreOp>(Mask(LoadStoreMask));
    switch (op) {
      case STRB_w:
      case STRH_w:
      case STR_w:
      case STR_x:
      case STR_b:
      case STR_h:
      case STR_s:
      case STR_d:
      case STR_q: return true;
      default: return false;
    }
  }
}


// Logical immediates can't encode zero, so a return value of zero is used to
// indicate a failure case. Specifically, where the constraints on imm_s are
// not met.
uint64_t Instruction::ImmLogical() const {
  unsigned reg_size = SixtyFourBits() ? kXRegSize : kWRegSize;
  int32_t n = BitN();
  int32_t imm_s = ImmSetBits();
  int32_t imm_r = ImmRotate();

  // An integer is constructed from the n, imm_s and imm_r bits according to
  // the following table:
  //
  //  N   imms    immr    size        S             R
  //  1  ssssss  rrrrrr    64    UInt(ssssss)  UInt(rrrrrr)
  //  0  0sssss  xrrrrr    32    UInt(sssss)   UInt(rrrrr)
  //  0  10ssss  xxrrrr    16    UInt(ssss)    UInt(rrrr)
  //  0  110sss  xxxrrr     8    UInt(sss)     UInt(rrr)
  //  0  1110ss  xxxxrr     4    UInt(ss)      UInt(rr)
  //  0  11110s  xxxxxr     2    UInt(s)       UInt(r)
  // (s bits must not be all set)
  //
  // A pattern is constructed of size bits, where the least significant S+1
  // bits are set. The pattern is rotated right by R, and repeated across a
  // 32 or 64-bit value, depending on destination register width.
  //

  if (n == 1) {
    if (imm_s == 0x3f) {
      return 0;
    }
    uint64_t bits = (UINT64_C(1) << (imm_s + 1)) - 1;
    return RotateRight(bits, imm_r, 64);
  } else {
    if ((imm_s >> 1) == 0x1f) {
      return 0;
    }
    for (int width = 0x20; width >= 0x2; width >>= 1) {
      if ((imm_s & width) == 0) {
        int mask = width - 1;
        if ((imm_s & mask) == mask) {
          return 0;
        }
        uint64_t bits = (UINT64_C(1) << ((imm_s & mask) + 1)) - 1;
        return RepeatBitsAcrossReg(reg_size,
                                   RotateRight(bits, imm_r & mask, width),
                                   width);
      }
    }
  }
  VIXL_UNREACHABLE();
  return 0;
}


uint32_t Instruction::ImmNEONabcdefgh() const {
  return ImmNEONabc() << 5 | ImmNEONdefgh();
}


float Instruction::Imm8ToFP32(uint32_t imm8) {
  //   Imm8: abcdefgh (8 bits)
  // Single: aBbb.bbbc.defg.h000.0000.0000.0000.0000 (32 bits)
  // where B is b ^ 1
  uint32_t bits = imm8;
  uint32_t bit7 = (bits >> 7) & 0x1;
  uint32_t bit6 = (bits >> 6) & 0x1;
  uint32_t bit5_to_0 = bits & 0x3f;
  uint32_t result = (bit7 << 31) | ((32 - bit6) << 25) | (bit5_to_0 << 19);

  return rawbits_to_float(result);
}


float Instruction::ImmFP32() const {
  return Imm8ToFP32(ImmFP());
}


double Instruction::Imm8ToFP64(uint32_t imm8) {
  //   Imm8: abcdefgh (8 bits)
  // Double: aBbb.bbbb.bbcd.efgh.0000.0000.0000.0000
  //         0000.0000.0000.0000.0000.0000.0000.0000 (64 bits)
  // where B is b ^ 1
  uint32_t bits = imm8;
  uint64_t bit7 = (bits >> 7) & 0x1;
  uint64_t bit6 = (bits >> 6) & 0x1;
  uint64_t bit5_to_0 = bits & 0x3f;
  uint64_t result = (bit7 << 63) | ((256 - bit6) << 54) | (bit5_to_0 << 48);

  return rawbits_to_double(result);
}


double Instruction::ImmFP64() const {
  return Imm8ToFP64(ImmFP());
}


float Instruction::ImmNEONFP32() const {
  return Imm8ToFP32(ImmNEONabcdefgh());
}


double Instruction::ImmNEONFP64() const {
  return Imm8ToFP64(ImmNEONabcdefgh());
}


unsigned CalcLSDataSize(LoadStoreOp op) {
  VIXL_ASSERT((LSSize_offset + LSSize_width) == (kInstructionSize * 8));
  unsigned size = static_cast<Instr>(op) >> LSSize_offset;
  if ((op & LSVector_mask) != 0) {
    // Vector register memory operations encode the access size in the "size"
    // and "opc" fields.
    if ((size == 0) && ((op & LSOpc_mask) >> LSOpc_offset) >= 2) {
      size = kQRegSizeInBytesLog2;
    }
  }
  return size;
}


unsigned CalcLSPairDataSize(LoadStorePairOp op) {
  VIXL_STATIC_ASSERT(kXRegSizeInBytes == kDRegSizeInBytes);
  VIXL_STATIC_ASSERT(kWRegSizeInBytes == kSRegSizeInBytes);
  switch (op) {
    case STP_q:
    case LDP_q: return kQRegSizeInBytesLog2;
    case STP_x:
    case LDP_x:
    case STP_d:
    case LDP_d: return kXRegSizeInBytesLog2;
    default: return kWRegSizeInBytesLog2;
  }
}


int Instruction::ImmBranchRangeBitwidth(ImmBranchType branch_type) {
  switch (branch_type) {
    case UncondBranchType:
      return ImmUncondBranch_width;
    case CondBranchType:
      return ImmCondBranch_width;
    case CompareBranchType:
      return ImmCmpBranch_width;
    case TestBranchType:
      return ImmTestBranch_width;
    default:
      VIXL_UNREACHABLE();
      return 0;
  }
}


int32_t Instruction::ImmBranchForwardRange(ImmBranchType branch_type) {
  int32_t encoded_max = 1 << (ImmBranchRangeBitwidth(branch_type) - 1);
  return encoded_max * kInstructionSize;
}


bool Instruction::IsValidImmPCOffset(ImmBranchType branch_type,
                                     int64_t offset) {
  return is_intn(ImmBranchRangeBitwidth(branch_type), offset);
}

ImmBranchRangeType Instruction::ImmBranchTypeToRange(ImmBranchType branch_type)
{
  switch (branch_type) {
    case UncondBranchType:
      return UncondBranchRangeType;
    case CondBranchType:
    case CompareBranchType:
      return CondBranchRangeType;
    case TestBranchType:
      return TestBranchRangeType;
    default:
      return UnknownBranchRangeType;
  }
}

int32_t Instruction::ImmBranchMaxForwardOffset(ImmBranchRangeType range_type)
{
  // Branches encode a pc-relative two's complement number of 32-bit
  // instructions. Compute the number of bytes corresponding to the largest
  // positive number of instructions that can be encoded.
  switch(range_type) {
    case TestBranchRangeType:
      return ((1 << ImmTestBranch_width) - 1) / 2 * kInstructionSize;
    case CondBranchRangeType:
      return ((1 << ImmCondBranch_width) - 1) / 2 * kInstructionSize;
    case UncondBranchRangeType:
      return ((1 << ImmUncondBranch_width) - 1) / 2 * kInstructionSize;
    default:
      VIXL_UNREACHABLE();
      return 0;
  }
}

int32_t Instruction::ImmBranchMinBackwardOffset(ImmBranchRangeType range_type)
{
  switch(range_type) {
    case TestBranchRangeType:
      return -int32_t(1 << ImmTestBranch_width) / 2 * kInstructionSize;
    case CondBranchRangeType:
      return -int32_t(1 << ImmCondBranch_width) / 2 * kInstructionSize;
    case UncondBranchRangeType:
      return -int32_t(1 << ImmUncondBranch_width) / 2 * kInstructionSize;
    default:
      VIXL_UNREACHABLE();
      return 0;
  }
}

const Instruction* Instruction::ImmPCOffsetTarget() const {
  const Instruction * base = this;
  ptrdiff_t offset;
  if (IsPCRelAddressing()) {
    // ADR and ADRP.
    offset = ImmPCRel();
    if (Mask(PCRelAddressingMask) == ADRP) {
      base = AlignDown(base, kPageSize);
      offset *= kPageSize;
    } else {
      VIXL_ASSERT(Mask(PCRelAddressingMask) == ADR);
    }
  } else {
    // All PC-relative branches.
    VIXL_ASSERT(BranchType() != UnknownBranchType);
    // Relative branch offsets are instruction-size-aligned.
    offset = ImmBranch() << kInstructionSizeLog2;
  }
  return base + offset;
}


int Instruction::ImmBranch() const {
  switch (BranchType()) {
    case CondBranchType: return ImmCondBranch();
    case UncondBranchType: return ImmUncondBranch();
    case CompareBranchType: return ImmCmpBranch();
    case TestBranchType: return ImmTestBranch();
    default: VIXL_UNREACHABLE();
  }
  return 0;
}


void Instruction::SetImmPCOffsetTarget(const Instruction* target) {
  if (IsPCRelAddressing()) {
    SetPCRelImmTarget(target);
  } else {
    SetBranchImmTarget(target);
  }
}


void Instruction::SetPCRelImmTarget(const Instruction* target) {
  ptrdiff_t imm21;
  if ((Mask(PCRelAddressingMask) == ADR)) {
    imm21 = target - this;
  } else {
    VIXL_ASSERT(Mask(PCRelAddressingMask) == ADRP);
    uintptr_t this_page = reinterpret_cast<uintptr_t>(this) / kPageSize;
    uintptr_t target_page = reinterpret_cast<uintptr_t>(target) / kPageSize;
    imm21 = target_page - this_page;
  }
  Instr imm = Assembler::ImmPCRelAddress(static_cast<int32_t>(imm21));

  SetInstructionBits(Mask(~ImmPCRel_mask) | imm);
}


void Instruction::SetBranchImmTarget(const Instruction* target) {
  VIXL_ASSERT(((target - this) & 3) == 0);
  Instr branch_imm = 0;
  uint32_t imm_mask = 0;
  int offset = static_cast<int>((target - this) >> kInstructionSizeLog2);
  switch (BranchType()) {
    case CondBranchType: {
      branch_imm = Assembler::ImmCondBranch(offset);
      imm_mask = ImmCondBranch_mask;
      break;
    }
    case UncondBranchType: {
      branch_imm = Assembler::ImmUncondBranch(offset);
      imm_mask = ImmUncondBranch_mask;
      break;
    }
    case CompareBranchType: {
      branch_imm = Assembler::ImmCmpBranch(offset);
      imm_mask = ImmCmpBranch_mask;
      break;
    }
    case TestBranchType: {
      branch_imm = Assembler::ImmTestBranch(offset);
      imm_mask = ImmTestBranch_mask;
      break;
    }
    default: VIXL_UNREACHABLE();
  }
  SetInstructionBits(Mask(~imm_mask) | branch_imm);
}


void Instruction::SetImmLLiteral(const Instruction* source) {
  VIXL_ASSERT(IsWordAligned(source));
  ptrdiff_t offset = (source - this) >> kLiteralEntrySizeLog2;
  Instr imm = Assembler::ImmLLiteral(static_cast<int>(offset));
  Instr mask = ImmLLiteral_mask;

  SetInstructionBits(Mask(~mask) | imm);
}


VectorFormat VectorFormatHalfWidth(const VectorFormat vform) {
  VIXL_ASSERT(vform == kFormat8H || vform == kFormat4S || vform == kFormat2D ||
              vform == kFormatH || vform == kFormatS || vform == kFormatD);
  switch (vform) {
    case kFormat8H: return kFormat8B;
    case kFormat4S: return kFormat4H;
    case kFormat2D: return kFormat2S;
    case kFormatH:  return kFormatB;
    case kFormatS:  return kFormatH;
    case kFormatD:  return kFormatS;
    default: VIXL_UNREACHABLE(); return kFormatUndefined;
  }
}


VectorFormat VectorFormatDoubleWidth(const VectorFormat vform) {
  VIXL_ASSERT(vform == kFormat8B || vform == kFormat4H || vform == kFormat2S ||
              vform == kFormatB || vform == kFormatH || vform == kFormatS);
  switch (vform) {
    case kFormat8B: return kFormat8H;
    case kFormat4H: return kFormat4S;
    case kFormat2S: return kFormat2D;
    case kFormatB:  return kFormatH;
    case kFormatH:  return kFormatS;
    case kFormatS:  return kFormatD;
    default: VIXL_UNREACHABLE(); return kFormatUndefined;
  }
}


VectorFormat VectorFormatFillQ(const VectorFormat vform) {
  switch (vform) {
    case kFormatB:
    case kFormat8B:
    case kFormat16B: return kFormat16B;
    case kFormatH:
    case kFormat4H:
    case kFormat8H:  return kFormat8H;
    case kFormatS:
    case kFormat2S:
    case kFormat4S:  return kFormat4S;
    case kFormatD:
    case kFormat1D:
    case kFormat2D:  return kFormat2D;
    default: VIXL_UNREACHABLE(); return kFormatUndefined;
  }
}

VectorFormat VectorFormatHalfWidthDoubleLanes(const VectorFormat vform) {
  switch (vform) {
    case kFormat4H: return kFormat8B;
    case kFormat8H: return kFormat16B;
    case kFormat2S: return kFormat4H;
    case kFormat4S: return kFormat8H;
    case kFormat1D: return kFormat2S;
    case kFormat2D: return kFormat4S;
    default: VIXL_UNREACHABLE(); return kFormatUndefined;
  }
}

VectorFormat VectorFormatDoubleLanes(const VectorFormat vform) {
  VIXL_ASSERT(vform == kFormat8B || vform == kFormat4H || vform == kFormat2S);
  switch (vform) {
    case kFormat8B: return kFormat16B;
    case kFormat4H: return kFormat8H;
    case kFormat2S: return kFormat4S;
    default: VIXL_UNREACHABLE(); return kFormatUndefined;
  }
}


VectorFormat VectorFormatHalfLanes(const VectorFormat vform) {
  VIXL_ASSERT(vform == kFormat16B || vform == kFormat8H || vform == kFormat4S);
  switch (vform) {
    case kFormat16B: return kFormat8B;
    case kFormat8H: return kFormat4H;
    case kFormat4S: return kFormat2S;
    default: VIXL_UNREACHABLE(); return kFormatUndefined;
  }
}


VectorFormat ScalarFormatFromLaneSize(int laneSize) {
  switch (laneSize) {
    case 8:  return kFormatB;
    case 16: return kFormatH;
    case 32: return kFormatS;
    case 64: return kFormatD;
    default: VIXL_UNREACHABLE(); return kFormatUndefined;
  }
}


unsigned RegisterSizeInBitsFromFormat(VectorFormat vform) {
  VIXL_ASSERT(vform != kFormatUndefined);
  switch (vform) {
    case kFormatB: return kBRegSize;
    case kFormatH: return kHRegSize;
    case kFormatS: return kSRegSize;
    case kFormatD: return kDRegSize;
    case kFormat8B:
    case kFormat4H:
    case kFormat2S:
    case kFormat1D: return kDRegSize;
    default: return kQRegSize;
  }
}


unsigned RegisterSizeInBytesFromFormat(VectorFormat vform) {
  return RegisterSizeInBitsFromFormat(vform) / 8;
}


unsigned LaneSizeInBitsFromFormat(VectorFormat vform) {
  VIXL_ASSERT(vform != kFormatUndefined);
  switch (vform) {
    case kFormatB:
    case kFormat8B:
    case kFormat16B: return 8;
    case kFormatH:
    case kFormat4H:
    case kFormat8H: return 16;
    case kFormatS:
    case kFormat2S:
    case kFormat4S: return 32;
    case kFormatD:
    case kFormat1D:
    case kFormat2D: return 64;
    default: VIXL_UNREACHABLE(); return 0;
  }
}


int LaneSizeInBytesFromFormat(VectorFormat vform) {
  return LaneSizeInBitsFromFormat(vform) / 8;
}


int LaneSizeInBytesLog2FromFormat(VectorFormat vform) {
  VIXL_ASSERT(vform != kFormatUndefined);
  switch (vform) {
    case kFormatB:
    case kFormat8B:
    case kFormat16B: return 0;
    case kFormatH:
    case kFormat4H:
    case kFormat8H: return 1;
    case kFormatS:
    case kFormat2S:
    case kFormat4S: return 2;
    case kFormatD:
    case kFormat1D:
    case kFormat2D: return 3;
    default: VIXL_UNREACHABLE(); return 0;
  }
}


int LaneCountFromFormat(VectorFormat vform) {
  VIXL_ASSERT(vform != kFormatUndefined);
  switch (vform) {
    case kFormat16B: return 16;
    case kFormat8B:
    case kFormat8H: return 8;
    case kFormat4H:
    case kFormat4S: return 4;
    case kFormat2S:
    case kFormat2D: return 2;
    case kFormat1D:
    case kFormatB:
    case kFormatH:
    case kFormatS:
    case kFormatD: return 1;
    default: VIXL_UNREACHABLE(); return 0;
  }
}


int MaxLaneCountFromFormat(VectorFormat vform) {
  VIXL_ASSERT(vform != kFormatUndefined);
  switch (vform) {
    case kFormatB:
    case kFormat8B:
    case kFormat16B: return 16;
    case kFormatH:
    case kFormat4H:
    case kFormat8H: return 8;
    case kFormatS:
    case kFormat2S:
    case kFormat4S: return 4;
    case kFormatD:
    case kFormat1D:
    case kFormat2D: return 2;
    default: VIXL_UNREACHABLE(); return 0;
  }
}


// Does 'vform' indicate a vector format or a scalar format?
bool IsVectorFormat(VectorFormat vform) {
  VIXL_ASSERT(vform != kFormatUndefined);
  switch (vform) {
    case kFormatB:
    case kFormatH:
    case kFormatS:
    case kFormatD: return false;
    default: return true;
  }
}


int64_t MaxIntFromFormat(VectorFormat vform) {
  return INT64_MAX >> (64 - LaneSizeInBitsFromFormat(vform));
}


int64_t MinIntFromFormat(VectorFormat vform) {
  return INT64_MIN >> (64 - LaneSizeInBitsFromFormat(vform));
}


uint64_t MaxUintFromFormat(VectorFormat vform) {
  return UINT64_MAX >> (64 - LaneSizeInBitsFromFormat(vform));
}
}  // namespace vixl

