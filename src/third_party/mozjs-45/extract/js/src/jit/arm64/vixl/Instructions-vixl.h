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

#ifndef VIXL_A64_INSTRUCTIONS_A64_H_
#define VIXL_A64_INSTRUCTIONS_A64_H_

#include "jit/arm64/vixl/Constants-vixl.h"
#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Utils-vixl.h"

namespace vixl {
// ISA constants. --------------------------------------------------------------

typedef uint32_t Instr;
const unsigned kInstructionSize = 4;
const unsigned kInstructionSizeLog2 = 2;
const unsigned kLiteralEntrySize = 4;
const unsigned kLiteralEntrySizeLog2 = 2;
const unsigned kMaxLoadLiteralRange = 1 * MBytes;

// This is the nominal page size (as used by the adrp instruction); the actual
// size of the memory pages allocated by the kernel is likely to differ.
const unsigned kPageSize = 4 * KBytes;
const unsigned kPageSizeLog2 = 12;

const unsigned kBRegSize = 8;
const unsigned kBRegSizeLog2 = 3;
const unsigned kBRegSizeInBytes = kBRegSize / 8;
const unsigned kBRegSizeInBytesLog2 = kBRegSizeLog2 - 3;
const unsigned kHRegSize = 16;
const unsigned kHRegSizeLog2 = 4;
const unsigned kHRegSizeInBytes = kHRegSize / 8;
const unsigned kHRegSizeInBytesLog2 = kHRegSizeLog2 - 3;
const unsigned kWRegSize = 32;
const unsigned kWRegSizeLog2 = 5;
const unsigned kWRegSizeInBytes = kWRegSize / 8;
const unsigned kWRegSizeInBytesLog2 = kWRegSizeLog2 - 3;
const unsigned kXRegSize = 64;
const unsigned kXRegSizeLog2 = 6;
const unsigned kXRegSizeInBytes = kXRegSize / 8;
const unsigned kXRegSizeInBytesLog2 = kXRegSizeLog2 - 3;
const unsigned kSRegSize = 32;
const unsigned kSRegSizeLog2 = 5;
const unsigned kSRegSizeInBytes = kSRegSize / 8;
const unsigned kSRegSizeInBytesLog2 = kSRegSizeLog2 - 3;
const unsigned kDRegSize = 64;
const unsigned kDRegSizeLog2 = 6;
const unsigned kDRegSizeInBytes = kDRegSize / 8;
const unsigned kDRegSizeInBytesLog2 = kDRegSizeLog2 - 3;
const unsigned kQRegSize = 128;
const unsigned kQRegSizeLog2 = 7;
const unsigned kQRegSizeInBytes = kQRegSize / 8;
const unsigned kQRegSizeInBytesLog2 = kQRegSizeLog2 - 3;
const uint64_t kWRegMask = UINT64_C(0xffffffff);
const uint64_t kXRegMask = UINT64_C(0xffffffffffffffff);
const uint64_t kSRegMask = UINT64_C(0xffffffff);
const uint64_t kDRegMask = UINT64_C(0xffffffffffffffff);
const uint64_t kSSignMask = UINT64_C(0x80000000);
const uint64_t kDSignMask = UINT64_C(0x8000000000000000);
const uint64_t kWSignMask = UINT64_C(0x80000000);
const uint64_t kXSignMask = UINT64_C(0x8000000000000000);
const uint64_t kByteMask = UINT64_C(0xff);
const uint64_t kHalfWordMask = UINT64_C(0xffff);
const uint64_t kWordMask = UINT64_C(0xffffffff);
const uint64_t kXMaxUInt = UINT64_C(0xffffffffffffffff);
const uint64_t kWMaxUInt = UINT64_C(0xffffffff);
const int64_t kXMaxInt = INT64_C(0x7fffffffffffffff);
const int64_t kXMinInt = INT64_C(0x8000000000000000);
const int32_t kWMaxInt = INT32_C(0x7fffffff);
const int32_t kWMinInt = INT32_C(0x80000000);
const unsigned kLinkRegCode = 30;
const unsigned kZeroRegCode = 31;
const unsigned kSPRegInternalCode = 63;
const unsigned kRegCodeMask = 0x1f;

const unsigned kAddressTagOffset = 56;
const unsigned kAddressTagWidth = 8;
const uint64_t kAddressTagMask =
    ((UINT64_C(1) << kAddressTagWidth) - 1) << kAddressTagOffset;
VIXL_STATIC_ASSERT(kAddressTagMask == UINT64_C(0xff00000000000000));

// AArch64 floating-point specifics. These match IEEE-754.
const unsigned kDoubleMantissaBits = 52;
const unsigned kDoubleExponentBits = 11;
const unsigned kFloatMantissaBits = 23;
const unsigned kFloatExponentBits = 8;
const unsigned kFloat16MantissaBits = 10;
const unsigned kFloat16ExponentBits = 5;

// Floating-point infinity values.
extern const float16 kFP16PositiveInfinity;
extern const float16 kFP16NegativeInfinity;
extern const float kFP32PositiveInfinity;
extern const float kFP32NegativeInfinity;
extern const double kFP64PositiveInfinity;
extern const double kFP64NegativeInfinity;

// The default NaN values (for FPCR.DN=1).
extern const float16 kFP16DefaultNaN;
extern const float kFP32DefaultNaN;
extern const double kFP64DefaultNaN;

unsigned CalcLSDataSize(LoadStoreOp op);
unsigned CalcLSPairDataSize(LoadStorePairOp op);

enum ImmBranchType {
  UnknownBranchType = 0,
  CondBranchType    = 1,
  UncondBranchType  = 2,
  CompareBranchType = 3,
  TestBranchType    = 4
};

// The classes of immediate branch ranges, in order of increasing range.
// Note that CondBranchType and CompareBranchType have the same range.
enum ImmBranchRangeType {
  TestBranchRangeType,   // tbz/tbnz: imm14 = +/- 32KB.
  CondBranchRangeType,   // b.cond/cbz/cbnz: imm19 = +/- 1MB.
  UncondBranchRangeType, // b/bl: imm26 = +/- 128MB.
  UnknownBranchRangeType,

  // Number of 'short-range' branch range types.
  // We don't consider unconditional branches 'short-range'.
  NumShortBranchRangeTypes = UncondBranchRangeType
};

enum AddrMode {
  Offset,
  PreIndex,
  PostIndex
};

enum FPRounding {
  // The first four values are encodable directly by FPCR<RMode>.
  FPTieEven = 0x0,
  FPPositiveInfinity = 0x1,
  FPNegativeInfinity = 0x2,
  FPZero = 0x3,

  // The final rounding modes are only available when explicitly specified by
  // the instruction (such as with fcvta). It cannot be set in FPCR.
  FPTieAway,
  FPRoundOdd
};

enum Reg31Mode {
  Reg31IsStackPointer,
  Reg31IsZeroRegister
};

// Instructions. ---------------------------------------------------------------

class Instruction {
 public:
  Instr InstructionBits() const {
    return *(reinterpret_cast<const Instr*>(this));
  }

  void SetInstructionBits(Instr new_instr) {
    *(reinterpret_cast<Instr*>(this)) = new_instr;
  }

  int Bit(int pos) const {
    return (InstructionBits() >> pos) & 1;
  }

  uint32_t Bits(int msb, int lsb) const {
    return unsigned_bitextract_32(msb, lsb, InstructionBits());
  }

  int32_t SignedBits(int msb, int lsb) const {
    int32_t bits = *(reinterpret_cast<const int32_t*>(this));
    return signed_bitextract_32(msb, lsb, bits);
  }

  Instr Mask(uint32_t mask) const {
    return InstructionBits() & mask;
  }

  #define DEFINE_GETTER(Name, HighBit, LowBit, Func)             \
  int32_t Name() const { return Func(HighBit, LowBit); }
  INSTRUCTION_FIELDS_LIST(DEFINE_GETTER)
  #undef DEFINE_GETTER

  #define DEFINE_SETTER(Name, HighBit, LowBit, Func)             \
  inline void Set##Name(unsigned n) { SetBits32(HighBit, LowBit, n); }
  INSTRUCTION_FIELDS_LIST(DEFINE_SETTER)
  #undef DEFINE_SETTER

  // ImmPCRel is a compound field (not present in INSTRUCTION_FIELDS_LIST),
  // formed from ImmPCRelLo and ImmPCRelHi.
  int ImmPCRel() const {
    int offset =
        static_cast<int>((ImmPCRelHi() << ImmPCRelLo_width) | ImmPCRelLo());
    int width = ImmPCRelLo_width + ImmPCRelHi_width;
    return signed_bitextract_32(width - 1, 0, offset);
  }

  uint64_t ImmLogical() const;
  unsigned ImmNEONabcdefgh() const;
  float ImmFP32() const;
  double ImmFP64() const;
  float ImmNEONFP32() const;
  double ImmNEONFP64() const;

  unsigned SizeLS() const {
    return CalcLSDataSize(static_cast<LoadStoreOp>(Mask(LoadStoreMask)));
  }

  unsigned SizeLSPair() const {
    return CalcLSPairDataSize(
        static_cast<LoadStorePairOp>(Mask(LoadStorePairMask)));
  }

  int NEONLSIndex(int access_size_shift) const {
    int64_t q = NEONQ();
    int64_t s = NEONS();
    int64_t size = NEONLSSize();
    int64_t index = (q << 3) | (s << 2) | size;
    return static_cast<int>(index >> access_size_shift);
  }

  // Helpers.
  bool IsCondBranchImm() const {
    return Mask(ConditionalBranchFMask) == ConditionalBranchFixed;
  }

  bool IsUncondBranchImm() const {
    return Mask(UnconditionalBranchFMask) == UnconditionalBranchFixed;
  }

  bool IsCompareBranch() const {
    return Mask(CompareBranchFMask) == CompareBranchFixed;
  }

  bool IsTestBranch() const {
    return Mask(TestBranchFMask) == TestBranchFixed;
  }

  bool IsImmBranch() const {
    return BranchType() != UnknownBranchType;
  }

  bool IsPCRelAddressing() const {
    return Mask(PCRelAddressingFMask) == PCRelAddressingFixed;
  }

  bool IsLogicalImmediate() const {
    return Mask(LogicalImmediateFMask) == LogicalImmediateFixed;
  }

  bool IsAddSubImmediate() const {
    return Mask(AddSubImmediateFMask) == AddSubImmediateFixed;
  }

  bool IsAddSubExtended() const {
    return Mask(AddSubExtendedFMask) == AddSubExtendedFixed;
  }

  bool IsLoadOrStore() const {
    return Mask(LoadStoreAnyFMask) == LoadStoreAnyFixed;
  }

  bool IsLoad() const;
  bool IsStore() const;

  bool IsLoadLiteral() const {
    // This includes PRFM_lit.
    return Mask(LoadLiteralFMask) == LoadLiteralFixed;
  }

  bool IsMovn() const {
    return (Mask(MoveWideImmediateMask) == MOVN_x) ||
           (Mask(MoveWideImmediateMask) == MOVN_w);
  }

  // Mozilla modifications.
  bool IsUncondB() const;
  bool IsCondB() const;
  bool IsBL() const;
  bool IsBR() const;
  bool IsBLR() const;
  bool IsTBZ() const;
  bool IsTBNZ() const;
  bool IsCBZ() const;
  bool IsCBNZ() const;
  bool IsLDR() const;
  bool IsNOP() const;
  bool IsADR() const;
  bool IsADRP() const;
  bool IsBranchLinkImm() const;
  bool IsTargetReachable(Instruction* target) const;
  ptrdiff_t ImmPCRawOffset() const;
  void SetImmPCRawOffset(ptrdiff_t offset);
  void SetBits32(int msb, int lsb, unsigned value);

  // Is this a stack pointer synchronization instruction as inserted by
  // MacroAssembler::syncStackPtr()?
  bool IsStackPtrSync() const;

  static int ImmBranchRangeBitwidth(ImmBranchType branch_type);
  static int32_t ImmBranchForwardRange(ImmBranchType branch_type);

  // Check if offset can be encoded as a RAW offset in a branch_type
  // instruction. The offset must be encodeable directly as the immediate field
  // in the instruction, it is not scaled by kInstructionSize first.
  static bool IsValidImmPCOffset(ImmBranchType branch_type, int64_t offset);

  // Get the range type corresponding to a branch type.
  static ImmBranchRangeType ImmBranchTypeToRange(ImmBranchType);

  // Get the maximum realizable forward PC offset (in bytes) for an immediate
  // branch of the given range type.
  // This is the largest positive multiple of kInstructionSize, offset, such
  // that:
  //
  //    IsValidImmPCOffset(xxx, offset / kInstructionSize)
  //
  // returns true for the same branch type.
  static int32_t ImmBranchMaxForwardOffset(ImmBranchRangeType range_type);

  // Get the minimuum realizable backward PC offset (in bytes) for an immediate
  // branch of the given range type.
  // This is the smallest (i.e., largest in magnitude) negative multiple of
  // kInstructionSize, offset, such that:
  //
  //    IsValidImmPCOffset(xxx, offset / kInstructionSize)
  //
  // returns true for the same branch type.
  static int32_t ImmBranchMinBackwardOffset(ImmBranchRangeType range_type);

  // Indicate whether Rd can be the stack pointer or the zero register. This
  // does not check that the instruction actually has an Rd field.
  Reg31Mode RdMode() const {
    // The following instructions use sp or wsp as Rd:
    //  Add/sub (immediate) when not setting the flags.
    //  Add/sub (extended) when not setting the flags.
    //  Logical (immediate) when not setting the flags.
    // Otherwise, r31 is the zero register.
    if (IsAddSubImmediate() || IsAddSubExtended()) {
      if (Mask(AddSubSetFlagsBit)) {
        return Reg31IsZeroRegister;
      } else {
        return Reg31IsStackPointer;
      }
    }
    if (IsLogicalImmediate()) {
      // Of the logical (immediate) instructions, only ANDS (and its aliases)
      // can set the flags. The others can all write into sp.
      // Note that some logical operations are not available to
      // immediate-operand instructions, so we have to combine two masks here.
      if (Mask(LogicalImmediateMask & LogicalOpMask) == ANDS) {
        return Reg31IsZeroRegister;
      } else {
        return Reg31IsStackPointer;
      }
    }
    return Reg31IsZeroRegister;
  }

  // Indicate whether Rn can be the stack pointer or the zero register. This
  // does not check that the instruction actually has an Rn field.
  Reg31Mode RnMode() const {
    // The following instructions use sp or wsp as Rn:
    //  All loads and stores.
    //  Add/sub (immediate).
    //  Add/sub (extended).
    // Otherwise, r31 is the zero register.
    if (IsLoadOrStore() || IsAddSubImmediate() || IsAddSubExtended()) {
      return Reg31IsStackPointer;
    }
    return Reg31IsZeroRegister;
  }

  ImmBranchType BranchType() const {
    if (IsCondBranchImm()) {
      return CondBranchType;
    } else if (IsUncondBranchImm()) {
      return UncondBranchType;
    } else if (IsCompareBranch()) {
      return CompareBranchType;
    } else if (IsTestBranch()) {
      return TestBranchType;
    } else {
      return UnknownBranchType;
    }
  }

  // Find the target of this instruction. 'this' may be a branch or a
  // PC-relative addressing instruction.
  const Instruction* ImmPCOffsetTarget() const;

  // Patch a PC-relative offset to refer to 'target'. 'this' may be a branch or
  // a PC-relative addressing instruction.
  void SetImmPCOffsetTarget(const Instruction* target);
  // Patch a literal load instruction to load from 'source'.
  void SetImmLLiteral(const Instruction* source);

  // The range of a load literal instruction, expressed as 'instr +- range'.
  // The range is actually the 'positive' range; the branch instruction can
  // target [instr - range - kInstructionSize, instr + range].
  static const int kLoadLiteralImmBitwidth = 19;
  static const int kLoadLiteralRange =
      (1 << kLoadLiteralImmBitwidth) / 2 - kInstructionSize;

  // Calculate the address of a literal referred to by a load-literal
  // instruction, and return it as the specified type.
  //
  // The literal itself is safely mutable only if the backing buffer is safely
  // mutable.
  template <typename T>
  T LiteralAddress() const {
    uint64_t base_raw = reinterpret_cast<uint64_t>(this);
    int64_t offset = ImmLLiteral() << kLiteralEntrySizeLog2;
    uint64_t address_raw = base_raw + offset;

    // Cast the address using a C-style cast. A reinterpret_cast would be
    // appropriate, but it can't cast one integral type to another.
    T address = (T)(address_raw);

    // Assert that the address can be represented by the specified type.
    VIXL_ASSERT((uint64_t)(address) == address_raw);

    return address;
  }

  uint32_t Literal32() const {
    uint32_t literal;
    memcpy(&literal, LiteralAddress<const void*>(), sizeof(literal));
    return literal;
  }

  uint64_t Literal64() const {
    uint64_t literal;
    memcpy(&literal, LiteralAddress<const void*>(), sizeof(literal));
    return literal;
  }

  float LiteralFP32() const {
    return rawbits_to_float(Literal32());
  }

  double LiteralFP64() const {
    return rawbits_to_double(Literal64());
  }

  const Instruction* NextInstruction() const {
    return this + kInstructionSize;
  }

  // Skip any constant pools with artificial guards at this point.
  // Return either |this| or the first instruction after the pool.
  const Instruction* skipPool() const;

  const Instruction* InstructionAtOffset(int64_t offset) const {
    VIXL_ASSERT(IsWordAligned(this + offset));
    return this + offset;
  }

  template<typename T> static Instruction* Cast(T src) {
    return reinterpret_cast<Instruction*>(src);
  }

  template<typename T> static const Instruction* CastConst(T src) {
    return reinterpret_cast<const Instruction*>(src);
  }

 private:
  int ImmBranch() const;

  static float Imm8ToFP32(uint32_t imm8);
  static double Imm8ToFP64(uint32_t imm8);

  void SetPCRelImmTarget(const Instruction* target);
  void SetBranchImmTarget(const Instruction* target);
};


// Functions for handling NEON vector format information.
enum VectorFormat {
  kFormatUndefined = 0xffffffff,
  kFormat8B  = NEON_8B,
  kFormat16B = NEON_16B,
  kFormat4H  = NEON_4H,
  kFormat8H  = NEON_8H,
  kFormat2S  = NEON_2S,
  kFormat4S  = NEON_4S,
  kFormat1D  = NEON_1D,
  kFormat2D  = NEON_2D,

  // Scalar formats. We add the scalar bit to distinguish between scalar and
  // vector enumerations; the bit is always set in the encoding of scalar ops
  // and always clear for vector ops. Although kFormatD and kFormat1D appear
  // to be the same, their meaning is subtly different. The first is a scalar
  // operation, the second a vector operation that only affects one lane.
  kFormatB = NEON_B | NEONScalar,
  kFormatH = NEON_H | NEONScalar,
  kFormatS = NEON_S | NEONScalar,
  kFormatD = NEON_D | NEONScalar
};

VectorFormat VectorFormatHalfWidth(const VectorFormat vform);
VectorFormat VectorFormatDoubleWidth(const VectorFormat vform);
VectorFormat VectorFormatDoubleLanes(const VectorFormat vform);
VectorFormat VectorFormatHalfLanes(const VectorFormat vform);
VectorFormat ScalarFormatFromLaneSize(int lanesize);
VectorFormat VectorFormatHalfWidthDoubleLanes(const VectorFormat vform);
VectorFormat VectorFormatFillQ(const VectorFormat vform);
unsigned RegisterSizeInBitsFromFormat(VectorFormat vform);
unsigned RegisterSizeInBytesFromFormat(VectorFormat vform);
// TODO: Make the return types of these functions consistent.
unsigned LaneSizeInBitsFromFormat(VectorFormat vform);
int LaneSizeInBytesFromFormat(VectorFormat vform);
int LaneSizeInBytesLog2FromFormat(VectorFormat vform);
int LaneCountFromFormat(VectorFormat vform);
int MaxLaneCountFromFormat(VectorFormat vform);
bool IsVectorFormat(VectorFormat vform);
int64_t MaxIntFromFormat(VectorFormat vform);
int64_t MinIntFromFormat(VectorFormat vform);
uint64_t MaxUintFromFormat(VectorFormat vform);


enum NEONFormat {
  NF_UNDEF = 0,
  NF_8B    = 1,
  NF_16B   = 2,
  NF_4H    = 3,
  NF_8H    = 4,
  NF_2S    = 5,
  NF_4S    = 6,
  NF_1D    = 7,
  NF_2D    = 8,
  NF_B     = 9,
  NF_H     = 10,
  NF_S     = 11,
  NF_D     = 12
};

static const unsigned kNEONFormatMaxBits = 6;

struct NEONFormatMap {
  // The bit positions in the instruction to consider.
  uint8_t bits[kNEONFormatMaxBits];

  // Mapping from concatenated bits to format.
  NEONFormat map[1 << kNEONFormatMaxBits];
};

class NEONFormatDecoder {
 public:
  enum SubstitutionMode {
    kPlaceholder,
    kFormat
  };

  // Construct a format decoder with increasingly specific format maps for each
  // subsitution. If no format map is specified, the default is the integer
  // format map.
  explicit NEONFormatDecoder(const Instruction* instr) {
    instrbits_ = instr->InstructionBits();
    SetFormatMaps(IntegerFormatMap());
  }
  NEONFormatDecoder(const Instruction* instr,
                    const NEONFormatMap* format) {
    instrbits_ = instr->InstructionBits();
    SetFormatMaps(format);
  }
  NEONFormatDecoder(const Instruction* instr,
                    const NEONFormatMap* format0,
                    const NEONFormatMap* format1) {
    instrbits_ = instr->InstructionBits();
    SetFormatMaps(format0, format1);
  }
  NEONFormatDecoder(const Instruction* instr,
                    const NEONFormatMap* format0,
                    const NEONFormatMap* format1,
                    const NEONFormatMap* format2) {
    instrbits_ = instr->InstructionBits();
    SetFormatMaps(format0, format1, format2);
  }

  // Set the format mapping for all or individual substitutions.
  void SetFormatMaps(const NEONFormatMap* format0,
                     const NEONFormatMap* format1 = NULL,
                     const NEONFormatMap* format2 = NULL) {
    VIXL_ASSERT(format0 != NULL);
    formats_[0] = format0;
    formats_[1] = (format1 == NULL) ? formats_[0] : format1;
    formats_[2] = (format2 == NULL) ? formats_[1] : format2;
  }
  void SetFormatMap(unsigned index, const NEONFormatMap* format) {
    VIXL_ASSERT(index <= (sizeof(formats_) / sizeof(formats_[0])));
    VIXL_ASSERT(format != NULL);
    formats_[index] = format;
  }

  // Substitute %s in the input string with the placeholder string for each
  // register, ie. "'B", "'H", etc.
  const char* SubstitutePlaceholders(const char* string) {
    return Substitute(string, kPlaceholder, kPlaceholder, kPlaceholder);
  }

  // Substitute %s in the input string with a new string based on the
  // substitution mode.
  const char* Substitute(const char* string,
                         SubstitutionMode mode0 = kFormat,
                         SubstitutionMode mode1 = kFormat,
                         SubstitutionMode mode2 = kFormat) {
    snprintf(form_buffer_, sizeof(form_buffer_), string,
             GetSubstitute(0, mode0),
             GetSubstitute(1, mode1),
             GetSubstitute(2, mode2));
    return form_buffer_;
  }

  // Append a "2" to a mnemonic string based of the state of the Q bit.
  const char* Mnemonic(const char* mnemonic) {
    if ((instrbits_ & NEON_Q) != 0) {
      snprintf(mne_buffer_, sizeof(mne_buffer_), "%s2", mnemonic);
      return mne_buffer_;
    }
    return mnemonic;
  }

  VectorFormat GetVectorFormat(int format_index = 0) {
    return GetVectorFormat(formats_[format_index]);
  }

  VectorFormat GetVectorFormat(const NEONFormatMap* format_map) {
    static const VectorFormat vform[] = {
      kFormatUndefined,
      kFormat8B, kFormat16B, kFormat4H, kFormat8H,
      kFormat2S, kFormat4S, kFormat1D, kFormat2D,
      kFormatB, kFormatH, kFormatS, kFormatD
    };
    VIXL_ASSERT(GetNEONFormat(format_map) < (sizeof(vform) / sizeof(vform[0])));
    return vform[GetNEONFormat(format_map)];
  }

  // Built in mappings for common cases.

  // The integer format map uses three bits (Q, size<1:0>) to encode the
  // "standard" set of NEON integer vector formats.
  static const NEONFormatMap* IntegerFormatMap() {
    static const NEONFormatMap map = {
      {23, 22, 30},
      {NF_8B, NF_16B, NF_4H, NF_8H, NF_2S, NF_4S, NF_UNDEF, NF_2D}
    };
    return &map;
  }

  // The long integer format map uses two bits (size<1:0>) to encode the
  // long set of NEON integer vector formats. These are used in narrow, wide
  // and long operations.
  static const NEONFormatMap* LongIntegerFormatMap() {
    static const NEONFormatMap map = {
      {23, 22}, {NF_8H, NF_4S, NF_2D}
    };
    return &map;
  }

  // The FP format map uses two bits (Q, size<0>) to encode the NEON FP vector
  // formats: NF_2S, NF_4S, NF_2D.
  static const NEONFormatMap* FPFormatMap() {
    // The FP format map assumes two bits (Q, size<0>) are used to encode the
    // NEON FP vector formats: NF_2S, NF_4S, NF_2D.
    static const NEONFormatMap map = {
      {22, 30}, {NF_2S, NF_4S, NF_UNDEF, NF_2D}
    };
    return &map;
  }

  // The load/store format map uses three bits (Q, 11, 10) to encode the
  // set of NEON vector formats.
  static const NEONFormatMap* LoadStoreFormatMap() {
    static const NEONFormatMap map = {
      {11, 10, 30},
      {NF_8B, NF_16B, NF_4H, NF_8H, NF_2S, NF_4S, NF_1D, NF_2D}
    };
    return &map;
  }

  // The logical format map uses one bit (Q) to encode the NEON vector format:
  // NF_8B, NF_16B.
  static const NEONFormatMap* LogicalFormatMap() {
    static const NEONFormatMap map = {
      {30}, {NF_8B, NF_16B}
    };
    return &map;
  }

  // The triangular format map uses between two and five bits to encode the NEON
  // vector format:
  // xxx10->8B, xxx11->16B, xx100->4H, xx101->8H
  // x1000->2S, x1001->4S,  10001->2D, all others undefined.
  static const NEONFormatMap* TriangularFormatMap() {
    static const NEONFormatMap map = {
      {19, 18, 17, 16, 30},
      {NF_UNDEF, NF_UNDEF, NF_8B, NF_16B, NF_4H, NF_8H, NF_8B, NF_16B, NF_2S,
       NF_4S, NF_8B, NF_16B, NF_4H, NF_8H, NF_8B, NF_16B, NF_UNDEF, NF_2D,
       NF_8B, NF_16B, NF_4H, NF_8H, NF_8B, NF_16B, NF_2S, NF_4S, NF_8B, NF_16B,
       NF_4H, NF_8H, NF_8B, NF_16B}
    };
    return &map;
  }

  // The scalar format map uses two bits (size<1:0>) to encode the NEON scalar
  // formats: NF_B, NF_H, NF_S, NF_D.
  static const NEONFormatMap* ScalarFormatMap() {
    static const NEONFormatMap map = {
      {23, 22}, {NF_B, NF_H, NF_S, NF_D}
    };
    return &map;
  }

  // The long scalar format map uses two bits (size<1:0>) to encode the longer
  // NEON scalar formats: NF_H, NF_S, NF_D.
  static const NEONFormatMap* LongScalarFormatMap() {
    static const NEONFormatMap map = {
      {23, 22}, {NF_H, NF_S, NF_D}
    };
    return &map;
  }

  // The FP scalar format map assumes one bit (size<0>) is used to encode the
  // NEON FP scalar formats: NF_S, NF_D.
  static const NEONFormatMap* FPScalarFormatMap() {
    static const NEONFormatMap map = {
      {22}, {NF_S, NF_D}
    };
    return &map;
  }

  // The triangular scalar format map uses between one and four bits to encode
  // the NEON FP scalar formats:
  // xxx1->B, xx10->H, x100->S, 1000->D, all others undefined.
  static const NEONFormatMap* TriangularScalarFormatMap() {
    static const NEONFormatMap map = {
      {19, 18, 17, 16},
      {NF_UNDEF, NF_B, NF_H, NF_B, NF_S, NF_B, NF_H, NF_B,
       NF_D,     NF_B, NF_H, NF_B, NF_S, NF_B, NF_H, NF_B}
    };
    return &map;
  }

 private:
  // Get a pointer to a string that represents the format or placeholder for
  // the specified substitution index, based on the format map and instruction.
  const char* GetSubstitute(int index, SubstitutionMode mode) {
    if (mode == kFormat) {
      return NEONFormatAsString(GetNEONFormat(formats_[index]));
    }
    VIXL_ASSERT(mode == kPlaceholder);
    return NEONFormatAsPlaceholder(GetNEONFormat(formats_[index]));
  }

  // Get the NEONFormat enumerated value for bits obtained from the
  // instruction based on the specified format mapping.
  NEONFormat GetNEONFormat(const NEONFormatMap* format_map) {
    return format_map->map[PickBits(format_map->bits)];
  }

  // Convert a NEONFormat into a string.
  static const char* NEONFormatAsString(NEONFormat format) {
    static const char* formats[] = {
      "undefined",
      "8b", "16b", "4h", "8h", "2s", "4s", "1d", "2d",
      "b", "h", "s", "d"
    };
    VIXL_ASSERT(format < (sizeof(formats) / sizeof(formats[0])));
    return formats[format];
  }

  // Convert a NEONFormat into a register placeholder string.
  static const char* NEONFormatAsPlaceholder(NEONFormat format) {
    VIXL_ASSERT((format == NF_B) || (format == NF_H) ||
                (format == NF_S) || (format == NF_D) ||
                (format == NF_UNDEF));
    static const char* formats[] = {
      "undefined",
      "undefined", "undefined", "undefined", "undefined",
      "undefined", "undefined", "undefined", "undefined",
      "'B", "'H", "'S", "'D"
    };
    return formats[format];
  }

  // Select bits from instrbits_ defined by the bits array, concatenate them,
  // and return the value.
  uint8_t PickBits(const uint8_t bits[]) {
    uint8_t result = 0;
    for (unsigned b = 0; b < kNEONFormatMaxBits; b++) {
      if (bits[b] == 0) break;
      result <<= 1;
      result |= ((instrbits_ & (1 << bits[b])) == 0) ? 0 : 1;
    }
    return result;
  }

  Instr instrbits_;
  const NEONFormatMap* formats_[3];
  char form_buffer_[64];
  char mne_buffer_[16];
};
}  // namespace vixl

#endif  // VIXL_A64_INSTRUCTIONS_A64_H_
