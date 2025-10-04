// Copyright 2015, VIXL authors
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

#ifndef VIXL_A64_SIMULATOR_A64_H_
#define VIXL_A64_SIMULATOR_A64_H_

#include "jstypes.h"

#ifdef JS_SIMULATOR_ARM64

#include "mozilla/Vector.h"

#include "jit/arm64/vixl/Assembler-vixl.h"
#include "jit/arm64/vixl/Disasm-vixl.h"
#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Instructions-vixl.h"
#include "jit/arm64/vixl/Instrument-vixl.h"
#include "jit/arm64/vixl/MozCachingDecoder.h"
#include "jit/arm64/vixl/Simulator-Constants-vixl.h"
#include "jit/arm64/vixl/Utils-vixl.h"
#include "jit/IonTypes.h"
#include "js/AllocPolicy.h"
#include "vm/MutexIDs.h"
#include "wasm/WasmSignalHandlers.h"

namespace vixl {

// Representation of memory, with typed getters and setters for access.
class Memory {
 public:
  template <typename T>
  static T AddressUntag(T address) {
    // Cast the address using a C-style cast. A reinterpret_cast would be
    // appropriate, but it can't cast one integral type to another.
    uint64_t bits = (uint64_t)address;
    return (T)(bits & ~kAddressTagMask);
  }

  template <typename T, typename A>
  static T Read(A address) {
    T value;
    address = AddressUntag(address);
    VIXL_ASSERT((sizeof(value) == 1) || (sizeof(value) == 2) ||
                (sizeof(value) == 4) || (sizeof(value) == 8) ||
                (sizeof(value) == 16));
    memcpy(&value, reinterpret_cast<const char *>(address), sizeof(value));
    return value;
  }

  template <typename T, typename A>
  static void Write(A address, T value) {
    address = AddressUntag(address);
    VIXL_ASSERT((sizeof(value) == 1) || (sizeof(value) == 2) ||
                (sizeof(value) == 4) || (sizeof(value) == 8) ||
                (sizeof(value) == 16));
    memcpy(reinterpret_cast<char *>(address), &value, sizeof(value));
  }
};

// Represent a register (r0-r31, v0-v31).
template<int kSizeInBytes>
class SimRegisterBase {
 public:
  SimRegisterBase() : written_since_last_log_(false) {}

  // Write the specified value. The value is zero-extended if necessary.
  template<typename T>
  void Set(T new_value) {
    VIXL_STATIC_ASSERT(sizeof(new_value) <= kSizeInBytes);
    if (sizeof(new_value) < kSizeInBytes) {
      // All AArch64 registers are zero-extending.
      memset(value_ + sizeof(new_value), 0, kSizeInBytes - sizeof(new_value));
    }
    memcpy(value_, &new_value, sizeof(new_value));
    NotifyRegisterWrite();
  }

  // Insert a typed value into a register, leaving the rest of the register
  // unchanged. The lane parameter indicates where in the register the value
  // should be inserted, in the range [ 0, sizeof(value_) / sizeof(T) ), where
  // 0 represents the least significant bits.
  template<typename T>
  void Insert(int lane, T new_value) {
    VIXL_ASSERT(lane >= 0);
    VIXL_ASSERT((sizeof(new_value) +
                 (lane * sizeof(new_value))) <= kSizeInBytes);
    memcpy(&value_[lane * sizeof(new_value)], &new_value, sizeof(new_value));
    NotifyRegisterWrite();
  }

  // Read the value as the specified type. The value is truncated if necessary.
  template<typename T>
  T Get(int lane = 0) const {
    T result;
    VIXL_ASSERT(lane >= 0);
    VIXL_ASSERT((sizeof(result) + (lane * sizeof(result))) <= kSizeInBytes);
    memcpy(&result, &value_[lane * sizeof(result)], sizeof(result));
    return result;
  }

  // TODO: Make this return a map of updated bytes, so that we can highlight
  // updated lanes for load-and-insert. (That never happens for scalar code, but
  // NEON has some instructions that can update individual lanes.)
  bool WrittenSinceLastLog() const {
    return written_since_last_log_;
  }

  void NotifyRegisterLogged() {
    written_since_last_log_ = false;
  }

 protected:
  uint8_t value_[kSizeInBytes];

  // Helpers to aid with register tracing.
  bool written_since_last_log_;

  void NotifyRegisterWrite() {
    written_since_last_log_ = true;
  }
};
typedef SimRegisterBase<kXRegSizeInBytes> SimRegister;      // r0-r31
typedef SimRegisterBase<kQRegSizeInBytes> SimVRegister;     // v0-v31

// Representation of a vector register, with typed getters and setters for lanes
// and additional information to represent lane state.
class LogicVRegister {
 public:
  inline LogicVRegister(SimVRegister& other)  // NOLINT
      : register_(other) {
    for (unsigned i = 0; i < sizeof(saturated_) / sizeof(saturated_[0]); i++) {
      saturated_[i] = kNotSaturated;
    }
    for (unsigned i = 0; i < sizeof(round_) / sizeof(round_[0]); i++) {
      round_[i] = 0;
    }
  }

  int64_t Int(VectorFormat vform, int index) const {
    int64_t element;
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: element = register_.Get<int8_t>(index); break;
      case 16: element = register_.Get<int16_t>(index); break;
      case 32: element = register_.Get<int32_t>(index); break;
      case 64: element = register_.Get<int64_t>(index); break;
      default: VIXL_UNREACHABLE(); return 0;
    }
    return element;
  }

  uint64_t Uint(VectorFormat vform, int index) const {
    uint64_t element;
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: element = register_.Get<uint8_t>(index); break;
      case 16: element = register_.Get<uint16_t>(index); break;
      case 32: element = register_.Get<uint32_t>(index); break;
      case 64: element = register_.Get<uint64_t>(index); break;
      default: VIXL_UNREACHABLE(); return 0;
    }
    return element;
  }

  int64_t IntLeftJustified(VectorFormat vform, int index) const {
    return Int(vform, index) << (64 - LaneSizeInBitsFromFormat(vform));
  }

  uint64_t UintLeftJustified(VectorFormat vform, int index) const {
    return Uint(vform, index) << (64 - LaneSizeInBitsFromFormat(vform));
  }

  void SetInt(VectorFormat vform, int index, int64_t value) const {
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: register_.Insert(index, static_cast<int8_t>(value)); break;
      case 16: register_.Insert(index, static_cast<int16_t>(value)); break;
      case 32: register_.Insert(index, static_cast<int32_t>(value)); break;
      case 64: register_.Insert(index, static_cast<int64_t>(value)); break;
      default: VIXL_UNREACHABLE(); return;
    }
  }

  void SetUint(VectorFormat vform, int index, uint64_t value) const {
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: register_.Insert(index, static_cast<uint8_t>(value)); break;
      case 16: register_.Insert(index, static_cast<uint16_t>(value)); break;
      case 32: register_.Insert(index, static_cast<uint32_t>(value)); break;
      case 64: register_.Insert(index, static_cast<uint64_t>(value)); break;
      default: VIXL_UNREACHABLE(); return;
    }
  }

  void ReadUintFromMem(VectorFormat vform, int index, uint64_t addr) const {
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: register_.Insert(index, Memory::Read<uint8_t>(addr)); break;
      case 16: register_.Insert(index, Memory::Read<uint16_t>(addr)); break;
      case 32: register_.Insert(index, Memory::Read<uint32_t>(addr)); break;
      case 64: register_.Insert(index, Memory::Read<uint64_t>(addr)); break;
      default: VIXL_UNREACHABLE(); return;
    }
  }

  void WriteUintToMem(VectorFormat vform, int index, uint64_t addr) const {
    uint64_t value = Uint(vform, index);
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8: Memory::Write(addr, static_cast<uint8_t>(value)); break;
      case 16: Memory::Write(addr, static_cast<uint16_t>(value)); break;
      case 32: Memory::Write(addr, static_cast<uint32_t>(value)); break;
      case 64: Memory::Write(addr, value); break;
    }
  }

  template <typename T>
  T Float(int index) const {
    return register_.Get<T>(index);
  }

  template <typename T>
  void SetFloat(int index, T value) const {
    register_.Insert(index, value);
  }

  // When setting a result in a register of size less than Q, the top bits of
  // the Q register must be cleared.
  void ClearForWrite(VectorFormat vform) const {
    unsigned size = RegisterSizeInBytesFromFormat(vform);
    for (unsigned i = size; i < kQRegSizeInBytes; i++) {
      SetUint(kFormat16B, i, 0);
    }
  }

  // Saturation state for each lane of a vector.
  enum Saturation {
    kNotSaturated = 0,
    kSignedSatPositive = 1 << 0,
    kSignedSatNegative = 1 << 1,
    kSignedSatMask = kSignedSatPositive | kSignedSatNegative,
    kSignedSatUndefined = kSignedSatMask,
    kUnsignedSatPositive = 1 << 2,
    kUnsignedSatNegative = 1 << 3,
    kUnsignedSatMask = kUnsignedSatPositive | kUnsignedSatNegative,
    kUnsignedSatUndefined = kUnsignedSatMask
  };

  // Getters for saturation state.
  Saturation GetSignedSaturation(int index) {
    return static_cast<Saturation>(saturated_[index] & kSignedSatMask);
  }

  Saturation GetUnsignedSaturation(int index) {
    return static_cast<Saturation>(saturated_[index] & kUnsignedSatMask);
  }

  // Setters for saturation state.
  void ClearSat(int index) {
    saturated_[index] = kNotSaturated;
  }

  void SetSignedSat(int index, bool positive) {
    SetSatFlag(index, positive ? kSignedSatPositive : kSignedSatNegative);
  }

  void SetUnsignedSat(int index, bool positive) {
    SetSatFlag(index, positive ? kUnsignedSatPositive : kUnsignedSatNegative);
  }

  void SetSatFlag(int index, Saturation sat) {
    saturated_[index] = static_cast<Saturation>(saturated_[index] | sat);
    VIXL_ASSERT((sat & kUnsignedSatMask) != kUnsignedSatUndefined);
    VIXL_ASSERT((sat & kSignedSatMask) != kSignedSatUndefined);
  }

  // Saturate lanes of a vector based on saturation state.
  LogicVRegister& SignedSaturate(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      Saturation sat = GetSignedSaturation(i);
      if (sat == kSignedSatPositive) {
        SetInt(vform, i, MaxIntFromFormat(vform));
      } else if (sat == kSignedSatNegative) {
        SetInt(vform, i, MinIntFromFormat(vform));
      }
    }
    return *this;
  }

  LogicVRegister& UnsignedSaturate(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      Saturation sat = GetUnsignedSaturation(i);
      if (sat == kUnsignedSatPositive) {
        SetUint(vform, i, MaxUintFromFormat(vform));
      } else if (sat == kUnsignedSatNegative) {
        SetUint(vform, i, 0);
      }
    }
    return *this;
  }

  // Getter for rounding state.
  bool GetRounding(int index) {
    return round_[index];
  }

  // Setter for rounding state.
  void SetRounding(int index, bool round) {
    round_[index] = round;
  }

  // Round lanes of a vector based on rounding state.
  LogicVRegister& Round(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      SetInt(vform, i, Int(vform, i) + (GetRounding(i) ? 1 : 0));
    }
    return *this;
  }

  // Unsigned halve lanes of a vector, and use the saturation state to set the
  // top bit.
  LogicVRegister& Uhalve(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      uint64_t val = Uint(vform, i);
      SetRounding(i, (val & 1) == 1);
      val >>= 1;
      if (GetUnsignedSaturation(i) != kNotSaturated) {
        // If the operation causes unsigned saturation, the bit shifted into the
        // most significant bit must be set.
        val |= (MaxUintFromFormat(vform) >> 1) + 1;
      }
      SetInt(vform, i, val);
    }
    return *this;
  }

  // Signed halve lanes of a vector, and use the carry state to set the top bit.
  LogicVRegister& Halve(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      int64_t val = Int(vform, i);
      SetRounding(i, (val & 1) == 1);
      val >>= 1;
      if (GetSignedSaturation(i) != kNotSaturated) {
        // If the operation causes signed saturation, the sign bit must be
        // inverted.
        val ^= (MaxUintFromFormat(vform) >> 1) + 1;
      }
      SetInt(vform, i, val);
    }
    return *this;
  }

 private:
  SimVRegister& register_;

  // Allocate one saturation state entry per lane; largest register is type Q,
  // and lanes can be a minimum of one byte wide.
  Saturation saturated_[kQRegSizeInBytes];

  // Allocate one rounding state entry per lane.
  bool round_[kQRegSizeInBytes];
};

// The proper way to initialize a simulated system register (such as NZCV) is as
// follows:
//  SimSystemRegister nzcv = SimSystemRegister::DefaultValueFor(NZCV);
class SimSystemRegister {
 public:
  // The default constructor represents a register which has no writable bits.
  // It is not possible to set its value to anything other than 0.
  SimSystemRegister() : value_(0), write_ignore_mask_(0xffffffff) { }

  uint32_t RawValue() const {
    return value_;
  }

  void SetRawValue(uint32_t new_value) {
    value_ = (value_ & write_ignore_mask_) | (new_value & ~write_ignore_mask_);
  }

  uint32_t Bits(int msb, int lsb) const {
    return ExtractUnsignedBitfield32(msb, lsb, value_);
  }

  int32_t SignedBits(int msb, int lsb) const {
    return ExtractSignedBitfield32(msb, lsb, value_);
  }

  void SetBits(int msb, int lsb, uint32_t bits);

  // Default system register values.
  static SimSystemRegister DefaultValueFor(SystemRegister id);

#define DEFINE_GETTER(Name, HighBit, LowBit, Func)                            \
  uint32_t Name() const { return Func(HighBit, LowBit); }              \
  void Set##Name(uint32_t bits) { SetBits(HighBit, LowBit, bits); }
#define DEFINE_WRITE_IGNORE_MASK(Name, Mask)                                  \
  static const uint32_t Name##WriteIgnoreMask = ~static_cast<uint32_t>(Mask);

  SYSTEM_REGISTER_FIELDS_LIST(DEFINE_GETTER, DEFINE_WRITE_IGNORE_MASK)

#undef DEFINE_ZERO_BITS
#undef DEFINE_GETTER

 protected:
  // Most system registers only implement a few of the bits in the word. Other
  // bits are "read-as-zero, write-ignored". The write_ignore_mask argument
  // describes the bits which are not modifiable.
  SimSystemRegister(uint32_t value, uint32_t write_ignore_mask)
      : value_(value), write_ignore_mask_(write_ignore_mask) { }

  uint32_t value_;
  uint32_t write_ignore_mask_;
};


class SimExclusiveLocalMonitor {
 public:
  SimExclusiveLocalMonitor() : kSkipClearProbability(8), seed_(0x87654321) {
    Clear();
  }

  // Clear the exclusive monitor (like clrex).
  void Clear() {
    address_ = 0;
    size_ = 0;
  }

  // Clear the exclusive monitor most of the time.
  void MaybeClear() {
    if ((seed_ % kSkipClearProbability) != 0) {
      Clear();
    }

    // Advance seed_ using a simple linear congruential generator.
    seed_ = (seed_ * 48271) % 2147483647;
  }

  // Mark the address range for exclusive access (like load-exclusive).
  void MarkExclusive(uint64_t address, size_t size) {
    address_ = address;
    size_ = size;
  }

  // Return true if the address range is marked (like store-exclusive).
  // This helper doesn't implicitly clear the monitor.
  bool IsExclusive(uint64_t address, size_t size) {
    VIXL_ASSERT(size > 0);
    // Be pedantic: Require both the address and the size to match.
    return (size == size_) && (address == address_);
  }

 private:
  uint64_t address_;
  size_t size_;

  const int kSkipClearProbability;
  uint32_t seed_;
};


// We can't accurate simulate the global monitor since it depends on external
// influences. Instead, this implementation occasionally causes accesses to
// fail, according to kPassProbability.
class SimExclusiveGlobalMonitor {
 public:
  SimExclusiveGlobalMonitor() : kPassProbability(8), seed_(0x87654321) {}

  bool IsExclusive(uint64_t address, size_t size) {
    USE(address, size);

    bool pass = (seed_ % kPassProbability) != 0;
    // Advance seed_ using a simple linear congruential generator.
    seed_ = (seed_ * 48271) % 2147483647;
    return pass;
  }

 private:
  const int kPassProbability;
  uint32_t seed_;
};

class Redirection;

class Simulator : public DecoderVisitor {
 public:
#ifdef JS_CACHE_SIMULATOR_ARM64
  using Decoder = CachingDecoder;
  mozilla::Atomic<bool> pendingCacheRequests = mozilla::Atomic<bool>{ false };
#endif
  explicit Simulator(Decoder* decoder, FILE* stream = stdout);
  ~Simulator();

  // Moz changes.
  void init(Decoder* decoder, FILE* stream);
  static Simulator* Current();
  static Simulator* Create();
  static void Destroy(Simulator* sim);
  uintptr_t stackLimit() const;
  uintptr_t* addressOfStackLimit();
  bool overRecursed(uintptr_t newsp = 0) const;
  bool overRecursedWithExtra(uint32_t extra) const;
  int64_t call(uint8_t* entry, int argument_count, ...);
  static void* RedirectNativeFunction(void* nativeFunction, js::jit::ABIFunctionType type);
  void setGPR32Result(int32_t result);
  void setGPR64Result(int64_t result);
  void setFP32Result(float result);
  void setFP64Result(double result);
#ifdef JS_CACHE_SIMULATOR_ARM64
  void FlushICache();
#endif
  void VisitCallRedirection(const Instruction* instr);
  static uintptr_t StackLimit() {
    return Simulator::Current()->stackLimit();
  }
  template<typename T> T Read(uintptr_t address);
  template <typename T> void Write(uintptr_t address_, T value);
  JS::ProfilingFrameIterator::RegisterState registerState();

  void ResetState();

  // Run the simulator.
  virtual void Run();
  void RunFrom(const Instruction* first);

  // Simulation helpers.
  const Instruction* pc() const { return pc_; }
  const Instruction* get_pc() const { return pc_; }
  int64_t get_sp() const { return xreg(31, Reg31IsStackPointer); }
  int64_t get_lr() const { return xreg(30); }
  int64_t get_fp() const { return xreg(29); }

  template <typename T>
  T get_pc_as() const { return reinterpret_cast<T>(const_cast<Instruction*>(pc())); }

  void set_pc(const Instruction* new_pc) {
    pc_ = Memory::AddressUntag(new_pc);
    pc_modified_ = true;
  }

  // Handle any wasm faults, returning true if the fault was handled.
  // This method is rather hot so inline the normal (no-wasm) case.
  bool MOZ_ALWAYS_INLINE handle_wasm_seg_fault(uintptr_t addr, unsigned numBytes) {
    if (MOZ_LIKELY(!js::wasm::CodeExists)) {
      return false;
    }

    uint8_t* newPC;
    if (!js::wasm::MemoryAccessTraps(registerState(), (uint8_t*)addr, numBytes, &newPC)) {
      return false;
    }

    set_pc((Instruction*)newPC);
    return true;
  }

  void increment_pc() {
    if (!pc_modified_) {
      pc_ = pc_->NextInstruction();
    }

    pc_modified_ = false;
  }

  void ExecuteInstruction();

  // Declare all Visitor functions.
  #define DECLARE(A) virtual void Visit##A(const Instruction* instr) override;
  VISITOR_LIST_THAT_RETURN(DECLARE)
  VISITOR_LIST_THAT_DONT_RETURN(DECLARE)
  #undef DECLARE


  // Integer register accessors.

  // Basic accessor: Read the register as the specified type.
  template<typename T>
  T reg(unsigned code, Reg31Mode r31mode = Reg31IsZeroRegister) const {
    VIXL_ASSERT(code < kNumberOfRegisters);
    if ((code == 31) && (r31mode == Reg31IsZeroRegister)) {
      T result;
      memset(&result, 0, sizeof(result));
      return result;
    }
    return registers_[code].Get<T>();
  }

  // Common specialized accessors for the reg() template.
  int32_t wreg(unsigned code,
               Reg31Mode r31mode = Reg31IsZeroRegister) const {
    return reg<int32_t>(code, r31mode);
  }

  int64_t xreg(unsigned code,
               Reg31Mode r31mode = Reg31IsZeroRegister) const {
    return reg<int64_t>(code, r31mode);
  }

  // As above, with parameterized size and return type. The value is
  // either zero-extended or truncated to fit, as required.
  template<typename T>
  T reg(unsigned size, unsigned code,
        Reg31Mode r31mode = Reg31IsZeroRegister) const {
    uint64_t raw;
    switch (size) {
      case kWRegSize: raw = reg<uint32_t>(code, r31mode); break;
      case kXRegSize: raw = reg<uint64_t>(code, r31mode); break;
      default:
        VIXL_UNREACHABLE();
        return 0;
    }

    T result;
    VIXL_STATIC_ASSERT(sizeof(result) <= sizeof(raw));
    // Copy the result and truncate to fit. This assumes a little-endian host.
    memcpy(&result, &raw, sizeof(result));
    return result;
  }

  // Use int64_t by default if T is not specified.
  int64_t reg(unsigned size, unsigned code,
              Reg31Mode r31mode = Reg31IsZeroRegister) const {
    return reg<int64_t>(size, code, r31mode);
  }

  enum RegLogMode {
    LogRegWrites,
    NoRegLog
  };

  // Write 'value' into an integer register. The value is zero-extended. This
  // behaviour matches AArch64 register writes.
  template<typename T>
  void set_reg(unsigned code, T value,
               RegLogMode log_mode = LogRegWrites,
               Reg31Mode r31mode = Reg31IsZeroRegister) {
    if (sizeof(T) < kWRegSizeInBytes) {
      // We use a C-style cast on purpose here.
      // Since we do not have access to 'constepxr if', the casts in this `if`
      // must be valid even if we know the code will never be executed, in
      // particular when `T` is a pointer type.
      int64_t tmp_64bit = (int64_t)value;
      int32_t tmp_32bit = static_cast<int32_t>(tmp_64bit);
      set_reg<int32_t>(code, tmp_32bit, log_mode, r31mode);
      return;
    }

    VIXL_ASSERT((sizeof(T) == kWRegSizeInBytes) ||
                (sizeof(T) == kXRegSizeInBytes));
    VIXL_ASSERT(code < kNumberOfRegisters);

    if ((code == 31) && (r31mode == Reg31IsZeroRegister)) {
      return;
    }

    registers_[code].Set(value);

    if (log_mode == LogRegWrites) LogRegister(code, r31mode);
  }

  // Common specialized accessors for the set_reg() template.
  void set_wreg(unsigned code, int32_t value,
                RegLogMode log_mode = LogRegWrites,
                Reg31Mode r31mode = Reg31IsZeroRegister) {
    set_reg(code, value, log_mode, r31mode);
  }

  void set_xreg(unsigned code, int64_t value,
                RegLogMode log_mode = LogRegWrites,
                Reg31Mode r31mode = Reg31IsZeroRegister) {
    set_reg(code, value, log_mode, r31mode);
  }

  // As above, with parameterized size and type. The value is either
  // zero-extended or truncated to fit, as required.
  template<typename T>
  void set_reg(unsigned size, unsigned code, T value,
               RegLogMode log_mode = LogRegWrites,
               Reg31Mode r31mode = Reg31IsZeroRegister) {
    // Zero-extend the input.
    uint64_t raw = 0;
    VIXL_STATIC_ASSERT(sizeof(value) <= sizeof(raw));
    memcpy(&raw, &value, sizeof(value));

    // Write (and possibly truncate) the value.
    switch (size) {
      case kWRegSize:
        set_reg(code, static_cast<uint32_t>(raw), log_mode, r31mode);
        break;
      case kXRegSize:
        set_reg(code, raw, log_mode, r31mode);
        break;
      default:
        VIXL_UNREACHABLE();
        return;
    }
  }

  // Common specialized accessors for the set_reg() template.

  // Commonly-used special cases.
  template<typename T>
  void set_lr(T value) {
    set_reg(kLinkRegCode, value);
  }

  template<typename T>
  void set_sp(T value) {
    set_reg(31, value, LogRegWrites, Reg31IsStackPointer);
  }

  // Vector register accessors.
  // These are equivalent to the integer register accessors, but for vector
  // registers.

  // A structure for representing a 128-bit Q register.
  struct qreg_t { uint8_t val[kQRegSizeInBytes]; };

  // Basic accessor: read the register as the specified type.
  template<typename T>
  T vreg(unsigned code) const {
    VIXL_STATIC_ASSERT((sizeof(T) == kBRegSizeInBytes) ||
                       (sizeof(T) == kHRegSizeInBytes) ||
                       (sizeof(T) == kSRegSizeInBytes) ||
                       (sizeof(T) == kDRegSizeInBytes) ||
                       (sizeof(T) == kQRegSizeInBytes));
    VIXL_ASSERT(code < kNumberOfVRegisters);

    return vregisters_[code].Get<T>();
  }

  // Common specialized accessors for the vreg() template.
  int8_t breg(unsigned code) const {
    return vreg<int8_t>(code);
  }

  int16_t hreg(unsigned code) const {
    return vreg<int16_t>(code);
  }

  float sreg(unsigned code) const {
    return vreg<float>(code);
  }

  uint32_t sreg_bits(unsigned code) const {
    return vreg<uint32_t>(code);
  }

  double dreg(unsigned code) const {
    return vreg<double>(code);
  }

  uint64_t dreg_bits(unsigned code) const {
    return vreg<uint64_t>(code);
  }

  qreg_t qreg(unsigned code)  const {
    return vreg<qreg_t>(code);
  }

  // As above, with parameterized size and return type. The value is
  // either zero-extended or truncated to fit, as required.
  template<typename T>
  T vreg(unsigned size, unsigned code) const {
    uint64_t raw = 0;
    T result;

    switch (size) {
      case kSRegSize: raw = vreg<uint32_t>(code); break;
      case kDRegSize: raw = vreg<uint64_t>(code); break;
      default:
        VIXL_UNREACHABLE();
        break;
    }

    VIXL_STATIC_ASSERT(sizeof(result) <= sizeof(raw));
    // Copy the result and truncate to fit. This assumes a little-endian host.
    memcpy(&result, &raw, sizeof(result));
    return result;
  }

  inline SimVRegister& vreg(unsigned code) {
    return vregisters_[code];
  }

  // Basic accessor: Write the specified value.
  template<typename T>
  void set_vreg(unsigned code, T value,
                RegLogMode log_mode = LogRegWrites) {
    VIXL_STATIC_ASSERT((sizeof(value) == kBRegSizeInBytes) ||
                       (sizeof(value) == kHRegSizeInBytes) ||
                       (sizeof(value) == kSRegSizeInBytes) ||
                       (sizeof(value) == kDRegSizeInBytes) ||
                       (sizeof(value) == kQRegSizeInBytes));
    VIXL_ASSERT(code < kNumberOfVRegisters);
    vregisters_[code].Set(value);

    if (log_mode == LogRegWrites) {
      LogVRegister(code, GetPrintRegisterFormat(value));
    }
  }

  // Common specialized accessors for the set_vreg() template.
  void set_breg(unsigned code, int8_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_hreg(unsigned code, int16_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_sreg(unsigned code, float value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_sreg_bits(unsigned code, uint32_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_dreg(unsigned code, double value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_dreg_bits(unsigned code, uint64_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  void set_qreg(unsigned code, qreg_t value,
                RegLogMode log_mode = LogRegWrites) {
    set_vreg(code, value, log_mode);
  }

  bool N() const { return nzcv_.N() != 0; }
  bool Z() const { return nzcv_.Z() != 0; }
  bool C() const { return nzcv_.C() != 0; }
  bool V() const { return nzcv_.V() != 0; }

  SimSystemRegister& ReadNzcv() { return nzcv_; }
  SimSystemRegister& nzcv() { return nzcv_; }

  // TODO: Find a way to make the fpcr_ members return the proper types, so
  // these accessors are not necessary.
  FPRounding RMode() { return static_cast<FPRounding>(fpcr_.RMode()); }
  bool DN() { return fpcr_.DN() != 0; }
  SimSystemRegister& fpcr() { return fpcr_; }

  UseDefaultNaN ReadDN() const {
    return fpcr_.DN() != 0 ? kUseDefaultNaN : kIgnoreDefaultNaN;
  }

  // Specify relevant register formats for Print(V)Register and related helpers.
  enum PrintRegisterFormat {
    // The lane size.
    kPrintRegLaneSizeB = 0 << 0,
    kPrintRegLaneSizeH = 1 << 0,
    kPrintRegLaneSizeS = 2 << 0,
    kPrintRegLaneSizeW = kPrintRegLaneSizeS,
    kPrintRegLaneSizeD = 3 << 0,
    kPrintRegLaneSizeX = kPrintRegLaneSizeD,
    kPrintRegLaneSizeQ = 4 << 0,

    kPrintRegLaneSizeOffset = 0,
    kPrintRegLaneSizeMask = 7 << 0,

    // The lane count.
    kPrintRegAsScalar = 0,
    kPrintRegAsDVector = 1 << 3,
    kPrintRegAsQVector = 2 << 3,

    kPrintRegAsVectorMask = 3 << 3,

    // Indicate floating-point format lanes. (This flag is only supported for S-
    // and D-sized lanes.)
    kPrintRegAsFP = 1 << 5,

    // Supported combinations.

    kPrintXReg = kPrintRegLaneSizeX | kPrintRegAsScalar,
    kPrintWReg = kPrintRegLaneSizeW | kPrintRegAsScalar,
    kPrintSReg = kPrintRegLaneSizeS | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintDReg = kPrintRegLaneSizeD | kPrintRegAsScalar | kPrintRegAsFP,

    kPrintReg1B = kPrintRegLaneSizeB | kPrintRegAsScalar,
    kPrintReg8B = kPrintRegLaneSizeB | kPrintRegAsDVector,
    kPrintReg16B = kPrintRegLaneSizeB | kPrintRegAsQVector,
    kPrintReg1H = kPrintRegLaneSizeH | kPrintRegAsScalar,
    kPrintReg4H = kPrintRegLaneSizeH | kPrintRegAsDVector,
    kPrintReg8H = kPrintRegLaneSizeH | kPrintRegAsQVector,
    kPrintReg1S = kPrintRegLaneSizeS | kPrintRegAsScalar,
    kPrintReg2S = kPrintRegLaneSizeS | kPrintRegAsDVector,
    kPrintReg4S = kPrintRegLaneSizeS | kPrintRegAsQVector,
    kPrintReg1SFP = kPrintRegLaneSizeS | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintReg2SFP = kPrintRegLaneSizeS | kPrintRegAsDVector | kPrintRegAsFP,
    kPrintReg4SFP = kPrintRegLaneSizeS | kPrintRegAsQVector | kPrintRegAsFP,
    kPrintReg1D = kPrintRegLaneSizeD | kPrintRegAsScalar,
    kPrintReg2D = kPrintRegLaneSizeD | kPrintRegAsQVector,
    kPrintReg1DFP = kPrintRegLaneSizeD | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintReg2DFP = kPrintRegLaneSizeD | kPrintRegAsQVector | kPrintRegAsFP,
    kPrintReg1Q = kPrintRegLaneSizeQ | kPrintRegAsScalar
  };

  unsigned GetPrintRegLaneSizeInBytesLog2(PrintRegisterFormat format) {
    return (format & kPrintRegLaneSizeMask) >> kPrintRegLaneSizeOffset;
  }

  unsigned GetPrintRegLaneSizeInBytes(PrintRegisterFormat format) {
    return 1 << GetPrintRegLaneSizeInBytesLog2(format);
  }

  unsigned GetPrintRegSizeInBytesLog2(PrintRegisterFormat format) {
    if (format & kPrintRegAsDVector) return kDRegSizeInBytesLog2;
    if (format & kPrintRegAsQVector) return kQRegSizeInBytesLog2;

    // Scalar types.
    return GetPrintRegLaneSizeInBytesLog2(format);
  }

  unsigned GetPrintRegSizeInBytes(PrintRegisterFormat format) {
    return 1 << GetPrintRegSizeInBytesLog2(format);
  }

  unsigned GetPrintRegLaneCount(PrintRegisterFormat format) {
    unsigned reg_size_log2 = GetPrintRegSizeInBytesLog2(format);
    unsigned lane_size_log2 = GetPrintRegLaneSizeInBytesLog2(format);
    VIXL_ASSERT(reg_size_log2 >= lane_size_log2);
    return 1 << (reg_size_log2 - lane_size_log2);
  }

  PrintRegisterFormat GetPrintRegisterFormatForSize(unsigned reg_size,
                                                    unsigned lane_size);

  PrintRegisterFormat GetPrintRegisterFormatForSize(unsigned size) {
    return GetPrintRegisterFormatForSize(size, size);
  }

  PrintRegisterFormat GetPrintRegisterFormatForSizeFP(unsigned size) {
    switch (size) {
      default: VIXL_UNREACHABLE(); return kPrintDReg;
      case kDRegSizeInBytes: return kPrintDReg;
      case kSRegSizeInBytes: return kPrintSReg;
    }
  }

  PrintRegisterFormat GetPrintRegisterFormatTryFP(PrintRegisterFormat format) {
    if ((GetPrintRegLaneSizeInBytes(format) == kSRegSizeInBytes) ||
        (GetPrintRegLaneSizeInBytes(format) == kDRegSizeInBytes)) {
      return static_cast<PrintRegisterFormat>(format | kPrintRegAsFP);
    }
    return format;
  }

  template<typename T>
  PrintRegisterFormat GetPrintRegisterFormat(T value) {
    return GetPrintRegisterFormatForSize(sizeof(value));
  }

  PrintRegisterFormat GetPrintRegisterFormat(double value) {
    VIXL_STATIC_ASSERT(sizeof(value) == kDRegSizeInBytes);
    return GetPrintRegisterFormatForSizeFP(sizeof(value));
  }

  PrintRegisterFormat GetPrintRegisterFormat(float value) {
    VIXL_STATIC_ASSERT(sizeof(value) == kSRegSizeInBytes);
    return GetPrintRegisterFormatForSizeFP(sizeof(value));
  }

  PrintRegisterFormat GetPrintRegisterFormat(VectorFormat vform);

  // Print all registers of the specified types.
  void PrintRegisters();
  void PrintVRegisters();
  void PrintSystemRegisters();

  // As above, but only print the registers that have been updated.
  void PrintWrittenRegisters();
  void PrintWrittenVRegisters();

  // As above, but respect LOG_REG and LOG_VREG.
  inline void LogWrittenRegisters() {
    if (trace_parameters() & LOG_REGS) PrintWrittenRegisters();
  }
  inline void LogWrittenVRegisters() {
    if (trace_parameters() & LOG_VREGS) PrintWrittenVRegisters();
  }
  inline void LogAllWrittenRegisters() {
    LogWrittenRegisters();
    LogWrittenVRegisters();
  }

  // Print individual register values (after update).
  void PrintRegister(unsigned code, Reg31Mode r31mode = Reg31IsStackPointer);
  void PrintVRegister(unsigned code, PrintRegisterFormat format);
  void PrintSystemRegister(SystemRegister id);

  // Like Print* (above), but respect trace_parameters().
  void LogRegister(unsigned code, Reg31Mode r31mode = Reg31IsStackPointer) {
    if (trace_parameters() & LOG_REGS) PrintRegister(code, r31mode);
  }
  void LogVRegister(unsigned code, PrintRegisterFormat format) {
    if (trace_parameters() & LOG_VREGS) PrintVRegister(code, format);
  }
  void LogSystemRegister(SystemRegister id) {
    if (trace_parameters() & LOG_SYSREGS) PrintSystemRegister(id);
  }

  // Print memory accesses.
  void PrintRead(uintptr_t address, unsigned reg_code,
                 PrintRegisterFormat format);
  void PrintWrite(uintptr_t address, unsigned reg_code,
                 PrintRegisterFormat format);
  void PrintVRead(uintptr_t address, unsigned reg_code,
                  PrintRegisterFormat format, unsigned lane);
  void PrintVWrite(uintptr_t address, unsigned reg_code,
                   PrintRegisterFormat format, unsigned lane);

  // Like Print* (above), but respect trace_parameters().
  void LogRead(uintptr_t address, unsigned reg_code,
               PrintRegisterFormat format) {
    if (trace_parameters() & LOG_REGS) PrintRead(address, reg_code, format);
  }
  void LogWrite(uintptr_t address, unsigned reg_code,
                PrintRegisterFormat format) {
    if (trace_parameters() & LOG_WRITE) PrintWrite(address, reg_code, format);
  }
  void LogVRead(uintptr_t address, unsigned reg_code,
                PrintRegisterFormat format, unsigned lane = 0) {
    if (trace_parameters() & LOG_VREGS) {
      PrintVRead(address, reg_code, format, lane);
    }
  }
  void LogVWrite(uintptr_t address, unsigned reg_code,
                 PrintRegisterFormat format, unsigned lane = 0) {
    if (trace_parameters() & LOG_WRITE) {
      PrintVWrite(address, reg_code, format, lane);
    }
  }

  // Helper functions for register tracing.
  void PrintRegisterRawHelper(unsigned code, Reg31Mode r31mode,
                              int size_in_bytes = kXRegSizeInBytes);
  void PrintVRegisterRawHelper(unsigned code, int bytes = kQRegSizeInBytes,
                               int lsb = 0);
  void PrintVRegisterFPHelper(unsigned code, unsigned lane_size_in_bytes,
                              int lane_count = 1, int rightmost_lane = 0);

  void DoUnreachable(const Instruction* instr);
  void DoTrace(const Instruction* instr);
  void DoLog(const Instruction* instr);

  static const char* WRegNameForCode(unsigned code,
                                     Reg31Mode mode = Reg31IsZeroRegister);
  static const char* XRegNameForCode(unsigned code,
                                     Reg31Mode mode = Reg31IsZeroRegister);
  static const char* SRegNameForCode(unsigned code);
  static const char* DRegNameForCode(unsigned code);
  static const char* VRegNameForCode(unsigned code);

  bool coloured_trace() const { return coloured_trace_; }
  void set_coloured_trace(bool value);

  int trace_parameters() const { return trace_parameters_; }
  void set_trace_parameters(int parameters);

  void set_instruction_stats(bool value);

  // Clear the simulated local monitor to force the next store-exclusive
  // instruction to fail.
  void ClearLocalMonitor() {
    local_monitor_.Clear();
  }

  void SilenceExclusiveAccessWarning() {
    print_exclusive_access_warning_ = false;
  }

 protected:
  const char* clr_normal;
  const char* clr_flag_name;
  const char* clr_flag_value;
  const char* clr_reg_name;
  const char* clr_reg_value;
  const char* clr_vreg_name;
  const char* clr_vreg_value;
  const char* clr_memory_address;
  const char* clr_warning;
  const char* clr_warning_message;
  const char* clr_printf;

  // Simulation helpers ------------------------------------
  bool ConditionPassed(Condition cond) {
    switch (cond) {
      case eq:
        return Z();
      case ne:
        return !Z();
      case hs:
        return C();
      case lo:
        return !C();
      case mi:
        return N();
      case pl:
        return !N();
      case vs:
        return V();
      case vc:
        return !V();
      case hi:
        return C() && !Z();
      case ls:
        return !(C() && !Z());
      case ge:
        return N() == V();
      case lt:
        return N() != V();
      case gt:
        return !Z() && (N() == V());
      case le:
        return !(!Z() && (N() == V()));
      case nv:
        VIXL_FALLTHROUGH();
      case al:
        return true;
      default:
        VIXL_UNREACHABLE();
        return false;
    }
  }

  bool ConditionPassed(Instr cond) {
    return ConditionPassed(static_cast<Condition>(cond));
  }

  bool ConditionFailed(Condition cond) {
    return !ConditionPassed(cond);
  }

  void AddSubHelper(const Instruction* instr, int64_t op2);
  uint64_t AddWithCarry(unsigned reg_size,
                        bool set_flags,
                        uint64_t left,
                        uint64_t right,
                        int carry_in = 0);
  void LogicalHelper(const Instruction* instr, int64_t op2);
  void ConditionalCompareHelper(const Instruction* instr, int64_t op2);
  void LoadStoreHelper(const Instruction* instr,
                       int64_t offset,
                       AddrMode addrmode);
  void LoadStorePairHelper(const Instruction* instr, AddrMode addrmode);
  template <typename T>
  void CompareAndSwapHelper(const Instruction* instr);
  template <typename T>
  void CompareAndSwapPairHelper(const Instruction* instr);
  template <typename T>
  void AtomicMemorySimpleHelper(const Instruction* instr);
  template <typename T>
  void AtomicMemorySwapHelper(const Instruction* instr);
  template <typename T>
  void LoadAcquireRCpcHelper(const Instruction* instr);
  uintptr_t AddressModeHelper(unsigned addr_reg,
                              int64_t offset,
                              AddrMode addrmode);
  void NEONLoadStoreMultiStructHelper(const Instruction* instr,
                                      AddrMode addr_mode);
  void NEONLoadStoreSingleStructHelper(const Instruction* instr,
                                       AddrMode addr_mode);

  uint64_t AddressUntag(uint64_t address) {
    return address & ~kAddressTagMask;
  }

  template <typename T>
  T* AddressUntag(T* address) {
    uintptr_t address_raw = reinterpret_cast<uintptr_t>(address);
    return reinterpret_cast<T*>(AddressUntag(address_raw));
  }

  int64_t ShiftOperand(unsigned reg_size,
                       int64_t value,
                       Shift shift_type,
                       unsigned amount);
  int64_t Rotate(unsigned reg_width,
                 int64_t value,
                 Shift shift_type,
                 unsigned amount);
  int64_t ExtendValue(unsigned reg_width,
                      int64_t value,
                      Extend extend_type,
                      unsigned left_shift = 0);
  uint16_t PolynomialMult(uint8_t op1, uint8_t op2);

  void ld1(VectorFormat vform,
           LogicVRegister dst,
           uint64_t addr);
  void ld1(VectorFormat vform,
           LogicVRegister dst,
           int index,
           uint64_t addr);
  void ld1r(VectorFormat vform,
            LogicVRegister dst,
            uint64_t addr);
  void ld2(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           uint64_t addr);
  void ld2(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           int index,
           uint64_t addr);
  void ld2r(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           uint64_t addr);
  void ld3(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           uint64_t addr);
  void ld3(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           int index,
           uint64_t addr);
  void ld3r(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           uint64_t addr);
  void ld4(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           LogicVRegister dst4,
           uint64_t addr);
  void ld4(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           LogicVRegister dst4,
           int index,
           uint64_t addr);
  void ld4r(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           LogicVRegister dst4,
           uint64_t addr);
  void st1(VectorFormat vform,
           LogicVRegister src,
           uint64_t addr);
  void st1(VectorFormat vform,
           LogicVRegister src,
           int index,
           uint64_t addr);
  void st2(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           uint64_t addr);
  void st2(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           int index,
           uint64_t addr);
  void st3(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           uint64_t addr);
  void st3(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           int index,
           uint64_t addr);
  void st4(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           LogicVRegister src4,
           uint64_t addr);
  void st4(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           LogicVRegister src4,
           int index,
           uint64_t addr);
  LogicVRegister cmp(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     Condition cond);
  LogicVRegister cmp(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     int imm,
                     Condition cond);
  LogicVRegister cmptst(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister add(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister addp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister mla(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister mls(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister mul(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister mul(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister mla(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister mls(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister pmul(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);

  typedef LogicVRegister (Simulator::*ByElementOp)(VectorFormat vform,
                                                   LogicVRegister dst,
                                                   const LogicVRegister& src1,
                                                   const LogicVRegister& src2,
                                                   int index);
  LogicVRegister fmul(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister fmla(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister fmls(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister fmulx(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smull(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smull2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister umull(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister umull2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister smlal(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smlal2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister umlal(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister umlal2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister smlsl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smlsl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister umlsl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister umlsl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister sqdmull(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqdmull2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sqdmlal(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqdmlal2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sqdmlsl(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqdmlsl2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sqdmulh(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqrdmulh(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sub(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister and_(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister orr(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister orn(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister eor(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bic(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bic(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     uint64_t imm);
  LogicVRegister bif(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bit(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bsl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister cls(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister clz(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister cnt(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister not_(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister rbit(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister rev(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int revSize);
  LogicVRegister rev16(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister rev32(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister rev64(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister addlp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       bool is_signed,
                       bool do_accumulate);
  LogicVRegister saddlp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister uaddlp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister sadalp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister uadalp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister ext(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister ins_element(VectorFormat vform,
                             LogicVRegister dst,
                             int dst_index,
                             const LogicVRegister& src,
                             int src_index);
  LogicVRegister ins_immediate(VectorFormat vform,
                               LogicVRegister dst,
                               int dst_index,
                               uint64_t imm);
  LogicVRegister dup_element(VectorFormat vform,
                             LogicVRegister dst,
                             const LogicVRegister& src,
                             int src_index);
  LogicVRegister dup_immediate(VectorFormat vform,
                               LogicVRegister dst,
                               uint64_t imm);
  LogicVRegister mov(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);                               
  LogicVRegister movi(VectorFormat vform,
                      LogicVRegister dst,
                      uint64_t imm);
  LogicVRegister mvni(VectorFormat vform,
                      LogicVRegister dst,
                      uint64_t imm);
  LogicVRegister orr(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     uint64_t imm);
  LogicVRegister sshl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister ushl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister sminmax(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         bool max);
  LogicVRegister smax(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister smin(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister sminmaxp(VectorFormat vform,
                          LogicVRegister dst,
                          int dst_index,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister smaxp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister sminp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister addp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister addv(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister uaddlv(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister saddlv(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister sminmaxv(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister smaxv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sminv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister uxtl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister uxtl2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sxtl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister sxtl2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& ind);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& ind);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& ind);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& tab4,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& tab4,
                     const LogicVRegister& ind);
  LogicVRegister uaddl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uaddl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister uaddw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uaddw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister saddl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister saddl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister saddw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister saddw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister usubl(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  LogicVRegister usubl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister usubw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister usubw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister ssubl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister ssubl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister ssubw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister ssubw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister uminmax(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         bool max);
  LogicVRegister umax(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister umin(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister uminmaxp(VectorFormat vform,
                          LogicVRegister dst,
                          int dst_index,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister umaxp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uminp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uminmaxv(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister umaxv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister uminv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister trn1(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister trn2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister zip1(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister zip2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uzp1(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uzp2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister shl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int shift);
  LogicVRegister scvtf(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int fbits,
                       FPRounding rounding_mode);
  LogicVRegister ucvtf(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int fbits,
                       FPRounding rounding_mode);
  LogicVRegister sshll(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister sshll2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister shll(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister shll2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister ushll(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister ushll2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister sli(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int shift);
  LogicVRegister sri(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int shift);
  LogicVRegister sshr(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister ushr(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister ssra(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister usra(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister srsra(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister ursra(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister suqadd(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister usqadd(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sqshl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister uqshl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister sqshlu(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister abs(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister neg(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister extractnarrow(VectorFormat vform,
                               LogicVRegister dst,
                               bool dstIsSigned,
                               const LogicVRegister& src,
                               bool srcIsSigned);
  LogicVRegister xtn(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister sqxtn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister uqxtn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sqxtun(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister absdiff(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         bool issigned);
  LogicVRegister saba(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uaba(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister shrn(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister shrn2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister rshrn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister rshrn2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister uqshrn(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister uqshrn2(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister uqrshrn(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister uqrshrn2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqshrn(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister sqshrn2(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister sqrshrn(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister sqrshrn2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqshrun(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister sqshrun2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqrshrun(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqrshrun2(VectorFormat vform,
                           LogicVRegister dst,
                           const LogicVRegister& src,
                           int shift);
  LogicVRegister sqrdmulh(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          bool round = true);
  LogicVRegister sqdmulh(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  #define NEON_3VREG_LOGIC_LIST(V) \
    V(addhn)                       \
    V(addhn2)                      \
    V(raddhn)                      \
    V(raddhn2)                     \
    V(subhn)                       \
    V(subhn2)                      \
    V(rsubhn)                      \
    V(rsubhn2)                     \
    V(pmull)                       \
    V(pmull2)                      \
    V(sabal)                       \
    V(sabal2)                      \
    V(uabal)                       \
    V(uabal2)                      \
    V(sabdl)                       \
    V(sabdl2)                      \
    V(uabdl)                       \
    V(uabdl2)                      \
    V(smull)                       \
    V(smull2)                      \
    V(umull)                       \
    V(umull2)                      \
    V(smlal)                       \
    V(smlal2)                      \
    V(umlal)                       \
    V(umlal2)                      \
    V(smlsl)                       \
    V(smlsl2)                      \
    V(umlsl)                       \
    V(umlsl2)                      \
    V(sqdmlal)                     \
    V(sqdmlal2)                    \
    V(sqdmlsl)                     \
    V(sqdmlsl2)                    \
    V(sqdmull)                     \
    V(sqdmull2)

  #define DEFINE_LOGIC_FUNC(FXN)                   \
    LogicVRegister FXN(VectorFormat vform,         \
                       LogicVRegister dst,         \
                       const LogicVRegister& src1, \
                       const LogicVRegister& src2);
  NEON_3VREG_LOGIC_LIST(DEFINE_LOGIC_FUNC)
  #undef DEFINE_LOGIC_FUNC

  #define NEON_FP3SAME_LIST(V)  \
    V(fadd,   FPAdd,   false)   \
    V(fsub,   FPSub,   true)    \
    V(fmul,   FPMul,   true)    \
    V(fmulx,  FPMulx,  true)    \
    V(fdiv,   FPDiv,   true)    \
    V(fmax,   FPMax,   false)   \
    V(fmin,   FPMin,   false)   \
    V(fmaxnm, FPMaxNM, false)   \
    V(fminnm, FPMinNM, false)

  #define DECLARE_NEON_FP_VECTOR_OP(FN, OP, PROCNAN) \
    template <typename T>                            \
    LogicVRegister FN(VectorFormat vform,            \
                      LogicVRegister dst,            \
                      const LogicVRegister& src1,    \
                      const LogicVRegister& src2);   \
    LogicVRegister FN(VectorFormat vform,            \
                      LogicVRegister dst,            \
                      const LogicVRegister& src1,    \
                      const LogicVRegister& src2);
  NEON_FP3SAME_LIST(DECLARE_NEON_FP_VECTOR_OP)
  #undef DECLARE_NEON_FP_VECTOR_OP

  #define NEON_FPPAIRWISE_LIST(V)         \
    V(faddp,   fadd,   FPAdd)             \
    V(fmaxp,   fmax,   FPMax)             \
    V(fmaxnmp, fmaxnm, FPMaxNM)           \
    V(fminp,   fmin,   FPMin)             \
    V(fminnmp, fminnm, FPMinNM)

  #define DECLARE_NEON_FP_PAIR_OP(FNP, FN, OP)       \
    LogicVRegister FNP(VectorFormat vform,           \
                       LogicVRegister dst,           \
                       const LogicVRegister& src1,   \
                       const LogicVRegister& src2);  \
    LogicVRegister FNP(VectorFormat vform,           \
                       LogicVRegister dst,           \
                       const LogicVRegister& src);
  NEON_FPPAIRWISE_LIST(DECLARE_NEON_FP_PAIR_OP)
  #undef DECLARE_NEON_FP_PAIR_OP

  template <typename T>
  LogicVRegister frecps(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister frecps(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  template <typename T>
  LogicVRegister frsqrts(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  LogicVRegister frsqrts(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  template <typename T>
  LogicVRegister fmla(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister fmla(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  template <typename T>
  LogicVRegister fmls(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister fmls(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister fnmul(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);

  template <typename T>
  LogicVRegister fcmp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      Condition cond);
  LogicVRegister fcmp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      Condition cond);
  LogicVRegister fabscmp(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         Condition cond);
  LogicVRegister fcmp_zero(VectorFormat vform,
                           LogicVRegister dst,
                           const LogicVRegister& src,
                           Condition cond);

  template <typename T>
  LogicVRegister fneg(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister fneg(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  template <typename T>
  LogicVRegister frecpx(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister frecpx(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  template <typename T>
  LogicVRegister fabs_(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fabs_(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fabd(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister frint(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       FPRounding rounding_mode,
                       bool inexact_exception = false);
  LogicVRegister fcvts(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       FPRounding rounding_mode,
                       int fbits = 0);
  LogicVRegister fcvtu(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       FPRounding rounding_mode,
                       int fbits = 0);
  LogicVRegister fcvtl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fcvtl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister fcvtn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fcvtn2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister fcvtxn(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister fcvtxn2(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister fsqrt(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister frsqrte(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister frecpe(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        FPRounding rounding);
  LogicVRegister ursqrte(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister urecpe(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);

  typedef float (Simulator::*FPMinMaxOp)(float a, float b);

  LogicVRegister fminmaxv(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          FPMinMaxOp Op);

  LogicVRegister fminv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fmaxv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fminnmv(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister fmaxnmv(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);

  static const uint32_t CRC32_POLY  = 0x04C11DB7;
  static const uint32_t CRC32C_POLY = 0x1EDC6F41;
  uint32_t Poly32Mod2(unsigned n, uint64_t data, uint32_t poly);
  template <typename T>
  uint32_t Crc32Checksum(uint32_t acc, T val, uint32_t poly);
  uint32_t Crc32Checksum(uint32_t acc, uint64_t val, uint32_t poly);

  void SysOp_W(int op, int64_t val);

  template <typename T>
  T FPRecipSqrtEstimate(T op);
  template <typename T>
  T FPRecipEstimate(T op, FPRounding rounding);
  template <typename T, typename R>
  R FPToFixed(T op, int fbits, bool is_signed, FPRounding rounding);

  void FPCompare(double val0, double val1, FPTrapFlags trap);
  double FPRoundInt(double value, FPRounding round_mode);
  double recip_sqrt_estimate(double a);
  double recip_estimate(double a);
  double FPRecipSqrtEstimate(double a);
  double FPRecipEstimate(double a);
  double FixedToDouble(int64_t src, int fbits, FPRounding round_mode);
  double UFixedToDouble(uint64_t src, int fbits, FPRounding round_mode);
  float FixedToFloat(int64_t src, int fbits, FPRounding round_mode);
  float UFixedToFloat(uint64_t src, int fbits, FPRounding round_mode);
  int32_t FPToInt32(double value, FPRounding rmode);
  int64_t FPToInt64(double value, FPRounding rmode);
  uint32_t FPToUInt32(double value, FPRounding rmode);
  uint64_t FPToUInt64(double value, FPRounding rmode);
  int32_t FPToFixedJS(double value);

  template <typename T>
  T FPAdd(T op1, T op2);

  template <typename T>
  T FPDiv(T op1, T op2);

  template <typename T>
  T FPMax(T a, T b);

  template <typename T>
  T FPMaxNM(T a, T b);

  template <typename T>
  T FPMin(T a, T b);

  template <typename T>
  T FPMinNM(T a, T b);

  template <typename T>
  T FPMul(T op1, T op2);

  template <typename T>
  T FPMulx(T op1, T op2);

  template <typename T>
  T FPMulAdd(T a, T op1, T op2);

  template <typename T>
  T FPSqrt(T op);

  template <typename T>
  T FPSub(T op1, T op2);

  template <typename T>
  T FPRecipStepFused(T op1, T op2);

  template <typename T>
  T FPRSqrtStepFused(T op1, T op2);

  // This doesn't do anything at the moment. We'll need it if we want support
  // for cumulative exception bits or floating-point exceptions.
  void FPProcessException() { }

  bool FPProcessNaNs(const Instruction* instr);

  // Pseudo Printf instruction
  void DoPrintf(const Instruction* instr);

  // Processor state ---------------------------------------

  // Simulated monitors for exclusive access instructions.
  SimExclusiveLocalMonitor local_monitor_;
  SimExclusiveGlobalMonitor global_monitor_;

  // Output stream.
  FILE* stream_;
  PrintDisassembler* print_disasm_;

  // Instruction statistics instrumentation.
  Instrument* instrumentation_;

  // General purpose registers. Register 31 is the stack pointer.
  SimRegister registers_[kNumberOfRegisters];

  // Vector registers
  SimVRegister vregisters_[kNumberOfVRegisters];

  // Program Status Register.
  // bits[31, 27]: Condition flags N, Z, C, and V.
  //               (Negative, Zero, Carry, Overflow)
  SimSystemRegister nzcv_;

  // Floating-Point Control Register
  SimSystemRegister fpcr_;

  // Only a subset of FPCR features are supported by the simulator. This helper
  // checks that the FPCR settings are supported.
  //
  // This is checked when floating-point instructions are executed, not when
  // FPCR is set. This allows generated code to modify FPCR for external
  // functions, or to save and restore it when entering and leaving generated
  // code.
  void AssertSupportedFPCR() {
    VIXL_ASSERT(fpcr().FZ() == 0);             // No flush-to-zero support.
    VIXL_ASSERT(fpcr().RMode() == FPTieEven);  // Ties-to-even rounding only.

    // The simulator does not support half-precision operations so fpcr().AHP()
    // is irrelevant, and is not checked here.
  }

  static int CalcNFlag(uint64_t result, unsigned reg_size) {
    return (result >> (reg_size - 1)) & 1;
  }

  static int CalcZFlag(uint64_t result) {
    return (result == 0) ? 1 : 0;
  }

  static const uint32_t kConditionFlagsMask = 0xf0000000;

  // Stack
  byte* stack_;
  static const int stack_protection_size_ = 512 * KBytes;
  static const int stack_size_ = (2 * MBytes) + (2 * stack_protection_size_);
  byte* stack_limit_;

  Decoder* decoder_;
  // Indicates if the pc has been modified by the instruction and should not be
  // automatically incremented.
  bool pc_modified_;
  const Instruction* pc_;

  static const char* xreg_names[];
  static const char* wreg_names[];
  static const char* sreg_names[];
  static const char* dreg_names[];
  static const char* vreg_names[];

  static const Instruction* kEndOfSimAddress;

 private:
  template <typename T>
  static T FPDefaultNaN();

  // Standard NaN processing.
  template <typename T>
  T FPProcessNaN(T op) {
    VIXL_ASSERT(std::isnan(op));
    if (IsSignallingNaN(op)) {
      FPProcessException();
    }
    return DN() ? FPDefaultNaN<T>() : ToQuietNaN(op);
  }

  template <typename T>
  T FPProcessNaNs(T op1, T op2) {
    if (IsSignallingNaN(op1)) {
      return FPProcessNaN(op1);
    } else if (IsSignallingNaN(op2)) {
      return FPProcessNaN(op2);
    } else if (std::isnan(op1)) {
      VIXL_ASSERT(IsQuietNaN(op1));
      return FPProcessNaN(op1);
    } else if (std::isnan(op2)) {
      VIXL_ASSERT(IsQuietNaN(op2));
      return FPProcessNaN(op2);
    } else {
      return 0.0;
    }
  }

  template <typename T>
  T FPProcessNaNs3(T op1, T op2, T op3) {
    if (IsSignallingNaN(op1)) {
      return FPProcessNaN(op1);
    } else if (IsSignallingNaN(op2)) {
      return FPProcessNaN(op2);
    } else if (IsSignallingNaN(op3)) {
      return FPProcessNaN(op3);
    } else if (std::isnan(op1)) {
      VIXL_ASSERT(IsQuietNaN(op1));
      return FPProcessNaN(op1);
    } else if (std::isnan(op2)) {
      VIXL_ASSERT(IsQuietNaN(op2));
      return FPProcessNaN(op2);
    } else if (std::isnan(op3)) {
      VIXL_ASSERT(IsQuietNaN(op3));
      return FPProcessNaN(op3);
    } else {
      return 0.0;
    }
  }

  bool coloured_trace_;

  // A set of TraceParameters flags.
  int trace_parameters_;

  // Indicates whether the instruction instrumentation is active.
  bool instruction_stats_;

  // Indicates whether the exclusive-access warning has been printed.
  bool print_exclusive_access_warning_;
  void PrintExclusiveAccessWarning();

  // Indicates that the simulator ran out of memory at some point.
  // Data structures may not be fully allocated.
  bool oom_;

 public:
  // True if the simulator ran out of memory during or after construction.
  bool oom() const { return oom_; }

 protected:
  mozilla::Vector<int64_t, 0, js::SystemAllocPolicy> spStack_;
};

}  // namespace vixl

namespace js {
namespace jit {

class SimulatorProcess
{
 public:
  static SimulatorProcess* singleton_;

  SimulatorProcess()
    : lock_(mutexid::Arm64SimulatorLock)
    , redirection_(nullptr)
  {}

  // Synchronizes access between main thread and compilation threads.
  js::Mutex lock_ MOZ_UNANNOTATED;
  vixl::Redirection* redirection_;

#ifdef JS_CACHE_SIMULATOR_ARM64
  // For each simulator, record what other thread registered as instruction
  // being invalidated.
  struct ICacheFlush {
    void* start;
    size_t length;
  };
  using ICacheFlushes = mozilla::Vector<ICacheFlush, 2>;
  struct SimFlushes {
    vixl::Simulator* thread;
    ICacheFlushes records;
  };
  mozilla::Vector<SimFlushes, 1> pendingFlushes_;

  static void recordICacheFlush(void* start, size_t length);
  static void membarrier();
  static ICacheFlushes& getICacheFlushes(vixl::Simulator* sim);
  [[nodiscard]] static bool registerSimulator(vixl::Simulator* sim);
  static void unregisterSimulator(vixl::Simulator* sim);
#endif

  static void setRedirection(vixl::Redirection* redirection) {
    singleton_->lock_.assertOwnedByCurrentThread();
    singleton_->redirection_ = redirection;
  }

  static vixl::Redirection* redirection() {
    singleton_->lock_.assertOwnedByCurrentThread();
    return singleton_->redirection_;
  }

  static bool initialize() {
    singleton_ = js_new<SimulatorProcess>();
    return !!singleton_;
  }
  static void destroy() {
    js_delete(singleton_);
    singleton_ = nullptr;
  }
};

// Protects the icache and redirection properties of the simulator.
class AutoLockSimulatorCache : public js::LockGuard<js::Mutex>
{
  using Base = js::LockGuard<js::Mutex>;

 public:
  explicit AutoLockSimulatorCache()
    : Base(SimulatorProcess::singleton_->lock_)
  {
  }
};

} // namespace jit
} // namespace js

#endif  // JS_SIMULATOR_ARM64
#endif  // VIXL_A64_SIMULATOR_A64_H_
