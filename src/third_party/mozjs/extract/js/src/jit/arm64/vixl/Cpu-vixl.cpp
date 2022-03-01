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

#include "jit/arm64/vixl/Cpu-vixl.h"

#include "jstypes.h"

#if defined(__aarch64__) && (defined(__ANDROID__) || defined(__linux__))
#include <sys/auxv.h>
#define VIXL_USE_LINUX_HWCAP 1
#endif

#include "jit/arm64/vixl/Utils-vixl.h"


namespace vixl {


const IDRegister::Field AA64PFR0::kFP(16, Field::kSigned);
const IDRegister::Field AA64PFR0::kAdvSIMD(20, Field::kSigned);
const IDRegister::Field AA64PFR0::kSVE(32);
const IDRegister::Field AA64PFR0::kDIT(48);

const IDRegister::Field AA64PFR1::kBT(0);

const IDRegister::Field AA64ISAR0::kAES(4);
const IDRegister::Field AA64ISAR0::kSHA1(8);
const IDRegister::Field AA64ISAR0::kSHA2(12);
const IDRegister::Field AA64ISAR0::kCRC32(16);
const IDRegister::Field AA64ISAR0::kAtomic(20);
const IDRegister::Field AA64ISAR0::kRDM(28);
const IDRegister::Field AA64ISAR0::kSHA3(32);
const IDRegister::Field AA64ISAR0::kSM3(36);
const IDRegister::Field AA64ISAR0::kSM4(40);
const IDRegister::Field AA64ISAR0::kDP(44);
const IDRegister::Field AA64ISAR0::kFHM(48);
const IDRegister::Field AA64ISAR0::kTS(52);

const IDRegister::Field AA64ISAR1::kDPB(0);
const IDRegister::Field AA64ISAR1::kAPA(4);
const IDRegister::Field AA64ISAR1::kAPI(8);
const IDRegister::Field AA64ISAR1::kJSCVT(12);
const IDRegister::Field AA64ISAR1::kFCMA(16);
const IDRegister::Field AA64ISAR1::kLRCPC(20);
const IDRegister::Field AA64ISAR1::kGPA(24);
const IDRegister::Field AA64ISAR1::kGPI(28);
const IDRegister::Field AA64ISAR1::kFRINTTS(32);
const IDRegister::Field AA64ISAR1::kSB(36);
const IDRegister::Field AA64ISAR1::kSPECRES(40);

const IDRegister::Field AA64MMFR1::kLO(16);

CPUFeatures AA64PFR0::GetCPUFeatures() const {
  CPUFeatures f;
  if (Get(kFP) >= 0) f.Combine(CPUFeatures::kFP);
  if (Get(kFP) >= 1) f.Combine(CPUFeatures::kFPHalf);
  if (Get(kAdvSIMD) >= 0) f.Combine(CPUFeatures::kNEON);
  if (Get(kAdvSIMD) >= 1) f.Combine(CPUFeatures::kNEONHalf);
  if (Get(kSVE) >= 1) f.Combine(CPUFeatures::kSVE);
  if (Get(kDIT) >= 1) f.Combine(CPUFeatures::kDIT);
  return f;
}

CPUFeatures AA64PFR1::GetCPUFeatures() const {
  CPUFeatures f;
  if (Get(kBT) >= 1) f.Combine(CPUFeatures::kBTI);
  return f;
}

CPUFeatures AA64ISAR0::GetCPUFeatures() const {
  CPUFeatures f;
  if (Get(kAES) >= 1) f.Combine(CPUFeatures::kAES);
  if (Get(kAES) >= 2) f.Combine(CPUFeatures::kPmull1Q);
  if (Get(kSHA1) >= 1) f.Combine(CPUFeatures::kSHA1);
  if (Get(kSHA2) >= 1) f.Combine(CPUFeatures::kSHA2);
  if (Get(kSHA2) >= 2) f.Combine(CPUFeatures::kSHA512);
  if (Get(kCRC32) >= 1) f.Combine(CPUFeatures::kCRC32);
  if (Get(kAtomic) >= 1) f.Combine(CPUFeatures::kAtomics);
  if (Get(kRDM) >= 1) f.Combine(CPUFeatures::kRDM);
  if (Get(kSHA3) >= 1) f.Combine(CPUFeatures::kSHA3);
  if (Get(kSM3) >= 1) f.Combine(CPUFeatures::kSM3);
  if (Get(kSM4) >= 1) f.Combine(CPUFeatures::kSM4);
  if (Get(kDP) >= 1) f.Combine(CPUFeatures::kDotProduct);
  if (Get(kFHM) >= 1) f.Combine(CPUFeatures::kFHM);
  if (Get(kTS) >= 1) f.Combine(CPUFeatures::kFlagM);
  if (Get(kTS) >= 2) f.Combine(CPUFeatures::kAXFlag);
  return f;
}

CPUFeatures AA64ISAR1::GetCPUFeatures() const {
  CPUFeatures f;
  if (Get(kDPB) >= 1) f.Combine(CPUFeatures::kDCPoP);
  if (Get(kJSCVT) >= 1) f.Combine(CPUFeatures::kJSCVT);
  if (Get(kFCMA) >= 1) f.Combine(CPUFeatures::kFcma);
  if (Get(kLRCPC) >= 1) f.Combine(CPUFeatures::kRCpc);
  if (Get(kLRCPC) >= 2) f.Combine(CPUFeatures::kRCpcImm);
  if (Get(kFRINTTS) >= 1) f.Combine(CPUFeatures::kFrintToFixedSizedInt);

  if (Get(kAPI) >= 1) f.Combine(CPUFeatures::kPAuth);
  if (Get(kAPA) >= 1) f.Combine(CPUFeatures::kPAuth, CPUFeatures::kPAuthQARMA);
  if (Get(kGPI) >= 1) f.Combine(CPUFeatures::kPAuthGeneric);
  if (Get(kGPA) >= 1) {
    f.Combine(CPUFeatures::kPAuthGeneric, CPUFeatures::kPAuthGenericQARMA);
  }
  return f;
}

CPUFeatures AA64MMFR1::GetCPUFeatures() const {
  CPUFeatures f;
  if (Get(kLO) >= 1) f.Combine(CPUFeatures::kLORegions);
  return f;
}

int IDRegister::Get(IDRegister::Field field) const {
  int msb = field.GetMsb();
  int lsb = field.GetLsb();
  VIXL_STATIC_ASSERT(static_cast<size_t>(Field::kMaxWidthInBits) <
                     (sizeof(int) * kBitsPerByte));
  switch (field.GetType()) {
    case Field::kSigned:
      return static_cast<int>(ExtractSignedBitfield64(msb, lsb, value_));
    case Field::kUnsigned:
      return static_cast<int>(ExtractUnsignedBitfield64(msb, lsb, value_));
  }
  VIXL_UNREACHABLE();
  return 0;
}

CPUFeatures CPU::InferCPUFeaturesFromIDRegisters() {
  CPUFeatures f;
#define VIXL_COMBINE_ID_REG(NAME) f.Combine(Read##NAME().GetCPUFeatures());
  VIXL_AARCH64_ID_REG_LIST(VIXL_COMBINE_ID_REG)
#undef VIXL_COMBINE_ID_REG
  return f;
}

CPUFeatures CPU::InferCPUFeaturesFromOS(
    CPUFeatures::QueryIDRegistersOption option) {
  CPUFeatures features;

#if VIXL_USE_LINUX_HWCAP
  // Map each set bit onto a feature. Ideally, we'd use HWCAP_* macros rather
  // than explicit bits, but explicit bits allow us to identify features that
  // the toolchain doesn't know about.
  static const CPUFeatures::Feature kFeatureBits[] = {
      // Bits 0-7
      CPUFeatures::kFP,
      CPUFeatures::kNEON,
      CPUFeatures::kNone,  // "EVTSTRM", which VIXL doesn't track.
      CPUFeatures::kAES,
      CPUFeatures::kPmull1Q,
      CPUFeatures::kSHA1,
      CPUFeatures::kSHA2,
      CPUFeatures::kCRC32,
      // Bits 8-15
      CPUFeatures::kAtomics,
      CPUFeatures::kFPHalf,
      CPUFeatures::kNEONHalf,
      CPUFeatures::kIDRegisterEmulation,
      CPUFeatures::kRDM,
      CPUFeatures::kJSCVT,
      CPUFeatures::kFcma,
      CPUFeatures::kRCpc,
      // Bits 16-23
      CPUFeatures::kDCPoP,
      CPUFeatures::kSHA3,
      CPUFeatures::kSM3,
      CPUFeatures::kSM4,
      CPUFeatures::kDotProduct,
      CPUFeatures::kSHA512,
      CPUFeatures::kSVE,
      CPUFeatures::kFHM,
      // Bits 24-27
      CPUFeatures::kDIT,
      CPUFeatures::kUSCAT,
      CPUFeatures::kRCpcImm,
      CPUFeatures::kFlagM
      // Bits 28-31 are unassigned.
  };
  static const size_t kFeatureBitCount =
      sizeof(kFeatureBits) / sizeof(kFeatureBits[0]);

  // Mozilla change: Set the default for the simulator.
#ifdef JS_SIMULATOR_ARM64
  unsigned long auxv = ~(0UL);  // Enable all features for the Simulator.
#else
  unsigned long auxv = getauxval(AT_HWCAP);  // NOLINT(runtime/int)
#endif

  VIXL_STATIC_ASSERT(kFeatureBitCount < (sizeof(auxv) * kBitsPerByte));
  for (size_t i = 0; i < kFeatureBitCount; i++) {
    if (auxv & (1UL << i)) features.Combine(kFeatureBits[i]);
  }
#endif  // VIXL_USE_LINUX_HWCAP

  if ((option == CPUFeatures::kQueryIDRegistersIfAvailable) &&
      (features.Has(CPUFeatures::kIDRegisterEmulation))) {
    features.Combine(InferCPUFeaturesFromIDRegisters());
  }
  return features;
}


#ifdef __aarch64__
#define VIXL_READ_ID_REG(NAME)                         \
  NAME CPU::Read##NAME() {                             \
    uint64_t value = 0;                                \
    __asm__("mrs %0, ID_" #NAME "_EL1" : "=r"(value)); \
    return NAME(value);                                \
  }
#else  // __aarch64__
#define VIXL_READ_ID_REG(NAME)                                        \
  NAME CPU::Read##NAME() {                                            \
    /* TODO: Use VIXL_UNREACHABLE once it works in release builds. */ \
    VIXL_ABORT();                                                     \
  }
#endif  // __aarch64__

VIXL_AARCH64_ID_REG_LIST(VIXL_READ_ID_REG)

#undef VIXL_READ_ID_REG


// Initialise to smallest possible cache size.
unsigned CPU::dcache_line_size_ = 1;
unsigned CPU::icache_line_size_ = 1;


}  // namespace vixl
