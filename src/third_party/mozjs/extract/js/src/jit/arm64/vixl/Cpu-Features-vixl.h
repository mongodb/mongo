// Copyright 2018, VIXL authors
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

#ifndef VIXL_CPU_FEATURES_H
#define VIXL_CPU_FEATURES_H

#include <ostream>

#include "jit/arm64/vixl/Globals-vixl.h"


namespace vixl {


// clang-format off
#define VIXL_CPU_FEATURE_LIST(V)                                               \
  /* If set, the OS traps and emulates MRS accesses to relevant (EL1) ID_*  */ \
  /* registers, so that the detailed feature registers can be read          */ \
  /* directly.                                                              */ \
  V(kIDRegisterEmulation, "ID register emulation",  "cpuid")                   \
                                                                               \
  V(kFP,                  "FP",                     "fp")                      \
  V(kNEON,                "NEON",                   "asimd")                   \
  V(kCRC32,               "CRC32",                  "crc32")                   \
  /* Cryptographic support instructions.                                    */ \
  V(kAES,                 "AES",                    "aes")                     \
  V(kSHA1,                "SHA1",                   "sha1")                    \
  V(kSHA2,                "SHA2",                   "sha2")                    \
  /* A form of PMULL{2} with a 128-bit (1Q) result.                         */ \
  V(kPmull1Q,             "Pmull1Q",                "pmull")                   \
  /* Atomic operations on memory: CAS, LDADD, STADD, SWP, etc.              */ \
  V(kAtomics,             "Atomics",                "atomics")                 \
  /* Limited ordering regions: LDLAR, STLLR and their variants.             */ \
  V(kLORegions,           "LORegions",              NULL)                      \
  /* Rounding doubling multiply add/subtract: SQRDMLAH and SQRDMLSH.        */ \
  V(kRDM,                 "RDM",                    "asimdrdm")                \
  /* Scalable Vector Extension.                                             */ \
  V(kSVE,                 "SVE",                    "sve")                     \
  /* SDOT and UDOT support (in NEON).                                       */ \
  V(kDotProduct,          "DotProduct",             "asimddp")                 \
  /* Half-precision (FP16) support for FP and NEON, respectively.           */ \
  V(kFPHalf,              "FPHalf",                 "fphp")                    \
  V(kNEONHalf,            "NEONHalf",               "asimdhp")                 \
  /* The RAS extension, including the ESB instruction.                      */ \
  V(kRAS,                 "RAS",                    NULL)                      \
  /* Data cache clean to the point of persistence: DC CVAP.                 */ \
  V(kDCPoP,               "DCPoP",                  "dcpop")                   \
  /* Data cache clean to the point of deep persistence: DC CVADP.           */ \
  V(kDCCVADP,             "DCCVADP",                NULL)                      \
  /* Cryptographic support instructions.                                    */ \
  V(kSHA3,                "SHA3",                   "sha3")                    \
  V(kSHA512,              "SHA512",                 "sha512")                  \
  V(kSM3,                 "SM3",                    "sm3")                     \
  V(kSM4,                 "SM4",                    "sm4")                     \
  /* Pointer authentication for addresses.                                  */ \
  V(kPAuth,               "PAuth",                  NULL)                      \
  /* Pointer authentication for addresses uses QARMA.                       */ \
  V(kPAuthQARMA,          "PAuthQARMA",             NULL)                      \
  /* Generic authentication (using the PACGA instruction).                  */ \
  V(kPAuthGeneric,        "PAuthGeneric",           NULL)                      \
  /* Generic authentication uses QARMA.                                     */ \
  V(kPAuthGenericQARMA,   "PAuthGenericQARMA",      NULL)                      \
  /* JavaScript-style FP -> integer conversion instruction: FJCVTZS.        */ \
  V(kJSCVT,               "JSCVT",                  "jscvt")                   \
  /* Complex number support for NEON: FCMLA and FCADD.                      */ \
  V(kFcma,                "Fcma",                   "fcma")                    \
  /* RCpc-based model (for weaker release consistency): LDAPR and variants. */ \
  V(kRCpc,                "RCpc",                   "lrcpc")                   \
  V(kRCpcImm,             "RCpc (imm)",             "ilrcpc")                  \
  /* Flag manipulation instructions: SETF{8,16}, CFINV, RMIF.               */ \
  V(kFlagM,               "FlagM",                  "flagm")                   \
  /* Unaligned single-copy atomicity.                                       */ \
  V(kUSCAT,               "USCAT",                  "uscat")                   \
  /* FP16 fused multiply-add or -subtract long: FMLAL{2}, FMLSL{2}.         */ \
  V(kFHM,                 "FHM",                    "asimdfhm")                \
  /* Data-independent timing (for selected instructions).                   */ \
  V(kDIT,                 "DIT",                    "dit")                     \
  /* Branch target identification.                                          */ \
  V(kBTI,                 "BTI",                    NULL)                      \
  /* Flag manipulation instructions: {AX,XA}FLAG                            */ \
  V(kAXFlag,              "AXFlag",                 NULL)                      \
  /* Random number generation extension,                                    */ \
  V(kRNG,                 "RNG",                    NULL)                      \
  /* Floating-point round to {32,64}-bit integer.                           */ \
  V(kFrintToFixedSizedInt,"Frint (bounded)",        NULL)
// clang-format on


class CPUFeaturesConstIterator;

// A representation of the set of features known to be supported by the target
// device. Each feature is represented by a simple boolean flag.
//
//   - When the Assembler is asked to assemble an instruction, it asserts (in
//     debug mode) that the necessary features are available.
//
//   - TODO: The MacroAssembler relies on the Assembler's assertions, but in
//     some cases it may be useful for macros to generate a fall-back sequence
//     in case features are not available.
//
//   - The Simulator assumes by default that all features are available, but it
//     is possible to configure it to fail if the simulated code uses features
//     that are not enabled.
//
//     The Simulator also offers pseudo-instructions to allow features to be
//     enabled and disabled dynamically. This is useful when you want to ensure
//     that some features are constrained to certain areas of code.
//
//   - The base Disassembler knows nothing about CPU features, but the
//     PrintDisassembler can be configured to annotate its output with warnings
//     about unavailable features. The Simulator uses this feature when
//     instruction trace is enabled.
//
//   - The Decoder-based components -- the Simulator and PrintDisassembler --
//     rely on a CPUFeaturesAuditor visitor. This visitor keeps a list of
//     features actually encountered so that a large block of code can be
//     examined (either directly or through simulation), and the required
//     features analysed later.
//
// Expected usage:
//
//     // By default, VIXL uses CPUFeatures::AArch64LegacyBaseline(), for
//     // compatibility with older version of VIXL.
//     MacroAssembler masm;
//
//     // Generate code only for the current CPU.
//     masm.SetCPUFeatures(CPUFeatures::InferFromOS());
//
//     // Turn off feature checking entirely.
//     masm.SetCPUFeatures(CPUFeatures::All());
//
// Feature set manipulation:
//
//     CPUFeatures f;  // The default constructor gives an empty set.
//     // Individual features can be added (or removed).
//     f.Combine(CPUFeatures::kFP, CPUFeatures::kNEON, CPUFeatures::AES);
//     f.Remove(CPUFeatures::kNEON);
//
//     // Some helpers exist for extensions that provide several features.
//     f.Remove(CPUFeatures::All());
//     f.Combine(CPUFeatures::AArch64LegacyBaseline());
//
//     // Chained construction is also possible.
//     CPUFeatures g =
//         f.With(CPUFeatures::kPmull1Q).Without(CPUFeatures::kCRC32);
//
//     // Features can be queried. Where multiple features are given, they are
//     // combined with logical AND.
//     if (h.Has(CPUFeatures::kNEON)) { ... }
//     if (h.Has(CPUFeatures::kFP, CPUFeatures::kNEON)) { ... }
//     if (h.Has(g)) { ... }
//     // If the empty set is requested, the result is always 'true'.
//     VIXL_ASSERT(h.Has(CPUFeatures()));
//
//     // For debug and reporting purposes, features can be enumerated (or
//     // printed directly):
//     std::cout << CPUFeatures::kNEON;  // Prints something like "NEON".
//     std::cout << f;  // Prints something like "FP, NEON, CRC32".
class CPUFeatures {
 public:
  // clang-format off
  // Individual features.
  // These should be treated as opaque tokens. User code should not rely on
  // specific numeric values or ordering.
  enum Feature {
    // Refer to VIXL_CPU_FEATURE_LIST (above) for the list of feature names that
    // this class supports.

    kNone = -1,
#define VIXL_DECLARE_FEATURE(SYMBOL, NAME, CPUINFO) SYMBOL,
    VIXL_CPU_FEATURE_LIST(VIXL_DECLARE_FEATURE)
#undef VIXL_DECLARE_FEATURE
    kNumberOfFeatures
  };
  // clang-format on

  // By default, construct with no features enabled.
  CPUFeatures() : features_(0) {}

  // Construct with some features already enabled.
  CPUFeatures(Feature feature0,
              Feature feature1 = kNone,
              Feature feature2 = kNone,
              Feature feature3 = kNone);

  // Construct with all features enabled. This can be used to disable feature
  // checking: `Has(...)` returns true regardless of the argument.
  static CPUFeatures All();

  // Construct an empty CPUFeatures. This is equivalent to the default
  // constructor, but is provided for symmetry and convenience.
  static CPUFeatures None() { return CPUFeatures(); }

  // The presence of these features was assumed by version of VIXL before this
  // API was added, so using this set by default ensures API compatibility.
  static CPUFeatures AArch64LegacyBaseline() {
    return CPUFeatures(kFP, kNEON, kCRC32);
  }

  // Construct a new CPUFeatures object using ID registers. This assumes that
  // kIDRegisterEmulation is present.
  static CPUFeatures InferFromIDRegisters();

  enum QueryIDRegistersOption {
    kDontQueryIDRegisters,
    kQueryIDRegistersIfAvailable
  };

  // Construct a new CPUFeatures object based on what the OS reports.
  static CPUFeatures InferFromOS(
      QueryIDRegistersOption option = kQueryIDRegistersIfAvailable);

  // Combine another CPUFeatures object into this one. Features that already
  // exist in this set are left unchanged.
  void Combine(const CPUFeatures& other);

  // Combine specific features into this set. Features that already exist in
  // this set are left unchanged.
  void Combine(Feature feature0,
               Feature feature1 = kNone,
               Feature feature2 = kNone,
               Feature feature3 = kNone);

  // Remove features in another CPUFeatures object from this one.
  void Remove(const CPUFeatures& other);

  // Remove specific features from this set.
  void Remove(Feature feature0,
              Feature feature1 = kNone,
              Feature feature2 = kNone,
              Feature feature3 = kNone);

  // Chaining helpers for convenient construction.
  CPUFeatures With(const CPUFeatures& other) const;
  CPUFeatures With(Feature feature0,
                   Feature feature1 = kNone,
                   Feature feature2 = kNone,
                   Feature feature3 = kNone) const;
  CPUFeatures Without(const CPUFeatures& other) const;
  CPUFeatures Without(Feature feature0,
                      Feature feature1 = kNone,
                      Feature feature2 = kNone,
                      Feature feature3 = kNone) const;

  // Query features.
  // Note that an empty query (like `Has(kNone)`) always returns true.
  bool Has(const CPUFeatures& other) const;
  bool Has(Feature feature0,
           Feature feature1 = kNone,
           Feature feature2 = kNone,
           Feature feature3 = kNone) const;

  // Return the number of enabled features.
  size_t Count() const;
  bool HasNoFeatures() const { return Count() == 0; }

  // Check for equivalence.
  bool operator==(const CPUFeatures& other) const {
    return Has(other) && other.Has(*this);
  }
  bool operator!=(const CPUFeatures& other) const { return !(*this == other); }

  typedef CPUFeaturesConstIterator const_iterator;

  const_iterator begin() const;
  const_iterator end() const;

 private:
  // Each bit represents a feature. This field will be replaced as needed if
  // features are added.
  uint64_t features_;

  friend std::ostream& operator<<(std::ostream& os,
                                  const vixl::CPUFeatures& features);
};

std::ostream& operator<<(std::ostream& os, vixl::CPUFeatures::Feature feature);
std::ostream& operator<<(std::ostream& os, const vixl::CPUFeatures& features);

// This is not a proper C++ iterator type, but it simulates enough of
// ForwardIterator that simple loops can be written.
class CPUFeaturesConstIterator {
 public:
  CPUFeaturesConstIterator(const CPUFeatures* cpu_features = NULL,
                           CPUFeatures::Feature start = CPUFeatures::kNone)
      : cpu_features_(cpu_features), feature_(start) {
    VIXL_ASSERT(IsValid());
  }

  bool operator==(const CPUFeaturesConstIterator& other) const;
  bool operator!=(const CPUFeaturesConstIterator& other) const {
    return !(*this == other);
  }
  CPUFeatures::Feature operator++();
  CPUFeatures::Feature operator++(int);

  CPUFeatures::Feature operator*() const {
    VIXL_ASSERT(IsValid());
    return feature_;
  }

  // For proper support of C++'s simplest "Iterator" concept, this class would
  // have to define member types (such as CPUFeaturesIterator::pointer) to make
  // it appear as if it iterates over Feature objects in memory. That is, we'd
  // need CPUFeatures::iterator to behave like std::vector<Feature>::iterator.
  // This is at least partially possible -- the std::vector<bool> specialisation
  // does something similar -- but it doesn't seem worthwhile for a
  // special-purpose debug helper, so they are omitted here.
 private:
  const CPUFeatures* cpu_features_;
  CPUFeatures::Feature feature_;

  bool IsValid() const {
    return ((cpu_features_ == NULL) && (feature_ == CPUFeatures::kNone)) ||
           cpu_features_->Has(feature_);
  }
};

// A convenience scope for temporarily modifying a CPU features object. This
// allows features to be enabled for short sequences.
//
// Expected usage:
//
//  {
//    CPUFeaturesScope cpu(&masm, CPUFeatures::kCRC32);
//    // This scope can now use CRC32, as well as anything else that was enabled
//    // before the scope.
//
//    ...
//
//    // At the end of the scope, the original CPU features are restored.
//  }
class CPUFeaturesScope {
 public:
  // Start a CPUFeaturesScope on any object that implements
  // `CPUFeatures* GetCPUFeatures()`.
  template <typename T>
  explicit CPUFeaturesScope(T* cpu_features_wrapper,
                            CPUFeatures::Feature feature0 = CPUFeatures::kNone,
                            CPUFeatures::Feature feature1 = CPUFeatures::kNone,
                            CPUFeatures::Feature feature2 = CPUFeatures::kNone,
                            CPUFeatures::Feature feature3 = CPUFeatures::kNone)
      : cpu_features_(cpu_features_wrapper->GetCPUFeatures()),
        old_features_(*cpu_features_) {
    cpu_features_->Combine(feature0, feature1, feature2, feature3);
  }

  template <typename T>
  CPUFeaturesScope(T* cpu_features_wrapper, const CPUFeatures& other)
      : cpu_features_(cpu_features_wrapper->GetCPUFeatures()),
        old_features_(*cpu_features_) {
    cpu_features_->Combine(other);
  }

  ~CPUFeaturesScope() { *cpu_features_ = old_features_; }

  // For advanced usage, the CPUFeatures object can be accessed directly.
  // The scope will restore the original state when it ends.

  CPUFeatures* GetCPUFeatures() const { return cpu_features_; }

  void SetCPUFeatures(const CPUFeatures& cpu_features) {
    *cpu_features_ = cpu_features;
  }

 private:
  CPUFeatures* const cpu_features_;
  const CPUFeatures old_features_;
};


}  // namespace vixl

#endif  // VIXL_CPU_FEATURES_H
