// Copyright 2014, VIXL authors
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

#ifndef VIXL_CPU_AARCH64_H
#define VIXL_CPU_AARCH64_H

#include "jit/arm64/vixl/Cpu-Features-vixl.h"
#include "jit/arm64/vixl/Globals-vixl.h"

#include "jit/arm64/vixl/Instructions-vixl.h"

#ifndef VIXL_INCLUDE_TARGET_AARCH64
// The supporting .cc file is only compiled when the A64 target is selected.
// Throw an explicit error now to avoid a harder-to-debug linker error later.
//
// These helpers _could_ work on any AArch64 host, even when generating AArch32
// code, but we don't support this because the available features may differ
// between AArch32 and AArch64 on the same platform, so basing AArch32 code
// generation on aarch64::CPU features is probably broken.
#error cpu-aarch64.h requires VIXL_INCLUDE_TARGET_AARCH64 (scons target=a64).
#endif

namespace vixl {

// A CPU ID register, for use with CPUFeatures::kIDRegisterEmulation. Fields
// specific to each register are described in relevant subclasses.
class IDRegister {
 protected:
  explicit IDRegister(uint64_t value = 0) : value_(value) {}

  class Field {
   public:
    enum Type { kUnsigned, kSigned };

    explicit Field(int lsb, Type type = kUnsigned) : lsb_(lsb), type_(type) {}

    static const int kMaxWidthInBits = 4;

    int GetWidthInBits() const {
      // All current ID fields have four bits.
      return kMaxWidthInBits;
    }
    int GetLsb() const { return lsb_; }
    int GetMsb() const { return lsb_ + GetWidthInBits() - 1; }
    Type GetType() const { return type_; }

   private:
    int lsb_;
    Type type_;
  };

 public:
  // Extract the specified field, performing sign-extension for signed fields.
  // This allows us to implement the 'value >= number' detection mechanism
  // recommended by the Arm ARM, for both signed and unsigned fields.
  int Get(Field field) const;

 private:
  uint64_t value_;
};

class AA64PFR0 : public IDRegister {
 public:
  explicit AA64PFR0(uint64_t value) : IDRegister(value) {}

  CPUFeatures GetCPUFeatures() const;

 private:
  static const Field kFP;
  static const Field kAdvSIMD;
  static const Field kSVE;
  static const Field kDIT;
};

class AA64PFR1 : public IDRegister {
 public:
  explicit AA64PFR1(uint64_t value) : IDRegister(value) {}

  CPUFeatures GetCPUFeatures() const;

 private:
  static const Field kBT;
};

class AA64ISAR0 : public IDRegister {
 public:
  explicit AA64ISAR0(uint64_t value) : IDRegister(value) {}

  CPUFeatures GetCPUFeatures() const;

 private:
  static const Field kAES;
  static const Field kSHA1;
  static const Field kSHA2;
  static const Field kCRC32;
  static const Field kAtomic;
  static const Field kRDM;
  static const Field kSHA3;
  static const Field kSM3;
  static const Field kSM4;
  static const Field kDP;
  static const Field kFHM;
  static const Field kTS;
};

class AA64ISAR1 : public IDRegister {
 public:
  explicit AA64ISAR1(uint64_t value) : IDRegister(value) {}

  CPUFeatures GetCPUFeatures() const;

 private:
  static const Field kDPB;
  static const Field kAPA;
  static const Field kAPI;
  static const Field kJSCVT;
  static const Field kFCMA;
  static const Field kLRCPC;
  static const Field kGPA;
  static const Field kGPI;
  static const Field kFRINTTS;
  static const Field kSB;
  static const Field kSPECRES;
};

class AA64MMFR1 : public IDRegister {
 public:
  explicit AA64MMFR1(uint64_t value) : IDRegister(value) {}

  CPUFeatures GetCPUFeatures() const;

 private:
  static const Field kLO;
};

class CPU {
 public:
  // Initialise CPU support.
  static void SetUp();

  // Ensures the data at a given address and with a given size is the same for
  // the I and D caches. I and D caches are not automatically coherent on ARM
  // so this operation is required before any dynamically generated code can
  // safely run.
  static void EnsureIAndDCacheCoherency(void* address, size_t length);

  // Flush the local instruction pipeline, forcing a reload of any instructions
  // beyond this barrier from the icache.
  static void FlushExecutionContext();

  // Read and interpret the ID registers. This requires
  // CPUFeatures::kIDRegisterEmulation, and therefore cannot be called on
  // non-AArch64 platforms.
  static CPUFeatures InferCPUFeaturesFromIDRegisters();

  // Read and interpret CPUFeatures reported by the OS. Failed queries (or
  // unsupported platforms) return an empty list. Note that this is
  // indistinguishable from a successful query on a platform that advertises no
  // features.
  //
  // Non-AArch64 hosts are considered to be unsupported platforms, and this
  // function returns an empty list.
  static CPUFeatures InferCPUFeaturesFromOS(
      CPUFeatures::QueryIDRegistersOption option =
          CPUFeatures::kQueryIDRegistersIfAvailable);

  // Handle tagged pointers.
  template <typename T>
  static T SetPointerTag(T pointer, uint64_t tag) {
    VIXL_ASSERT(IsUintN(kAddressTagWidth, tag));

    // Use C-style casts to get static_cast behaviour for integral types (T),
    // and reinterpret_cast behaviour for other types.

    uint64_t raw = (uint64_t)pointer;
    VIXL_STATIC_ASSERT(sizeof(pointer) == sizeof(raw));

    raw = (raw & ~kAddressTagMask) | (tag << kAddressTagOffset);
    return (T)raw;
  }

  template <typename T>
  static uint64_t GetPointerTag(T pointer) {
    // Use C-style casts to get static_cast behaviour for integral types (T),
    // and reinterpret_cast behaviour for other types.

    uint64_t raw = (uint64_t)pointer;
    VIXL_STATIC_ASSERT(sizeof(pointer) == sizeof(raw));

    return (raw & kAddressTagMask) >> kAddressTagOffset;
  }

 private:
#define VIXL_AARCH64_ID_REG_LIST(V) \
  V(AA64PFR0)                       \
  V(AA64PFR1)                       \
  V(AA64ISAR0)                      \
  V(AA64ISAR1)                      \
  V(AA64MMFR1)

#define VIXL_READ_ID_REG(NAME) static NAME Read##NAME();
  // On native AArch64 platforms, read the named CPU ID registers. These require
  // CPUFeatures::kIDRegisterEmulation, and should not be called on non-AArch64
  // platforms.
  VIXL_AARCH64_ID_REG_LIST(VIXL_READ_ID_REG)
#undef VIXL_READ_ID_REG

  // Return the content of the cache type register.
  static uint32_t GetCacheType();

  // I and D cache line size in bytes.
  static unsigned icache_line_size_;
  static unsigned dcache_line_size_;
};

}  // namespace vixl

#endif  // VIXL_CPU_AARCH64_H
