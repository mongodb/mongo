/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MacroAssembler_h
#define jit_MacroAssembler_h

#include "mozilla/EndianUtils.h"
#include "mozilla/MacroForEach.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/Variant.h"

#if defined(JS_CODEGEN_X86)
#  include "jit/x86/MacroAssembler-x86.h"
#elif defined(JS_CODEGEN_X64)
#  include "jit/x64/MacroAssembler-x64.h"
#elif defined(JS_CODEGEN_ARM)
#  include "jit/arm/MacroAssembler-arm.h"
#elif defined(JS_CODEGEN_ARM64)
#  include "jit/arm64/MacroAssembler-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
#  include "jit/mips32/MacroAssembler-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
#  include "jit/mips64/MacroAssembler-mips64.h"
#elif defined(JS_CODEGEN_LOONG64)
#  include "jit/loong64/MacroAssembler-loong64.h"
#elif defined(JS_CODEGEN_RISCV64)
#  include "jit/riscv64/MacroAssembler-riscv64.h"
#elif defined(JS_CODEGEN_WASM32)
#  include "jit/wasm32/MacroAssembler-wasm32.h"
#elif defined(JS_CODEGEN_NONE)
#  include "jit/none/MacroAssembler-none.h"
#else
#  error "Unknown architecture!"
#endif
#include "jit/ABIArgGenerator.h"
#include "jit/ABIFunctions.h"
#include "jit/AtomicOp.h"
#include "jit/IonTypes.h"
#include "jit/MoveResolver.h"
#include "jit/VMFunctions.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "util/Memory.h"
#include "vm/FunctionFlags.h"
#include "vm/Opcodes.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmFrame.h"

// [SMDOC] MacroAssembler multi-platform overview
//
// * How to read/write MacroAssembler method declarations:
//
// The following macros are made to avoid #ifdef around each method declarations
// of the Macro Assembler, and they are also used as an hint on the location of
// the implementations of each method.  For example, the following declaration
//
//   void Pop(FloatRegister t) DEFINED_ON(x86_shared, arm);
//
// suggests the MacroAssembler::Pop(FloatRegister) method is implemented in
// x86-shared/MacroAssembler-x86-shared.h, and also in arm/MacroAssembler-arm.h.
//
// - If there is no annotation, then there is only one generic definition in
//   MacroAssembler.cpp.
//
// - If the declaration is "inline", then the method definition(s) would be in
//   the "-inl.h" variant of the same file(s).
//
// The script check_macroassembler_style.py (which runs on every build) is
// used to verify that method definitions match the annotation on the method
// declarations.  If there is any difference, then you either forgot to define
// the method in one of the macro assembler, or you forgot to update the
// annotation of the macro assembler declaration.
//
// Some convenient short-cuts are used to avoid repeating the same list of
// architectures on each method declaration, such as PER_ARCH and
// PER_SHARED_ARCH.
//
// Functions that are architecture-agnostic and are the same for all
// architectures, that it's necessary to define inline *in this header* to
// avoid used-before-defined warnings/errors that would occur if the
// definitions were in MacroAssembler-inl.h, should use the OOL_IN_HEADER
// marker at end of the declaration:
//
//   inline uint32_t framePushed() const OOL_IN_HEADER;
//
// Such functions should then be defined immediately after MacroAssembler's
// definition, for example:
//
//   //{{{ check_macroassembler_style
//   inline uint32_t
//   MacroAssembler::framePushed() const
//   {
//       return framePushed_;
//   }
//   ////}}} check_macroassembler_style

#define ALL_ARCH mips32, mips64, arm, arm64, x86, x64, loong64, riscv64, wasm32
#define ALL_SHARED_ARCH \
  arm, arm64, loong64, riscv64, x86_shared, mips_shared, wasm32

// * How this macro works:
//
// DEFINED_ON is a macro which check if, for the current architecture, the
// method is defined on the macro assembler or not.
//
// For each architecture, we have a macro named DEFINED_ON_arch.  This macro is
// empty if this is not the current architecture.  Otherwise it must be either
// set to "define" or "crash" (only used for the none target so far).
//
// The DEFINED_ON macro maps the list of architecture names given as arguments
// to a list of macro names.  For example,
//
//   DEFINED_ON(arm, x86_shared)
//
// is expanded to
//
//   DEFINED_ON_none DEFINED_ON_arm DEFINED_ON_x86_shared
//
// which are later expanded on ARM, x86, x64 by DEFINED_ON_EXPAND_ARCH_RESULTS
// to
//
//   define
//
// or if the JIT is disabled or set to no architecture to
//
//   crash
//
// or to nothing, if the current architecture is not listed in the list of
// arguments of DEFINED_ON.  Note, only one of the DEFINED_ON_arch macro
// contributes to the non-empty result, which is the macro of the current
// architecture if it is listed in the arguments of DEFINED_ON.
//
// This result is appended to DEFINED_ON_RESULT_ before expanding the macro,
// which results in either no annotation, a MOZ_CRASH(), or a "= delete"
// annotation on the method declaration.

#define DEFINED_ON_x86
#define DEFINED_ON_x64
#define DEFINED_ON_x86_shared
#define DEFINED_ON_arm
#define DEFINED_ON_arm64
#define DEFINED_ON_mips32
#define DEFINED_ON_mips64
#define DEFINED_ON_mips_shared
#define DEFINED_ON_loong64
#define DEFINED_ON_riscv64
#define DEFINED_ON_wasm32
#define DEFINED_ON_none

// Specialize for each architecture.
#if defined(JS_CODEGEN_X86)
#  undef DEFINED_ON_x86
#  define DEFINED_ON_x86 define
#  undef DEFINED_ON_x86_shared
#  define DEFINED_ON_x86_shared define
#elif defined(JS_CODEGEN_X64)
#  undef DEFINED_ON_x64
#  define DEFINED_ON_x64 define
#  undef DEFINED_ON_x86_shared
#  define DEFINED_ON_x86_shared define
#elif defined(JS_CODEGEN_ARM)
#  undef DEFINED_ON_arm
#  define DEFINED_ON_arm define
#elif defined(JS_CODEGEN_ARM64)
#  undef DEFINED_ON_arm64
#  define DEFINED_ON_arm64 define
#elif defined(JS_CODEGEN_MIPS32)
#  undef DEFINED_ON_mips32
#  define DEFINED_ON_mips32 define
#  undef DEFINED_ON_mips_shared
#  define DEFINED_ON_mips_shared define
#elif defined(JS_CODEGEN_MIPS64)
#  undef DEFINED_ON_mips64
#  define DEFINED_ON_mips64 define
#  undef DEFINED_ON_mips_shared
#  define DEFINED_ON_mips_shared define
#elif defined(JS_CODEGEN_LOONG64)
#  undef DEFINED_ON_loong64
#  define DEFINED_ON_loong64 define
#elif defined(JS_CODEGEN_RISCV64)
#  undef DEFINED_ON_riscv64
#  define DEFINED_ON_riscv64 define
#elif defined(JS_CODEGEN_WASM32)
#  undef DEFINED_ON_wasm32
#  define DEFINED_ON_wasm32 define
#elif defined(JS_CODEGEN_NONE)
#  undef DEFINED_ON_none
#  define DEFINED_ON_none crash
#else
#  error "Unknown architecture!"
#endif

#define DEFINED_ON_RESULT_crash \
  { MOZ_CRASH(); }
#define DEFINED_ON_RESULT_define
#define DEFINED_ON_RESULT_ = delete

#define DEFINED_ON_DISPATCH_RESULT_2(Macro, Result) Macro##Result
#define DEFINED_ON_DISPATCH_RESULT(...) \
  DEFINED_ON_DISPATCH_RESULT_2(DEFINED_ON_RESULT_, __VA_ARGS__)

// We need to let the evaluation of MOZ_FOR_EACH terminates.
#define DEFINED_ON_EXPAND_ARCH_RESULTS_3(ParenResult) \
  DEFINED_ON_DISPATCH_RESULT ParenResult
#define DEFINED_ON_EXPAND_ARCH_RESULTS_2(ParenResult) \
  DEFINED_ON_EXPAND_ARCH_RESULTS_3(ParenResult)
#define DEFINED_ON_EXPAND_ARCH_RESULTS(ParenResult) \
  DEFINED_ON_EXPAND_ARCH_RESULTS_2(ParenResult)

#define DEFINED_ON_FWDARCH(Arch) DEFINED_ON_##Arch
#define DEFINED_ON_MAP_ON_ARCHS(ArchList) \
  DEFINED_ON_EXPAND_ARCH_RESULTS(         \
      (MOZ_FOR_EACH(DEFINED_ON_FWDARCH, (), ArchList)))

#define DEFINED_ON(...) DEFINED_ON_MAP_ON_ARCHS((none, __VA_ARGS__))

#define PER_ARCH DEFINED_ON(ALL_ARCH)
#define PER_SHARED_ARCH DEFINED_ON(ALL_SHARED_ARCH)
#define OOL_IN_HEADER

namespace JS {
struct ExpandoAndGeneration;
}

namespace js {

class StaticStrings;
class TypedArrayObject;

enum class NativeIteratorIndices : uint32_t;

namespace wasm {
class CalleeDesc;
class CallSiteDesc;
class BytecodeOffset;
class MemoryAccessDesc;

struct ModuleEnvironment;

enum class FailureMode : uint8_t;
enum class SimdOp;
enum class SymbolicAddress;
enum class Trap;
}  // namespace wasm

namespace jit {

// Defined in JitFrames.h
enum class ExitFrameType : uint8_t;

class AutoSaveLiveRegisters;
class CompileZone;
class TemplateNativeObject;
class TemplateObject;

enum class CheckUnsafeCallWithABI {
  // Require the callee to use AutoUnsafeCallWithABI.
  Check,

  // We pushed an exit frame so this callWithABI can safely GC and walk the
  // stack.
  DontCheckHasExitFrame,

  // Don't check this callWithABI uses AutoUnsafeCallWithABI, for instance
  // because we're calling a simple helper function (like malloc or js_free)
  // that we can't change and/or that we know won't GC.
  DontCheckOther,
};

// This is a global function made to create the DynFn type in a controlled
// environment which would check if the function signature has been registered
// as an ABI function signature.
template <typename Sig>
static inline DynFn DynamicFunction(Sig fun);

enum class CharEncoding { Latin1, TwoByte };

constexpr uint32_t WasmCallerInstanceOffsetBeforeCall =
    wasm::FrameWithInstances::callerInstanceOffsetWithoutFrame();
constexpr uint32_t WasmCalleeInstanceOffsetBeforeCall =
    wasm::FrameWithInstances::calleeInstanceOffsetWithoutFrame();

// Allocation sites may be passed to GC thing allocation methods either via a
// register (for baseline compilation) or an enum indicating one of the
// catch-all allocation sites (for optimized compilation).
struct AllocSiteInput
    : public mozilla::Variant<Register, gc::CatchAllAllocSite> {
  using Base = mozilla::Variant<Register, gc::CatchAllAllocSite>;
  AllocSiteInput() : Base(gc::CatchAllAllocSite::Unknown) {}
  explicit AllocSiteInput(gc::CatchAllAllocSite catchAll) : Base(catchAll) {}
  explicit AllocSiteInput(Register reg) : Base(reg) {}
};

// [SMDOC] Code generation invariants (incomplete)
//
// ## 64-bit GPRs carrying 32-bit values
//
// At least at the end of every JS or Wasm operation (= SpiderMonkey bytecode or
// Wasm bytecode; this is necessarily a little vague), if a 64-bit GPR has a
// 32-bit value, then the upper 32 bits of the register may be predictable in
// accordance with platform-specific rules, as follows.
//
// - On x64 and arm64, the upper bits are zero
// - On mips64 and loongarch64 the upper bits are the sign extension of the
//   lower bits
// - (On risc-v we have no rule, having no port yet.  Sign extension is the most
//   likely rule, but "unpredictable" is an option.)
//
// In most cases no extra work needs to be done to maintain the invariant:
//
// - 32-bit operations on x64 and arm64 zero-extend the result to 64 bits.
//   These operations ignore the upper bits of the inputs.
// - 32-bit operations on mips64 sign-extend the result to 64 bits (even many
//   that are labeled as "unsigned", eg ADDU, though not all, eg LU).
//   Additionally, the inputs to many 32-bit operations must be properly
//   sign-extended to avoid "unpredictable" behavior, and our simulators check
//   that inputs conform.
// - (32-bit operations on risc-v and loongarch64 sign-extend, much as mips, but
//   appear to ignore the upper bits of the inputs.)
//
// The upshot of these invariants is, among other things, that:
//
// - No code needs to be generated when a 32-bit value is extended to 64 bits
//   or a 64-bit value is wrapped to 32 bits, if the upper bits are known to be
//   correct because they resulted from an operation that produced them
//   predictably.
// - Literal loads must be careful to avoid instructions that might extend the
//   literal in the wrong way.
// - Code that produces values using intermediate values with non-canonical
//   extensions must extend according to platform conventions before being
//   "done".
//
// All optimizations are necessarily platform-specific and should only be used
// in platform-specific code.  We may add architectures in the future that do
// not follow the patterns of the few architectures we already have.
//
// Also see MacroAssembler::debugAssertCanonicalInt32().

// The public entrypoint for emitting assembly. Note that a MacroAssembler can
// use cx->lifoAlloc, so take care not to interleave masm use with other
// lifoAlloc use if one will be destroyed before the other.
class MacroAssembler : public MacroAssemblerSpecific {
 private:
  // Information about the current JSRuntime. This is nullptr only for Wasm
  // compilations.
  CompileRuntime* maybeRuntime_ = nullptr;

  // Information about the current Realm. This is nullptr for Wasm compilations
  // and when compiling JitRuntime trampolines.
  CompileRealm* maybeRealm_ = nullptr;

  // Labels for handling exceptions and failures.
  NonAssertingLabel failureLabel_;

 protected:
  // Constructor is protected. Use one of the derived classes!
  explicit MacroAssembler(TempAllocator& alloc,
                          CompileRuntime* maybeRuntime = nullptr,
                          CompileRealm* maybeRealm = nullptr);

 public:
  MoveResolver& moveResolver() {
    // As an optimization, the MoveResolver is a persistent data structure
    // shared between visitors in the CodeGenerator. This assertion
    // checks that state is not leaking from visitor to visitor
    // via an unresolved addMove().
    MOZ_ASSERT(moveResolver_.hasNoPendingMoves());
    return moveResolver_;
  }

  size_t instructionsSize() const { return size(); }

  CompileRealm* realm() const {
    MOZ_ASSERT(maybeRealm_);
    return maybeRealm_;
  }
  CompileRuntime* runtime() const {
    MOZ_ASSERT(maybeRuntime_);
    return maybeRuntime_;
  }

#ifdef JS_HAS_HIDDEN_SP
  void Push(RegisterOrSP reg);
#endif

#ifdef ENABLE_WASM_SIMD
  // `op` should be a shift operation. Return true if a variable-width shift
  // operation on this architecture should pre-mask the shift count, and if so,
  // return the mask in `*mask`.
  static bool MustMaskShiftCountSimd128(wasm::SimdOp op, int32_t* mask);
#endif

 private:
  // The value returned by GetMaxOffsetGuardLimit() in WasmTypes.h
  uint32_t wasmMaxOffsetGuardLimit_;

 public:
  uint32_t wasmMaxOffsetGuardLimit() const { return wasmMaxOffsetGuardLimit_; }
  void setWasmMaxOffsetGuardLimit(uint32_t limit) {
    wasmMaxOffsetGuardLimit_ = limit;
  }

  //{{{ check_macroassembler_decl_style
 public:
  // ===============================================================
  // MacroAssembler high-level usage.

  // Flushes the assembly buffer, on platforms that need it.
  void flush() PER_SHARED_ARCH;

  // Add a comment that is visible in the pretty printed assembly code.
  void comment(const char* msg) PER_SHARED_ARCH;

  // ===============================================================
  // Frame manipulation functions.

  inline uint32_t framePushed() const OOL_IN_HEADER;
  inline void setFramePushed(uint32_t framePushed) OOL_IN_HEADER;
  inline void adjustFrame(int32_t value) OOL_IN_HEADER;

  // Adjust the frame, to account for implicit modification of the stack
  // pointer, such that callee can remove arguments on the behalf of the
  // caller.
  inline void implicitPop(uint32_t bytes) OOL_IN_HEADER;

 private:
  // This field is used to statically (at compilation time) emulate a frame
  // pointer by keeping track of stack manipulations.
  //
  // It is maintained by all stack manipulation functions below.
  uint32_t framePushed_;

 public:
  // ===============================================================
  // Stack manipulation functions -- sets of registers.

  // Approximately speaking, the following routines must use the same memory
  // layout.  Any inconsistencies will certainly lead to crashing in generated
  // code:
  //
  //   MacroAssembler::PushRegsInMaskSizeInBytes
  //   MacroAssembler::PushRegsInMask
  //   MacroAssembler::storeRegsInMask
  //   MacroAssembler::PopRegsInMask
  //   MacroAssembler::PopRegsInMaskIgnore
  //   FloatRegister::getRegisterDumpOffsetInBytes
  //   (no class) PushRegisterDump
  //   (union) RegisterContent
  //   JitRuntime::generateInvalidator
  //   JitRuntime::generateBailoutHandler
  //   JSJitFrameIter::machineState
  //
  // To be more exact, the invariants are:
  //
  // * The save area is conceptually viewed as starting at a highest address
  //   (really, at "highest address - 1") and working down to some lower
  //   address.
  //
  // * PushRegsInMask, storeRegsInMask and PopRegsInMask{Ignore} must use
  //   exactly the same memory layout, when starting from the abovementioned
  //   highest address.
  //
  // * PushRegsInMaskSizeInBytes must produce a value which is exactly equal
  //   to the change in the machine's stack pointer register as a result of
  //   calling PushRegsInMask or PopRegsInMask{Ignore}.  This value must be at
  //   least uintptr_t-aligned on the target, and may be more aligned than that.
  //
  // * PushRegsInMaskSizeInBytes must produce a value which is greater than or
  //   equal to the amount of space used by storeRegsInMask.
  //
  // * Hence, regardless of whether the save area is created with
  //   storeRegsInMask or PushRegsInMask, it is guaranteed to fit inside an
  //   area of size calculated by PushRegsInMaskSizeInBytes.
  //
  // * For the `ignore` argument of PopRegsInMaskIgnore, equality checking
  //   for the floating point/SIMD registers is done on the basis of the
  //   underlying physical register, regardless of width.  For example, if the
  //   to-restore set contains v17 (the SIMD register with encoding 17) and
  //   the ignore set contains d17 (the double register with encoding 17) then
  //   no part of the physical register with encoding 17 will be restored.
  //   (This is probably not true on arm32, since that has aliased float32
  //   registers; but none of our other targets do.)
  //
  // * {Push,store}RegsInMask/storeRegsInMask are further constrained as
  //   follows: when given the argument AllFloatRegisters, the resulting
  //   memory area must contain exactly all the SIMD/FP registers for the
  //   target at their widest width (that we care about).  [We have no targets
  //   where the SIMD registers and FP register sets are disjoint.]  They must
  //   be packed end-to-end with no holes, with the register with the lowest
  //   encoding number (0), as returned by FloatRegister::encoding(), at the
  //   abovementioned highest address, register 1 just below that, etc.
  //
  //   Furthermore the sizeof(RegisterContent) must equal the size of a SIMD
  //   register in the abovementioned array.
  //
  //   Furthermore the value returned by
  //   FloatRegister::getRegisterDumpOffsetInBytes must be a correct index
  //   into the abovementioned array.  Given the constraints, the only correct
  //   value is `reg.encoding() * sizeof(RegisterContent)`.
  //
  // Note that some of the routines listed above are JS-only, and do not support
  // SIMD registers. They are otherwise part of the same equivalence class.
  // Register spilling for e.g. OOL VM calls is implemented using
  // PushRegsInMask, and recovered on bailout using machineState. This requires
  // the same layout to be used in machineState, and therefore in all other code
  // that can spill registers that are recovered on bailout. Implementations of
  // JitRuntime::generate{Invalidator,BailoutHandler} should either call
  // PushRegsInMask, or check carefully to be sure that they generate the same
  // layout.

  // The size of the area used by PushRegsInMask.
  size_t PushRegsInMaskSizeInBytes(LiveRegisterSet set)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  void PushRegsInMask(LiveRegisterSet set)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);
  void PushRegsInMask(LiveGeneralRegisterSet set);

  // Like PushRegsInMask, but instead of pushing the registers, store them to
  // |dest|. |dest| should point to the end of the reserved space, so the
  // first register will be stored at |dest.offset - sizeof(register)|.  It is
  // required that |dest.offset| is at least as large as the value computed by
  // PushRegsInMaskSizeInBytes for this |set|.  In other words, |dest.base|
  // must point to either the lowest address in the save area, or some address
  // below that.
  void storeRegsInMask(LiveRegisterSet set, Address dest, Register scratch)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  void PopRegsInMask(LiveRegisterSet set);
  void PopRegsInMask(LiveGeneralRegisterSet set);
  void PopRegsInMaskIgnore(LiveRegisterSet set, LiveRegisterSet ignore)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  // ===============================================================
  // Stack manipulation functions -- single registers/values.

  void Push(const Operand op) DEFINED_ON(x86_shared);
  void Push(Register reg) PER_SHARED_ARCH;
  void Push(Register reg1, Register reg2, Register reg3, Register reg4)
      DEFINED_ON(arm64);
  void Push(const Imm32 imm) PER_SHARED_ARCH;
  void Push(const ImmWord imm) PER_SHARED_ARCH;
  void Push(const ImmPtr imm) PER_SHARED_ARCH;
  void Push(const ImmGCPtr ptr) PER_SHARED_ARCH;
  void Push(FloatRegister reg) PER_SHARED_ARCH;
  void PushBoxed(FloatRegister reg) PER_ARCH;
  void PushFlags() DEFINED_ON(x86_shared);
  void Push(PropertyKey key, Register scratchReg);
  void Push(const Address& addr);
  void Push(TypedOrValueRegister v);
  void Push(const ConstantOrRegister& v);
  void Push(const ValueOperand& val);
  void Push(const Value& val);
  void Push(JSValueType type, Register reg);
  void Push(const Register64 reg);
  void PushEmptyRooted(VMFunctionData::RootType rootType);
  inline CodeOffset PushWithPatch(ImmWord word);
  inline CodeOffset PushWithPatch(ImmPtr imm);

  void Pop(const Operand op) DEFINED_ON(x86_shared);
  void Pop(Register reg) PER_SHARED_ARCH;
  void Pop(FloatRegister t) PER_SHARED_ARCH;
  void Pop(const ValueOperand& val) PER_SHARED_ARCH;
  void PopFlags() DEFINED_ON(x86_shared);
  void PopStackPtr()
      DEFINED_ON(arm, mips_shared, x86_shared, loong64, riscv64, wasm32);
  void popRooted(VMFunctionData::RootType rootType, Register cellReg,
                 const ValueOperand& valueReg);

  // Move the stack pointer based on the requested amount.
  void adjustStack(int amount);
  void freeStack(uint32_t amount);

  // Warning: This method does not update the framePushed() counter.
  void freeStack(Register amount);

 private:
  // ===============================================================
  // Register allocation fields.
#ifdef DEBUG
  friend AutoRegisterScope;
  friend AutoFloatRegisterScope;
  // Used to track register scopes for debug builds.
  // Manipulated by the AutoGenericRegisterScope class.
  AllocatableRegisterSet debugTrackedRegisters_;
#endif  // DEBUG

 public:
  // ===============================================================
  // Simple call functions.

  // The returned CodeOffset is the assembler offset for the instruction
  // immediately following the call; that is, for the return point.
  CodeOffset call(Register reg) PER_SHARED_ARCH;
  CodeOffset call(Label* label) PER_SHARED_ARCH;

  void call(const Address& addr) PER_SHARED_ARCH;
  void call(ImmWord imm) PER_SHARED_ARCH;
  // Call a target native function, which is neither traceable nor movable.
  void call(ImmPtr imm) PER_SHARED_ARCH;
  CodeOffset call(wasm::SymbolicAddress imm) PER_SHARED_ARCH;
  inline CodeOffset call(const wasm::CallSiteDesc& desc,
                         wasm::SymbolicAddress imm);

  // Call a target JitCode, which must be traceable, and may be movable.
  void call(JitCode* c) PER_SHARED_ARCH;

  inline void call(TrampolinePtr code);

  inline CodeOffset call(const wasm::CallSiteDesc& desc, const Register reg);
  inline CodeOffset call(const wasm::CallSiteDesc& desc, uint32_t funcDefIndex);
  inline void call(const wasm::CallSiteDesc& desc, wasm::Trap trap);

  CodeOffset callWithPatch() PER_SHARED_ARCH;
  void patchCall(uint32_t callerOffset, uint32_t calleeOffset) PER_SHARED_ARCH;

  // Push the return address and make a call. On platforms where this function
  // is not defined, push the link register (pushReturnAddress) at the entry
  // point of the callee.
  void callAndPushReturnAddress(Register reg) DEFINED_ON(x86_shared);
  void callAndPushReturnAddress(Label* label) DEFINED_ON(x86_shared);

  // These do not adjust framePushed().
  void pushReturnAddress()
      DEFINED_ON(mips_shared, arm, arm64, loong64, riscv64, wasm32);
  void popReturnAddress()
      DEFINED_ON(mips_shared, arm, arm64, loong64, riscv64, wasm32);

  // Useful for dealing with two-valued returns.
  void moveRegPair(Register src0, Register src1, Register dst0, Register dst1,
                   MoveOp::Type type = MoveOp::GENERAL);

 public:
  // ===============================================================
  // Patchable near/far jumps.

  // "Far jumps" provide the ability to jump to any uint32_t offset from any
  // other uint32_t offset without using a constant pool (thus returning a
  // simple CodeOffset instead of a CodeOffsetJump).
  CodeOffset farJumpWithPatch() PER_SHARED_ARCH;
  void patchFarJump(CodeOffset farJump, uint32_t targetOffset) PER_SHARED_ARCH;

  // Emit a nop that can be patched to and from a nop and a call with int32
  // relative displacement.
  CodeOffset nopPatchableToCall() PER_SHARED_ARCH;
  void nopPatchableToCall(const wasm::CallSiteDesc& desc);
  static void patchNopToCall(uint8_t* callsite,
                             uint8_t* target) PER_SHARED_ARCH;
  static void patchCallToNop(uint8_t* callsite) PER_SHARED_ARCH;

  // These methods are like movWithPatch/PatchDataWithValueCheck but allow
  // using pc-relative addressing on certain platforms (RIP-relative LEA on x64,
  // ADR instruction on arm64).
  //
  // Note: "Near" applies to ARM64 where the target must be within 1 MB (this is
  // release-asserted).
  CodeOffset moveNearAddressWithPatch(Register dest) PER_ARCH;
  static void patchNearAddressMove(CodeLocationLabel loc,
                                   CodeLocationLabel target)
      DEFINED_ON(x86, x64, arm, arm64, loong64, riscv64, wasm32, mips_shared);

 public:
  // ===============================================================
  // [SMDOC] JIT-to-C++ Function Calls (callWithABI)
  //
  // callWithABI is used to make a call using the standard C/C++ system ABI.
  //
  // callWithABI is a low level interface for making calls, as such every call
  // made with callWithABI should be organized with 6 steps: spilling live
  // registers, aligning the stack, listing arguments of the called function,
  // calling a function pointer, extracting the returned value and restoring
  // live registers.
  //
  // A more detailed example of the six stages:
  //
  // 1) Saving of registers that are live. This will vary depending on which
  //    SpiderMonkey compiler you are working on. Registers that shouldn't be
  //    restored can be excluded.
  //
  //      LiveRegisterSet volatileRegs(...);
  //      volatileRegs.take(scratch);
  //      masm.PushRegsInMask(volatileRegs);
  //
  // 2) Align the stack to perform the call with the correct stack alignment.
  //
  //    When the stack pointer alignment is unknown and cannot be corrected
  //    when generating the code, setupUnalignedABICall must be used to
  //    dynamically align the stack pointer to the expectation of the ABI.
  //    When the stack pointer is known at JIT compilation time, the stack can
  //    be fixed manually and setupAlignedABICall and setupWasmABICall can be
  //    used.
  //
  //    setupWasmABICall is a special case of setupAlignedABICall as
  //    SpiderMonkey's WebAssembly implementation mostly follow the system
  //    ABI, except for float/double arguments, which always use floating
  //    point registers, even if this is not supported by the system ABI.
  //
  //      masm.setupUnalignedABICall(scratch);
  //
  // 3) Passing arguments. Arguments are passed left-to-right.
  //
  //      masm.passABIArg(scratch);
  //      masm.passABIArg(FloatOp0, MoveOp::Double);
  //
  //    Note how float register arguments are annotated with MoveOp::Double.
  //
  //    Concerning stack-relative address, see the note on passABIArg.
  //
  // 4) Make the call:
  //
  //      using Fn = int32_t (*)(int32_t)
  //      masm.callWithABI<Fn, Callee>();
  //
  //    In the case where the call returns a double, that needs to be
  //    indicated to the callWithABI like this:
  //
  //      using Fn = double (*)(int32_t)
  //      masm.callWithABI<Fn, Callee>(MoveOp::DOUBLE);
  //
  //    There are overloads to allow calls to registers and addresses.
  //
  // 5) Take care of the result
  //
  //      masm.storeCallPointerResult(scratch1);
  //      masm.storeCallBoolResult(scratch1);
  //      masm.storeCallInt32Result(scratch1);
  //      masm.storeCallFloatResult(scratch1);
  //
  // 6) Restore the potentially clobbered volatile registers
  //
  //      masm.PopRegsInMask(volatileRegs);
  //
  //    If expecting a returned value, this call should use
  //    PopRegsInMaskIgnore to filter out the registers which are containing
  //    the returned value.
  //
  // Unless an exit frame is pushed prior to the setupABICall, the callee
  // should not GC. To ensure this is the case callWithABI is instrumented to
  // make sure that in the default case callees are annotated with an
  // AutoUnsafeCallWithABI on the stack.
  //
  // A callWithABI can opt out of checking, if for example it is known there
  // is an exit frame, or the callee is known not to GC.
  //
  // If your callee needs to be able to GC, consider using a VMFunction, or
  // create a fake exit frame, and instrument the TraceJitExitFrame
  // accordingly.

  // Setup a call to C/C++ code, given the assumption that the framePushed
  // accurately defines the state of the stack, and that the top of the stack
  // was properly aligned. Note that this only supports cdecl.
  //
  // As a rule of thumb, this can be used in CodeGenerator but not in CacheIR or
  // Baseline code (because the stack is not aligned to ABIStackAlignment).
  void setupAlignedABICall();

  // As setupAlignedABICall, but for WebAssembly native ABI calls, which pass
  // through a builtin thunk that uses the wasm ABI. All the wasm ABI calls
  // can be native, since we always know the stack alignment a priori.
  void setupWasmABICall();

  // Setup an ABI call for when the alignment is not known. This may need a
  // scratch register.
  void setupUnalignedABICall(Register scratch) PER_ARCH;

  // Arguments must be assigned to a C/C++ call in order. They are moved
  // in parallel immediately before performing the call. This process may
  // temporarily use more stack, in which case esp-relative addresses will be
  // automatically adjusted. It is extremely important that esp-relative
  // addresses are computed *after* setupABICall(). Furthermore, no
  // operations should be emitted while setting arguments.
  void passABIArg(const MoveOperand& from, MoveOp::Type type);
  inline void passABIArg(Register reg);
  inline void passABIArg(FloatRegister reg, MoveOp::Type type);

  inline void callWithABI(
      DynFn fun, MoveOp::Type result = MoveOp::GENERAL,
      CheckUnsafeCallWithABI check = CheckUnsafeCallWithABI::Check);
  template <typename Sig, Sig fun>
  inline void callWithABI(
      MoveOp::Type result = MoveOp::GENERAL,
      CheckUnsafeCallWithABI check = CheckUnsafeCallWithABI::Check);
  inline void callWithABI(Register fun, MoveOp::Type result = MoveOp::GENERAL);
  inline void callWithABI(const Address& fun,
                          MoveOp::Type result = MoveOp::GENERAL);

  CodeOffset callWithABI(wasm::BytecodeOffset offset, wasm::SymbolicAddress fun,
                         mozilla::Maybe<int32_t> instanceOffset,
                         MoveOp::Type result = MoveOp::GENERAL);
  void callDebugWithABI(wasm::SymbolicAddress fun,
                        MoveOp::Type result = MoveOp::GENERAL);

 private:
  // Reinitialize the variables which have to be cleared before making a call
  // with callWithABI.
  template <class ABIArgGeneratorT>
  void setupABICallHelper();

  // Reinitialize the variables which have to be cleared before making a call
  // with native abi.
  void setupNativeABICall();

  // Reserve the stack and resolve the arguments move.
  void callWithABIPre(uint32_t* stackAdjust,
                      bool callFromWasm = false) PER_ARCH;

  // Emits a call to a C/C++ function, resolving all argument moves.
  void callWithABINoProfiler(void* fun, MoveOp::Type result,
                             CheckUnsafeCallWithABI check);
  void callWithABINoProfiler(Register fun, MoveOp::Type result) PER_ARCH;
  void callWithABINoProfiler(const Address& fun, MoveOp::Type result) PER_ARCH;

  // Restore the stack to its state before the setup function call.
  void callWithABIPost(uint32_t stackAdjust, MoveOp::Type result,
                       bool callFromWasm = false) PER_ARCH;

  // Create the signature to be able to decode the arguments of a native
  // function, when calling a function within the simulator.
  inline void appendSignatureType(MoveOp::Type type);
  inline ABIFunctionType signature() const;

  // Private variables used to handle moves between registers given as
  // arguments to passABIArg and the list of ABI registers expected for the
  // signature of the function.
  MoveResolver moveResolver_;

  // Architecture specific implementation which specify how registers & stack
  // offsets are used for calling a function.
  ABIArgGenerator abiArgs_;

#ifdef DEBUG
  // Flag use to assert that we use ABI function in the right context.
  bool inCall_;
#endif

  // If set by setupUnalignedABICall then callWithABI will pop the stack
  // register which is on the stack.
  bool dynamicAlignment_;

#ifdef JS_SIMULATOR
  // The signature is used to accumulate all types of arguments which are used
  // by the caller. This is used by the simulators to decode the arguments
  // properly, and cast the function pointer to the right type.
  uint32_t signature_;
#endif

 public:
  // ===============================================================
  // Jit Frames.
  //
  // These functions are used to build the content of the Jit frames.  See
  // CommonFrameLayout class, and all its derivatives. The content should be
  // pushed in the opposite order as the fields of the structures, such that
  // the structures can be used to interpret the content of the stack.

  // Call the Jit function, and push the return address (or let the callee
  // push the return address).
  //
  // These functions return the offset of the return address, in order to use
  // the return address to index the safepoints, which are used to list all
  // live registers.
  inline uint32_t callJitNoProfiler(Register callee);
  inline uint32_t callJit(Register callee);
  inline uint32_t callJit(JitCode* code);
  inline uint32_t callJit(TrampolinePtr code);
  inline uint32_t callJit(ImmPtr callee);

  // The frame descriptor is the second field of all Jit frames, pushed before
  // calling the Jit function. See CommonFrameLayout::descriptor_.
  inline void pushFrameDescriptor(FrameType type);
  inline void PushFrameDescriptor(FrameType type);

  // For JitFrameLayout, the descriptor also stores the number of arguments
  // passed by the caller. See MakeFrameDescriptorForJitCall.
  inline void pushFrameDescriptorForJitCall(FrameType type, uint32_t argc);
  inline void pushFrameDescriptorForJitCall(FrameType type, Register argc,
                                            Register scratch);
  inline void PushFrameDescriptorForJitCall(FrameType type, uint32_t argc);
  inline void PushFrameDescriptorForJitCall(FrameType type, Register argc,
                                            Register scratch);

  // Load the number of actual arguments from the frame's JitFrameLayout.
  inline void loadNumActualArgs(Register framePtr, Register dest);

  // Push the callee token of a JSFunction which pointer is stored in the
  // |callee| register. The callee token is packed with a |constructing| flag
  // which correspond to the fact that the JS function is called with "new" or
  // not.
  inline void PushCalleeToken(Register callee, bool constructing);

  // Unpack a callee token located at the |token| address, and return the
  // JSFunction pointer in the |dest| register.
  inline void loadFunctionFromCalleeToken(Address token, Register dest);

  // This function emulates a call by pushing an exit frame on the stack,
  // except that the fake-function is inlined within the body of the caller.
  //
  // This function assumes that the current frame is an IonJS frame.
  //
  // This function returns the offset of the /fake/ return address, in order to
  // use the return address to index the safepoints, which are used to list all
  // live registers.
  //
  // This function should be balanced with a call to adjustStack, to pop the
  // exit frame and emulate the return statement of the inlined function.
  inline uint32_t buildFakeExitFrame(Register scratch);

 private:
  // This function is used by buildFakeExitFrame to push a fake return address
  // on the stack. This fake return address should never be used for resuming
  // any execution, and can even be an invalid pointer into the instruction
  // stream, as long as it does not alias any other.
  uint32_t pushFakeReturnAddress(Register scratch) PER_SHARED_ARCH;

 public:
  // ===============================================================
  // Exit frame footer.
  //
  // When calling outside the Jit we push an exit frame. To mark the stack
  // correctly, we have to push additional information, called the Exit frame
  // footer, which is used to identify how the stack is marked.
  //
  // See JitFrames.h, and TraceJitExitFrame in JitFrames.cpp.

  // Push stub code and the VMFunctionData pointer.
  inline void enterExitFrame(Register cxreg, Register scratch,
                             const VMFunctionData* f);

  // Push an exit frame token to identify which fake exit frame this footer
  // corresponds to.
  inline void enterFakeExitFrame(Register cxreg, Register scratch,
                                 ExitFrameType type);

  // Push an exit frame token for a native call.
  inline void enterFakeExitFrameForNative(Register cxreg, Register scratch,
                                          bool isConstructing);

  // Pop ExitFrame footer in addition to the extra frame.
  inline void leaveExitFrame(size_t extraFrame = 0);

 private:
  // Save the top of the stack into JitActivation::packedExitFP of the
  // current thread, which should be the location of the latest exit frame.
  void linkExitFrame(Register cxreg, Register scratch);

 public:
  // ===============================================================
  // Move instructions

  inline void move64(Imm64 imm, Register64 dest) PER_ARCH;
  inline void move64(Register64 src, Register64 dest) PER_ARCH;

  inline void moveFloat32ToGPR(FloatRegister src,
                               Register dest) PER_SHARED_ARCH;
  inline void moveGPRToFloat32(Register src,
                               FloatRegister dest) PER_SHARED_ARCH;

  inline void moveDoubleToGPR64(FloatRegister src, Register64 dest) PER_ARCH;
  inline void moveGPR64ToDouble(Register64 src, FloatRegister dest) PER_ARCH;

  inline void move8SignExtend(Register src, Register dest) PER_SHARED_ARCH;
  inline void move16SignExtend(Register src, Register dest) PER_SHARED_ARCH;

  // move64To32 will clear the high bits of `dest` on 64-bit systems.
  inline void move64To32(Register64 src, Register dest) PER_ARCH;

  inline void move32To64ZeroExtend(Register src, Register64 dest) PER_ARCH;

  inline void move8To64SignExtend(Register src, Register64 dest) PER_ARCH;
  inline void move16To64SignExtend(Register src, Register64 dest) PER_ARCH;
  inline void move32To64SignExtend(Register src, Register64 dest) PER_ARCH;

  inline void move32SignExtendToPtr(Register src, Register dest) PER_ARCH;
  inline void move32ZeroExtendToPtr(Register src, Register dest) PER_ARCH;

  // Copy a constant, typed-register, or a ValueOperand into a ValueOperand
  // destination.
  inline void moveValue(const ConstantOrRegister& src,
                        const ValueOperand& dest);
  void moveValue(const TypedOrValueRegister& src,
                 const ValueOperand& dest) PER_ARCH;
  void moveValue(const ValueOperand& src, const ValueOperand& dest) PER_ARCH;
  void moveValue(const Value& src, const ValueOperand& dest) PER_ARCH;

  void movePropertyKey(PropertyKey key, Register dest);

  // ===============================================================
  // Load instructions

  inline void load32SignExtendToPtr(const Address& src, Register dest) PER_ARCH;

  inline void loadAbiReturnAddress(Register dest) PER_SHARED_ARCH;

 public:
  // ===============================================================
  // Logical instructions

  inline void not32(Register reg) PER_SHARED_ARCH;
  inline void notPtr(Register reg) PER_ARCH;

  inline void and32(Register src, Register dest) PER_SHARED_ARCH;
  inline void and32(Imm32 imm, Register dest) PER_SHARED_ARCH;
  inline void and32(Imm32 imm, Register src, Register dest) DEFINED_ON(arm64);
  inline void and32(Imm32 imm, const Address& dest) PER_SHARED_ARCH;
  inline void and32(const Address& src, Register dest) PER_SHARED_ARCH;

  inline void andPtr(Register src, Register dest) PER_ARCH;
  inline void andPtr(Imm32 imm, Register dest) PER_ARCH;

  inline void and64(Imm64 imm, Register64 dest) PER_ARCH;
  inline void or64(Imm64 imm, Register64 dest) PER_ARCH;
  inline void xor64(Imm64 imm, Register64 dest) PER_ARCH;

  inline void or32(Register src, Register dest) PER_SHARED_ARCH;
  inline void or32(Imm32 imm, Register dest) PER_SHARED_ARCH;
  inline void or32(Imm32 imm, const Address& dest) PER_SHARED_ARCH;

  inline void orPtr(Register src, Register dest) PER_ARCH;
  inline void orPtr(Imm32 imm, Register dest) PER_ARCH;

  inline void and64(Register64 src, Register64 dest) PER_ARCH;
  inline void or64(Register64 src, Register64 dest) PER_ARCH;
  inline void xor64(Register64 src, Register64 dest) PER_ARCH;

  inline void xor32(Register src, Register dest) PER_SHARED_ARCH;
  inline void xor32(Imm32 imm, Register dest) PER_SHARED_ARCH;
  inline void xor32(Imm32 imm, const Address& dest) PER_SHARED_ARCH;
  inline void xor32(const Address& src, Register dest) PER_SHARED_ARCH;

  inline void xorPtr(Register src, Register dest) PER_ARCH;
  inline void xorPtr(Imm32 imm, Register dest) PER_ARCH;

  inline void and64(const Operand& src, Register64 dest)
      DEFINED_ON(x64, mips64, loong64, riscv64);
  inline void or64(const Operand& src, Register64 dest)
      DEFINED_ON(x64, mips64, loong64, riscv64);
  inline void xor64(const Operand& src, Register64 dest)
      DEFINED_ON(x64, mips64, loong64, riscv64);

  // ===============================================================
  // Swap instructions

  // Swap the two lower bytes and sign extend the result to 32-bit.
  inline void byteSwap16SignExtend(Register reg) PER_SHARED_ARCH;

  // Swap the two lower bytes and zero extend the result to 32-bit.
  inline void byteSwap16ZeroExtend(Register reg) PER_SHARED_ARCH;

  // Swap all four bytes in a 32-bit integer.
  inline void byteSwap32(Register reg) PER_SHARED_ARCH;

  // Swap all eight bytes in a 64-bit integer.
  inline void byteSwap64(Register64 reg) PER_ARCH;

  // ===============================================================
  // Arithmetic functions

  // Condition flags aren't guaranteed to be set by these functions, for example
  // x86 will always set condition flags, but ARM64 won't do it unless
  // explicitly requested. Instead use branch(Add|Sub|Mul|Neg) to test for
  // condition flags after performing arithmetic operations.

  inline void add32(Register src, Register dest) PER_SHARED_ARCH;
  inline void add32(Imm32 imm, Register dest) PER_SHARED_ARCH;
  inline void add32(Imm32 imm, const Address& dest) PER_SHARED_ARCH;
  inline void add32(Imm32 imm, const AbsoluteAddress& dest)
      DEFINED_ON(x86_shared);

  inline void addPtr(Register src, Register dest) PER_ARCH;
  inline void addPtr(Register src1, Register src2, Register dest)
      DEFINED_ON(arm64);
  inline void addPtr(Imm32 imm, Register dest) PER_ARCH;
  inline void addPtr(Imm32 imm, Register src, Register dest) DEFINED_ON(arm64);
  inline void addPtr(ImmWord imm, Register dest) PER_ARCH;
  inline void addPtr(ImmPtr imm, Register dest);
  inline void addPtr(Imm32 imm, const Address& dest)
      DEFINED_ON(mips_shared, arm, arm64, x86, x64, loong64, riscv64, wasm32);
  inline void addPtr(Imm32 imm, const AbsoluteAddress& dest)
      DEFINED_ON(x86, x64);
  inline void addPtr(const Address& src, Register dest)
      DEFINED_ON(mips_shared, arm, arm64, x86, x64, loong64, riscv64, wasm32);

  inline void add64(Register64 src, Register64 dest) PER_ARCH;
  inline void add64(Imm32 imm, Register64 dest) PER_ARCH;
  inline void add64(Imm64 imm, Register64 dest) PER_ARCH;
  inline void add64(const Operand& src, Register64 dest)
      DEFINED_ON(x64, mips64, loong64, riscv64);

  inline void addFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

  // Compute dest=SP-imm where dest is a pointer registers and not SP.  The
  // offset returned from sub32FromStackPtrWithPatch() must be passed to
  // patchSub32FromStackPtr().
  inline CodeOffset sub32FromStackPtrWithPatch(Register dest) PER_ARCH;
  inline void patchSub32FromStackPtr(CodeOffset offset, Imm32 imm) PER_ARCH;

  inline void addDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;
  inline void addConstantDouble(double d, FloatRegister dest) DEFINED_ON(x86);

  inline void sub32(const Address& src, Register dest) PER_SHARED_ARCH;
  inline void sub32(Register src, Register dest) PER_SHARED_ARCH;
  inline void sub32(Imm32 imm, Register dest) PER_SHARED_ARCH;

  inline void subPtr(Register src, Register dest) PER_ARCH;
  inline void subPtr(Register src, const Address& dest)
      DEFINED_ON(mips_shared, arm, arm64, x86, x64, loong64, riscv64, wasm32);
  inline void subPtr(Imm32 imm, Register dest) PER_ARCH;
  inline void subPtr(ImmWord imm, Register dest) DEFINED_ON(x64);
  inline void subPtr(const Address& addr, Register dest)
      DEFINED_ON(mips_shared, arm, arm64, x86, x64, loong64, riscv64, wasm32);

  inline void sub64(Register64 src, Register64 dest) PER_ARCH;
  inline void sub64(Imm64 imm, Register64 dest) PER_ARCH;
  inline void sub64(const Operand& src, Register64 dest)
      DEFINED_ON(x64, mips64, loong64, riscv64);

  inline void subFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

  inline void subDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

  inline void mul32(Register rhs, Register srcDest) PER_SHARED_ARCH;
  inline void mul32(Imm32 imm, Register srcDest) PER_SHARED_ARCH;

  inline void mul32(Register src1, Register src2, Register dest, Label* onOver)
      DEFINED_ON(arm64);

  // Return the high word of the unsigned multiplication into |dest|.
  inline void mulHighUnsigned32(Imm32 imm, Register src,
                                Register dest) PER_ARCH;

  inline void mulPtr(Register rhs, Register srcDest) PER_ARCH;

  inline void mul64(const Operand& src, const Register64& dest) DEFINED_ON(x64);
  inline void mul64(const Operand& src, const Register64& dest,
                    const Register temp)
      DEFINED_ON(x64, mips64, loong64, riscv64);
  inline void mul64(Imm64 imm, const Register64& dest) PER_ARCH;
  inline void mul64(Imm64 imm, const Register64& dest, const Register temp)
      DEFINED_ON(x86, x64, arm, mips32, mips64, loong64, riscv64);
  inline void mul64(const Register64& src, const Register64& dest,
                    const Register temp) PER_ARCH;
  inline void mul64(const Register64& src1, const Register64& src2,
                    const Register64& dest) DEFINED_ON(arm64);
  inline void mul64(Imm64 src1, const Register64& src2, const Register64& dest)
      DEFINED_ON(arm64);

  inline void mulBy3(Register src, Register dest) PER_ARCH;

  inline void mulFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;
  inline void mulDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

  inline void mulDoublePtr(ImmPtr imm, Register temp, FloatRegister dest)
      DEFINED_ON(mips_shared, arm, arm64, x86, x64, loong64, riscv64, wasm32);

  // Perform an integer division, returning the integer part rounded toward
  // zero. rhs must not be zero, and the division must not overflow.
  //
  // On ARM, the chip must have hardware division instructions.
  inline void quotient32(Register rhs, Register srcDest, bool isUnsigned)
      DEFINED_ON(mips_shared, arm, arm64, loong64, riscv64, wasm32);

  // As above, but srcDest must be eax and tempEdx must be edx.
  inline void quotient32(Register rhs, Register srcDest, Register tempEdx,
                         bool isUnsigned) DEFINED_ON(x86_shared);

  // Perform an integer division, returning the remainder part.
  // rhs must not be zero, and the division must not overflow.
  //
  // On ARM, the chip must have hardware division instructions.
  inline void remainder32(Register rhs, Register srcDest, bool isUnsigned)
      DEFINED_ON(mips_shared, arm, arm64, loong64, riscv64, wasm32);

  // As above, but srcDest must be eax and tempEdx must be edx.
  inline void remainder32(Register rhs, Register srcDest, Register tempEdx,
                          bool isUnsigned) DEFINED_ON(x86_shared);

  // Perform an integer division, returning the integer part rounded toward
  // zero. rhs must not be zero, and the division must not overflow.
  //
  // This variant preserves registers, and doesn't require hardware division
  // instructions on ARM (will call out to a runtime routine).
  //
  // rhs is preserved, srdDest is clobbered.
  void flexibleRemainder32(Register rhs, Register srcDest, bool isUnsigned,
                           const LiveRegisterSet& volatileLiveRegs)
      DEFINED_ON(mips_shared, arm, arm64, x86_shared, loong64, riscv64, wasm32);

  // Perform an integer division, returning the integer part rounded toward
  // zero. rhs must not be zero, and the division must not overflow.
  //
  // This variant preserves registers, and doesn't require hardware division
  // instructions on ARM (will call out to a runtime routine).
  //
  // rhs is preserved, srdDest is clobbered.
  void flexibleQuotient32(Register rhs, Register srcDest, bool isUnsigned,
                          const LiveRegisterSet& volatileLiveRegs)
      DEFINED_ON(mips_shared, arm, arm64, x86_shared, loong64, riscv64);

  // Perform an integer division, returning the integer part rounded toward
  // zero. rhs must not be zero, and the division must not overflow. The
  // remainder is stored into the third argument register here.
  //
  // This variant preserves registers, and doesn't require hardware division
  // instructions on ARM (will call out to a runtime routine).
  //
  // rhs is preserved, srdDest and remOutput are clobbered.
  void flexibleDivMod32(Register rhs, Register srcDest, Register remOutput,
                        bool isUnsigned,
                        const LiveRegisterSet& volatileLiveRegs)
      DEFINED_ON(mips_shared, arm, arm64, x86_shared, loong64, riscv64, wasm32);

  inline void divFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;
  inline void divDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

  inline void inc64(AbsoluteAddress dest) PER_ARCH;

  inline void neg32(Register reg) PER_SHARED_ARCH;
  inline void neg64(Register64 reg) PER_ARCH;
  inline void negPtr(Register reg) PER_ARCH;

  inline void negateFloat(FloatRegister reg) PER_SHARED_ARCH;

  inline void negateDouble(FloatRegister reg) PER_SHARED_ARCH;

  inline void abs32(Register src, Register dest) PER_SHARED_ARCH;
  inline void absFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;
  inline void absDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

  inline void sqrtFloat32(FloatRegister src,
                          FloatRegister dest) PER_SHARED_ARCH;
  inline void sqrtDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

  void floorFloat32ToInt32(FloatRegister src, Register dest,
                           Label* fail) PER_SHARED_ARCH;
  void floorDoubleToInt32(FloatRegister src, Register dest,
                          Label* fail) PER_SHARED_ARCH;

  void ceilFloat32ToInt32(FloatRegister src, Register dest,
                          Label* fail) PER_SHARED_ARCH;
  void ceilDoubleToInt32(FloatRegister src, Register dest,
                         Label* fail) PER_SHARED_ARCH;

  void roundFloat32ToInt32(FloatRegister src, Register dest, FloatRegister temp,
                           Label* fail) PER_SHARED_ARCH;
  void roundDoubleToInt32(FloatRegister src, Register dest, FloatRegister temp,
                          Label* fail) PER_SHARED_ARCH;

  void truncFloat32ToInt32(FloatRegister src, Register dest,
                           Label* fail) PER_SHARED_ARCH;
  void truncDoubleToInt32(FloatRegister src, Register dest,
                          Label* fail) PER_SHARED_ARCH;

  void nearbyIntDouble(RoundingMode mode, FloatRegister src,
                       FloatRegister dest) PER_SHARED_ARCH;
  void nearbyIntFloat32(RoundingMode mode, FloatRegister src,
                        FloatRegister dest) PER_SHARED_ARCH;

  void signInt32(Register input, Register output);
  void signDouble(FloatRegister input, FloatRegister output);
  void signDoubleToInt32(FloatRegister input, Register output,
                         FloatRegister temp, Label* fail);

  void copySignDouble(FloatRegister lhs, FloatRegister rhs,
                      FloatRegister output) PER_SHARED_ARCH;
  void copySignFloat32(FloatRegister lhs, FloatRegister rhs,
                       FloatRegister output) DEFINED_ON(x86_shared, arm64);

  // Returns a random double in range [0, 1) in |dest|. The |rng| register must
  // hold a pointer to a mozilla::non_crypto::XorShift128PlusRNG.
  void randomDouble(Register rng, FloatRegister dest, Register64 temp0,
                    Register64 temp1);

  // srcDest = {min,max}{Float32,Double}(srcDest, other)
  // For min and max, handle NaN specially if handleNaN is true.

  inline void minFloat32(FloatRegister other, FloatRegister srcDest,
                         bool handleNaN) PER_SHARED_ARCH;
  inline void minDouble(FloatRegister other, FloatRegister srcDest,
                        bool handleNaN) PER_SHARED_ARCH;

  inline void maxFloat32(FloatRegister other, FloatRegister srcDest,
                         bool handleNaN) PER_SHARED_ARCH;
  inline void maxDouble(FloatRegister other, FloatRegister srcDest,
                        bool handleNaN) PER_SHARED_ARCH;

  void minMaxArrayInt32(Register array, Register result, Register temp1,
                        Register temp2, Register temp3, bool isMax,
                        Label* fail);
  void minMaxArrayNumber(Register array, FloatRegister result,
                         FloatRegister floatTemp, Register temp1,
                         Register temp2, bool isMax, Label* fail);

  // Compute |pow(base, power)| and store the result in |dest|. If the result
  // exceeds the int32 range, jumps to |onOver|.
  // |base| and |power| are preserved, the other input registers are clobbered.
  void pow32(Register base, Register power, Register dest, Register temp1,
             Register temp2, Label* onOver);

  void sameValueDouble(FloatRegister left, FloatRegister right,
                       FloatRegister temp, Register dest);

  void branchIfNotRegExpPrototypeOptimizable(Register proto, Register temp,
                                             Label* label);
  void branchIfNotRegExpInstanceOptimizable(Register regexp, Register temp,
                                            Label* label);

  void loadRegExpLastIndex(Register regexp, Register string, Register lastIndex,
                           Label* notFoundZeroLastIndex);

  // ===============================================================
  // Shift functions

  // For shift-by-register there may be platform-specific variations, for
  // example, x86 will perform the shift mod 32 but ARM will perform the shift
  // mod 256.
  //
  // For shift-by-immediate the platform assembler may restrict the immediate,
  // for example, the ARM assembler requires the count for 32-bit shifts to be
  // in the range [0,31].

  inline void lshift32(Imm32 shift, Register srcDest) PER_SHARED_ARCH;
  inline void rshift32(Imm32 shift, Register srcDest) PER_SHARED_ARCH;
  inline void rshift32Arithmetic(Imm32 shift, Register srcDest) PER_SHARED_ARCH;

  inline void lshiftPtr(Imm32 imm, Register dest) PER_ARCH;
  inline void rshiftPtr(Imm32 imm, Register dest) PER_ARCH;
  inline void rshiftPtr(Imm32 imm, Register src, Register dest)
      DEFINED_ON(arm64);
  inline void rshiftPtrArithmetic(Imm32 imm, Register dest) PER_ARCH;

  inline void lshift64(Imm32 imm, Register64 dest) PER_ARCH;
  inline void rshift64(Imm32 imm, Register64 dest) PER_ARCH;
  inline void rshift64Arithmetic(Imm32 imm, Register64 dest) PER_ARCH;

  // On x86_shared these have the constraint that shift must be in CL.
  inline void lshift32(Register shift, Register srcDest) PER_SHARED_ARCH;
  inline void rshift32(Register shift, Register srcDest) PER_SHARED_ARCH;
  inline void rshift32Arithmetic(Register shift,
                                 Register srcDest) PER_SHARED_ARCH;
  inline void lshiftPtr(Register shift, Register srcDest) PER_ARCH;
  inline void rshiftPtr(Register shift, Register srcDest) PER_ARCH;

  // These variants do not have the above constraint, but may emit some extra
  // instructions on x86_shared. They also handle shift >= 32 consistently by
  // masking with 0x1F (either explicitly or relying on the hardware to do
  // that).
  inline void flexibleLshift32(Register shift,
                               Register srcDest) PER_SHARED_ARCH;
  inline void flexibleRshift32(Register shift,
                               Register srcDest) PER_SHARED_ARCH;
  inline void flexibleRshift32Arithmetic(Register shift,
                                         Register srcDest) PER_SHARED_ARCH;

  inline void lshift64(Register shift, Register64 srcDest) PER_ARCH;
  inline void rshift64(Register shift, Register64 srcDest) PER_ARCH;
  inline void rshift64Arithmetic(Register shift, Register64 srcDest) PER_ARCH;

  // ===============================================================
  // Rotation functions
  // Note: - on x86 and x64 the count register must be in CL.
  //       - on x64 the temp register should be InvalidReg.

  inline void rotateLeft(Imm32 count, Register input,
                         Register dest) PER_SHARED_ARCH;
  inline void rotateLeft(Register count, Register input,
                         Register dest) PER_SHARED_ARCH;
  inline void rotateLeft64(Imm32 count, Register64 input, Register64 dest)
      DEFINED_ON(x64);
  inline void rotateLeft64(Register count, Register64 input, Register64 dest)
      DEFINED_ON(x64);
  inline void rotateLeft64(Imm32 count, Register64 input, Register64 dest,
                           Register temp) PER_ARCH;
  inline void rotateLeft64(Register count, Register64 input, Register64 dest,
                           Register temp) PER_ARCH;

  inline void rotateRight(Imm32 count, Register input,
                          Register dest) PER_SHARED_ARCH;
  inline void rotateRight(Register count, Register input,
                          Register dest) PER_SHARED_ARCH;
  inline void rotateRight64(Imm32 count, Register64 input, Register64 dest)
      DEFINED_ON(x64);
  inline void rotateRight64(Register count, Register64 input, Register64 dest)
      DEFINED_ON(x64);
  inline void rotateRight64(Imm32 count, Register64 input, Register64 dest,
                            Register temp) PER_ARCH;
  inline void rotateRight64(Register count, Register64 input, Register64 dest,
                            Register temp) PER_ARCH;

  // ===============================================================
  // Bit counting functions

  // knownNotZero may be true only if the src is known not to be zero.
  inline void clz32(Register src, Register dest,
                    bool knownNotZero) PER_SHARED_ARCH;
  inline void ctz32(Register src, Register dest,
                    bool knownNotZero) PER_SHARED_ARCH;

  inline void clz64(Register64 src, Register dest) PER_ARCH;
  inline void ctz64(Register64 src, Register dest) PER_ARCH;

  // On x86_shared, temp may be Invalid only if the chip has the POPCNT
  // instruction. On ARM, temp may never be Invalid.
  inline void popcnt32(Register src, Register dest,
                       Register temp) PER_SHARED_ARCH;

  // temp may be invalid only if the chip has the POPCNT instruction.
  inline void popcnt64(Register64 src, Register64 dest, Register temp) PER_ARCH;

  // ===============================================================
  // Condition functions

  inline void cmp8Set(Condition cond, Address lhs, Imm32 rhs,
                      Register dest) PER_SHARED_ARCH;

  inline void cmp16Set(Condition cond, Address lhs, Imm32 rhs,
                       Register dest) PER_SHARED_ARCH;

  template <typename T1, typename T2>
  inline void cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest)
      DEFINED_ON(x86_shared, arm, arm64, mips32, mips64, loong64, riscv64,
                 wasm32);

  // Only the NotEqual and Equal conditions are allowed.
  inline void cmp64Set(Condition cond, Address lhs, Imm64 rhs,
                       Register dest) PER_ARCH;

  template <typename T1, typename T2>
  inline void cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest) PER_ARCH;

  // ===============================================================
  // Branch functions

  inline void branch8(Condition cond, const Address& lhs, Imm32 rhs,
                      Label* label) PER_SHARED_ARCH;

  // Compares the byte in |lhs| against |rhs| using a 8-bit comparison on
  // x86/x64 or a 32-bit comparison (all other platforms). The caller should
  // ensure |rhs| is a zero- resp. sign-extended byte value for cross-platform
  // compatible code.
  inline void branch8(Condition cond, const BaseIndex& lhs, Register rhs,
                      Label* label) PER_SHARED_ARCH;

  inline void branch16(Condition cond, const Address& lhs, Imm32 rhs,
                       Label* label) PER_SHARED_ARCH;

  template <class L>
  inline void branch32(Condition cond, Register lhs, Register rhs,
                       L label) PER_SHARED_ARCH;
  template <class L>
  inline void branch32(Condition cond, Register lhs, Imm32 rhs,
                       L label) PER_SHARED_ARCH;

  inline void branch32(Condition cond, Register lhs, const Address& rhs,
                       Label* label) DEFINED_ON(arm64);

  inline void branch32(Condition cond, const Address& lhs, Register rhs,
                       Label* label) PER_SHARED_ARCH;
  inline void branch32(Condition cond, const Address& lhs, Imm32 rhs,
                       Label* label) PER_SHARED_ARCH;

  inline void branch32(Condition cond, const AbsoluteAddress& lhs, Register rhs,
                       Label* label)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);
  inline void branch32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs,
                       Label* label)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);

  inline void branch32(Condition cond, const BaseIndex& lhs, Register rhs,
                       Label* label) DEFINED_ON(arm, x86_shared);
  inline void branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs,
                       Label* label) PER_SHARED_ARCH;

  inline void branch32(Condition cond, const Operand& lhs, Register rhs,
                       Label* label) DEFINED_ON(x86_shared);
  inline void branch32(Condition cond, const Operand& lhs, Imm32 rhs,
                       Label* label) DEFINED_ON(x86_shared);

  inline void branch32(Condition cond, wasm::SymbolicAddress lhs, Imm32 rhs,
                       Label* label)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);

  // The supported condition are Equal, NotEqual, LessThan(orEqual),
  // GreaterThan(orEqual), Below(orEqual) and Above(orEqual). When a fail label
  // is not defined it will fall through to next instruction, else jump to the
  // fail label.
  inline void branch64(Condition cond, Register64 lhs, Imm64 val,
                       Label* success, Label* fail = nullptr) PER_ARCH;
  inline void branch64(Condition cond, Register64 lhs, Register64 rhs,
                       Label* success, Label* fail = nullptr) PER_ARCH;
  // Only the NotEqual and Equal conditions are allowed for the branch64
  // variants with Address as lhs.
  inline void branch64(Condition cond, const Address& lhs, Imm64 val,
                       Label* label) PER_ARCH;
  inline void branch64(Condition cond, const Address& lhs, Register64 rhs,
                       Label* label) PER_ARCH;

  // Compare the value at |lhs| with the value at |rhs|.  The scratch
  // register *must not* be the base of |lhs| or |rhs|.
  inline void branch64(Condition cond, const Address& lhs, const Address& rhs,
                       Register scratch, Label* label) PER_ARCH;

  template <class L>
  inline void branchPtr(Condition cond, Register lhs, Register rhs,
                        L label) PER_SHARED_ARCH;
  inline void branchPtr(Condition cond, Register lhs, Imm32 rhs,
                        Label* label) PER_SHARED_ARCH;
  inline void branchPtr(Condition cond, Register lhs, ImmPtr rhs,
                        Label* label) PER_SHARED_ARCH;
  inline void branchPtr(Condition cond, Register lhs, ImmGCPtr rhs,
                        Label* label) PER_SHARED_ARCH;
  inline void branchPtr(Condition cond, Register lhs, ImmWord rhs,
                        Label* label) PER_SHARED_ARCH;

  template <class L>
  inline void branchPtr(Condition cond, const Address& lhs, Register rhs,
                        L label) PER_SHARED_ARCH;
  inline void branchPtr(Condition cond, const Address& lhs, ImmPtr rhs,
                        Label* label) PER_SHARED_ARCH;
  inline void branchPtr(Condition cond, const Address& lhs, ImmGCPtr rhs,
                        Label* label) PER_SHARED_ARCH;
  inline void branchPtr(Condition cond, const Address& lhs, ImmWord rhs,
                        Label* label) PER_SHARED_ARCH;

  inline void branchPtr(Condition cond, const BaseIndex& lhs, ImmWord rhs,
                        Label* label) PER_SHARED_ARCH;
  inline void branchPtr(Condition cond, const BaseIndex& lhs, Register rhs,
                        Label* label) PER_SHARED_ARCH;

  inline void branchPtr(Condition cond, const AbsoluteAddress& lhs,
                        Register rhs, Label* label)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);
  inline void branchPtr(Condition cond, const AbsoluteAddress& lhs, ImmWord rhs,
                        Label* label)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);

  inline void branchPtr(Condition cond, wasm::SymbolicAddress lhs, Register rhs,
                        Label* label)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);

  // Given a pointer to a GC Cell, retrieve the StoreBuffer pointer from its
  // chunk header, or nullptr if it is in the tenured heap.
  void loadStoreBuffer(Register ptr, Register buffer) PER_ARCH;

  void branchPtrInNurseryChunk(Condition cond, Register ptr, Register temp,
                               Label* label)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);
  void branchPtrInNurseryChunk(Condition cond, const Address& address,
                               Register temp, Label* label) DEFINED_ON(x86);
  void branchValueIsNurseryCell(Condition cond, const Address& address,
                                Register temp, Label* label) PER_ARCH;
  void branchValueIsNurseryCell(Condition cond, ValueOperand value,
                                Register temp, Label* label) PER_ARCH;

  // This function compares a Value (lhs) which is having a private pointer
  // boxed inside a js::Value, with a raw pointer (rhs).
  inline void branchPrivatePtr(Condition cond, const Address& lhs, Register rhs,
                               Label* label) PER_ARCH;

  inline void branchFloat(DoubleCondition cond, FloatRegister lhs,
                          FloatRegister rhs, Label* label) PER_SHARED_ARCH;

  // Truncate a double/float32 to int32 and when it doesn't fit an int32 it will
  // jump to the failure label. This particular variant is allowed to return the
  // value module 2**32, which isn't implemented on all architectures. E.g. the
  // x64 variants will do this only in the int64_t range.
  inline void branchTruncateFloat32MaybeModUint32(FloatRegister src,
                                                  Register dest, Label* fail)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);
  inline void branchTruncateDoubleMaybeModUint32(FloatRegister src,
                                                 Register dest, Label* fail)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);

  // Truncate a double/float32 to intptr and when it doesn't fit jump to the
  // failure label.
  inline void branchTruncateFloat32ToPtr(FloatRegister src, Register dest,
                                         Label* fail) DEFINED_ON(x86, x64);
  inline void branchTruncateDoubleToPtr(FloatRegister src, Register dest,
                                        Label* fail) DEFINED_ON(x86, x64);

  // Truncate a double/float32 to int32 and when it doesn't fit jump to the
  // failure label.
  inline void branchTruncateFloat32ToInt32(FloatRegister src, Register dest,
                                           Label* fail)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);
  inline void branchTruncateDoubleToInt32(FloatRegister src, Register dest,
                                          Label* fail) PER_ARCH;

  inline void branchDouble(DoubleCondition cond, FloatRegister lhs,
                           FloatRegister rhs, Label* label) PER_SHARED_ARCH;

  inline void branchDoubleNotInInt64Range(Address src, Register temp,
                                          Label* fail);
  inline void branchDoubleNotInUInt64Range(Address src, Register temp,
                                           Label* fail);
  inline void branchFloat32NotInInt64Range(Address src, Register temp,
                                           Label* fail);
  inline void branchFloat32NotInUInt64Range(Address src, Register temp,
                                            Label* fail);

  template <typename T>
  inline void branchAdd32(Condition cond, T src, Register dest,
                          Label* label) PER_SHARED_ARCH;
  template <typename T>
  inline void branchSub32(Condition cond, T src, Register dest,
                          Label* label) PER_SHARED_ARCH;
  template <typename T>
  inline void branchMul32(Condition cond, T src, Register dest,
                          Label* label) PER_SHARED_ARCH;
  template <typename T>
  inline void branchRshift32(Condition cond, T src, Register dest,
                             Label* label) PER_SHARED_ARCH;

  inline void branchNeg32(Condition cond, Register reg,
                          Label* label) PER_SHARED_ARCH;

  inline void branchAdd64(Condition cond, Imm64 imm, Register64 dest,
                          Label* label) DEFINED_ON(x86, arm, wasm32);

  template <typename T>
  inline void branchAddPtr(Condition cond, T src, Register dest,
                           Label* label) PER_SHARED_ARCH;

  template <typename T>
  inline void branchSubPtr(Condition cond, T src, Register dest,
                           Label* label) PER_SHARED_ARCH;

  inline void branchMulPtr(Condition cond, Register src, Register dest,
                           Label* label) PER_SHARED_ARCH;

  inline void decBranchPtr(Condition cond, Register lhs, Imm32 rhs,
                           Label* label) PER_SHARED_ARCH;

  template <class L>
  inline void branchTest32(Condition cond, Register lhs, Register rhs,
                           L label) PER_SHARED_ARCH;
  template <class L>
  inline void branchTest32(Condition cond, Register lhs, Imm32 rhs,
                           L label) PER_SHARED_ARCH;
  inline void branchTest32(Condition cond, const Address& lhs, Imm32 rhh,
                           Label* label) PER_SHARED_ARCH;
  inline void branchTest32(Condition cond, const AbsoluteAddress& lhs,
                           Imm32 rhs, Label* label)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);

  template <class L>
  inline void branchTestPtr(Condition cond, Register lhs, Register rhs,
                            L label) PER_SHARED_ARCH;
  inline void branchTestPtr(Condition cond, Register lhs, Imm32 rhs,
                            Label* label) PER_SHARED_ARCH;
  inline void branchTestPtr(Condition cond, const Address& lhs, Imm32 rhs,
                            Label* label) PER_SHARED_ARCH;

  template <class L>
  inline void branchTest64(Condition cond, Register64 lhs, Register64 rhs,
                           Register temp, L label) PER_ARCH;

  // Branches to |label| if |reg| is false. |reg| should be a C++ bool.
  template <class L>
  inline void branchIfFalseBool(Register reg, L label);

  // Branches to |label| if |reg| is true. |reg| should be a C++ bool.
  inline void branchIfTrueBool(Register reg, Label* label);

  inline void branchIfRope(Register str, Label* label);
  inline void branchIfNotRope(Register str, Label* label);

  inline void branchLatin1String(Register string, Label* label);
  inline void branchTwoByteString(Register string, Label* label);

  inline void branchIfBigIntIsNegative(Register bigInt, Label* label);
  inline void branchIfBigIntIsNonNegative(Register bigInt, Label* label);
  inline void branchIfBigIntIsZero(Register bigInt, Label* label);
  inline void branchIfBigIntIsNonZero(Register bigInt, Label* label);

  inline void branchTestFunctionFlags(Register fun, uint32_t flags,
                                      Condition cond, Label* label);

  inline void branchIfNotFunctionIsNonBuiltinCtor(Register fun,
                                                  Register scratch,
                                                  Label* label);

  inline void branchIfFunctionHasNoJitEntry(Register fun, bool isConstructing,
                                            Label* label);
  inline void branchIfFunctionHasJitEntry(Register fun, bool isConstructing,
                                          Label* label);

  inline void branchIfScriptHasJitScript(Register script, Label* label);
  inline void branchIfScriptHasNoJitScript(Register script, Label* label);
  inline void loadJitScript(Register script, Register dest);

  // Loads the function's argument count.
  inline void loadFunctionArgCount(Register func, Register output);

  // Loads the function length. This handles interpreted, native, and bound
  // functions. The caller is responsible for checking that INTERPRETED_LAZY and
  // RESOLVED_LENGTH flags are not set.
  void loadFunctionLength(Register func, Register funFlagsAndArgCount,
                          Register output, Label* slowPath);

  // Loads the function name. This handles interpreted, native, and bound
  // functions.
  void loadFunctionName(Register func, Register output, ImmGCPtr emptyString,
                        Label* slowPath);

  void assertFunctionIsExtended(Register func);

  inline void branchFunctionKind(Condition cond,
                                 FunctionFlags::FunctionKind kind, Register fun,
                                 Register scratch, Label* label);

  inline void branchIfObjectEmulatesUndefined(Register objReg, Register scratch,
                                              Label* slowCheck, Label* label);

  // For all methods below: spectreRegToZero is a register that will be zeroed
  // on speculatively executed code paths (when the branch should be taken but
  // branch prediction speculates it isn't). Usually this will be the object
  // register but the caller may pass a different register.

  inline void branchTestObjClass(Condition cond, Register obj,
                                 const JSClass* clasp, Register scratch,
                                 Register spectreRegToZero, Label* label);
  inline void branchTestObjClassNoSpectreMitigations(Condition cond,
                                                     Register obj,
                                                     const JSClass* clasp,
                                                     Register scratch,
                                                     Label* label);

  inline void branchTestObjClass(Condition cond, Register obj,
                                 const Address& clasp, Register scratch,
                                 Register spectreRegToZero, Label* label);
  inline void branchTestObjClassNoSpectreMitigations(Condition cond,
                                                     Register obj,
                                                     const Address& clasp,
                                                     Register scratch,
                                                     Label* label);

  inline void branchTestObjClass(Condition cond, Register obj, Register clasp,
                                 Register scratch, Register spectreRegToZero,
                                 Label* label);

  inline void branchTestObjShape(Condition cond, Register obj,
                                 const Shape* shape, Register scratch,
                                 Register spectreRegToZero, Label* label);
  inline void branchTestObjShapeNoSpectreMitigations(Condition cond,
                                                     Register obj,
                                                     const Shape* shape,
                                                     Label* label);

  void branchTestObjShapeList(Condition cond, Register obj,
                              Register shapeElements, Register shapeScratch,
                              Register endScratch, Register spectreScratch,
                              Label* label);

  inline void branchTestClassIsFunction(Condition cond, Register clasp,
                                        Label* label);
  inline void branchTestObjIsFunction(Condition cond, Register obj,
                                      Register scratch,
                                      Register spectreRegToZero, Label* label);
  inline void branchTestObjIsFunctionNoSpectreMitigations(Condition cond,
                                                          Register obj,
                                                          Register scratch,
                                                          Label* label);

  inline void branchTestObjShape(Condition cond, Register obj, Register shape,
                                 Register scratch, Register spectreRegToZero,
                                 Label* label);
  inline void branchTestObjShapeNoSpectreMitigations(Condition cond,
                                                     Register obj,
                                                     Register shape,
                                                     Label* label);

  // TODO: audit/fix callers to be Spectre safe.
  inline void branchTestObjShapeUnsafe(Condition cond, Register obj,
                                       Register shape, Label* label);

  void branchTestObjCompartment(Condition cond, Register obj,
                                const Address& compartment, Register scratch,
                                Label* label);
  void branchTestObjCompartment(Condition cond, Register obj,
                                const JS::Compartment* compartment,
                                Register scratch, Label* label);

  void branchIfNonNativeObj(Register obj, Register scratch, Label* label);

  void branchIfObjectNotExtensible(Register obj, Register scratch,
                                   Label* label);

  inline void branchTestClassIsProxy(bool proxy, Register clasp, Label* label);

  inline void branchTestObjectIsProxy(bool proxy, Register object,
                                      Register scratch, Label* label);

  inline void branchTestProxyHandlerFamily(Condition cond, Register proxy,
                                           Register scratch,
                                           const void* handlerp, Label* label);

  inline void branchTestObjectIsWasmGcObject(bool isGcObject, Register obj,
                                             Register scratch, Label* label);

  inline void branchTestNeedsIncrementalBarrier(Condition cond, Label* label);
  inline void branchTestNeedsIncrementalBarrierAnyZone(Condition cond,
                                                       Label* label,
                                                       Register scratch);

  // Perform a type-test on a tag of a Value (32bits boxing), or the tagged
  // value (64bits boxing).
  inline void branchTestUndefined(Condition cond, Register tag,
                                  Label* label) PER_SHARED_ARCH;
  inline void branchTestInt32(Condition cond, Register tag,
                              Label* label) PER_SHARED_ARCH;
  inline void branchTestDouble(Condition cond, Register tag, Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);
  inline void branchTestNumber(Condition cond, Register tag,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestBoolean(Condition cond, Register tag,
                                Label* label) PER_SHARED_ARCH;
  inline void branchTestString(Condition cond, Register tag,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestSymbol(Condition cond, Register tag,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestBigInt(Condition cond, Register tag,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestNull(Condition cond, Register tag,
                             Label* label) PER_SHARED_ARCH;
  inline void branchTestObject(Condition cond, Register tag,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestPrimitive(Condition cond, Register tag,
                                  Label* label) PER_SHARED_ARCH;
  inline void branchTestMagic(Condition cond, Register tag,
                              Label* label) PER_SHARED_ARCH;
  void branchTestType(Condition cond, Register tag, JSValueType type,
                      Label* label);

  // Perform a type-test on a Value, addressed by Address or BaseIndex, or
  // loaded into ValueOperand.
  // BaseIndex and ValueOperand variants clobber the ScratchReg on x64.
  // All Variants clobber the ScratchReg on arm64.
  inline void branchTestUndefined(Condition cond, const Address& address,
                                  Label* label) PER_SHARED_ARCH;
  inline void branchTestUndefined(Condition cond, const BaseIndex& address,
                                  Label* label) PER_SHARED_ARCH;
  inline void branchTestUndefined(Condition cond, const ValueOperand& value,
                                  Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestInt32(Condition cond, const Address& address,
                              Label* label) PER_SHARED_ARCH;
  inline void branchTestInt32(Condition cond, const BaseIndex& address,
                              Label* label) PER_SHARED_ARCH;
  inline void branchTestInt32(Condition cond, const ValueOperand& value,
                              Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestDouble(Condition cond, const Address& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestDouble(Condition cond, const BaseIndex& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestDouble(Condition cond, const ValueOperand& value,
                               Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestNumber(Condition cond, const ValueOperand& value,
                               Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestBoolean(Condition cond, const Address& address,
                                Label* label) PER_SHARED_ARCH;
  inline void branchTestBoolean(Condition cond, const BaseIndex& address,
                                Label* label) PER_SHARED_ARCH;
  inline void branchTestBoolean(Condition cond, const ValueOperand& value,
                                Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestString(Condition cond, const Address& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestString(Condition cond, const BaseIndex& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestString(Condition cond, const ValueOperand& value,
                               Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestSymbol(Condition cond, const Address& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestSymbol(Condition cond, const BaseIndex& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestSymbol(Condition cond, const ValueOperand& value,
                               Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestBigInt(Condition cond, const Address& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestBigInt(Condition cond, const BaseIndex& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestBigInt(Condition cond, const ValueOperand& value,
                               Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestNull(Condition cond, const Address& address,
                             Label* label) PER_SHARED_ARCH;
  inline void branchTestNull(Condition cond, const BaseIndex& address,
                             Label* label) PER_SHARED_ARCH;
  inline void branchTestNull(Condition cond, const ValueOperand& value,
                             Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  // Clobbers the ScratchReg on x64.
  inline void branchTestObject(Condition cond, const Address& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestObject(Condition cond, const BaseIndex& address,
                               Label* label) PER_SHARED_ARCH;
  inline void branchTestObject(Condition cond, const ValueOperand& value,
                               Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestGCThing(Condition cond, const Address& address,
                                Label* label) PER_SHARED_ARCH;
  inline void branchTestGCThing(Condition cond, const BaseIndex& address,
                                Label* label) PER_SHARED_ARCH;
  inline void branchTestGCThing(Condition cond, const ValueOperand& value,
                                Label* label) PER_SHARED_ARCH;

  inline void branchTestPrimitive(Condition cond, const ValueOperand& value,
                                  Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestMagic(Condition cond, const Address& address,
                              Label* label) PER_SHARED_ARCH;
  inline void branchTestMagic(Condition cond, const BaseIndex& address,
                              Label* label) PER_SHARED_ARCH;
  template <class L>
  inline void branchTestMagic(Condition cond, const ValueOperand& value,
                              L label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  inline void branchTestMagic(Condition cond, const Address& valaddr,
                              JSWhyMagic why, Label* label) PER_ARCH;

  inline void branchTestMagicValue(Condition cond, const ValueOperand& val,
                                   JSWhyMagic why, Label* label);

  void branchTestValue(Condition cond, const ValueOperand& lhs,
                       const Value& rhs, Label* label) PER_ARCH;

  inline void branchTestValue(Condition cond, const BaseIndex& lhs,
                              const ValueOperand& rhs, Label* label) PER_ARCH;

  // Checks if given Value is evaluated to true or false in a condition.
  // The type of the value should match the type of the method.
  inline void branchTestInt32Truthy(bool truthy, const ValueOperand& value,
                                    Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, x86_shared,
                 wasm32);
  inline void branchTestDoubleTruthy(bool truthy, FloatRegister reg,
                                     Label* label) PER_SHARED_ARCH;
  inline void branchTestBooleanTruthy(bool truthy, const ValueOperand& value,
                                      Label* label) PER_ARCH;
  inline void branchTestStringTruthy(bool truthy, const ValueOperand& value,
                                     Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);
  inline void branchTestBigIntTruthy(bool truthy, const ValueOperand& value,
                                     Label* label)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, wasm32,
                 x86_shared);

  // Create an unconditional branch to the address given as argument.
  inline void branchToComputedAddress(const BaseIndex& address) PER_ARCH;

 private:
  template <typename T, typename S, typename L>
  inline void branchPtrImpl(Condition cond, const T& lhs, const S& rhs, L label)
      DEFINED_ON(x86_shared);

  void branchPtrInNurseryChunkImpl(Condition cond, Register ptr, Label* label)
      DEFINED_ON(x86);
  template <typename T>
  void branchValueIsNurseryCellImpl(Condition cond, const T& value,
                                    Register temp, Label* label)
      DEFINED_ON(arm64, x64, mips64, loong64, riscv64);

  template <typename T>
  inline void branchTestUndefinedImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestInt32Impl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestDoubleImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestNumberImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestBooleanImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestStringImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestSymbolImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestBigIntImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestNullImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestObjectImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T>
  inline void branchTestGCThingImpl(Condition cond, const T& t,
                                    Label* label) PER_SHARED_ARCH;
  template <typename T>
  inline void branchTestPrimitiveImpl(Condition cond, const T& t, Label* label)
      DEFINED_ON(arm, arm64, x86_shared);
  template <typename T, class L>
  inline void branchTestMagicImpl(Condition cond, const T& t, L label)
      DEFINED_ON(arm, arm64, x86_shared);

 public:
  template <typename T>
  inline void testNumberSet(Condition cond, const T& src,
                            Register dest) PER_SHARED_ARCH;
  template <typename T>
  inline void testBooleanSet(Condition cond, const T& src,
                             Register dest) PER_SHARED_ARCH;
  template <typename T>
  inline void testStringSet(Condition cond, const T& src,
                            Register dest) PER_SHARED_ARCH;
  template <typename T>
  inline void testSymbolSet(Condition cond, const T& src,
                            Register dest) PER_SHARED_ARCH;
  template <typename T>
  inline void testBigIntSet(Condition cond, const T& src,
                            Register dest) PER_SHARED_ARCH;

 public:
  // The fallibleUnbox* methods below combine a Value type check with an unbox.
  // Especially on 64-bit platforms this can be implemented more efficiently
  // than a separate branch + unbox.
  //
  // |src| and |dest| can be the same register, but |dest| may hold garbage on
  // failure.
  inline void fallibleUnboxPtr(const ValueOperand& src, Register dest,
                               JSValueType type, Label* fail) PER_ARCH;
  inline void fallibleUnboxPtr(const Address& src, Register dest,
                               JSValueType type, Label* fail) PER_ARCH;
  inline void fallibleUnboxPtr(const BaseIndex& src, Register dest,
                               JSValueType type, Label* fail) PER_ARCH;
  template <typename T>
  inline void fallibleUnboxInt32(const T& src, Register dest, Label* fail);
  template <typename T>
  inline void fallibleUnboxBoolean(const T& src, Register dest, Label* fail);
  template <typename T>
  inline void fallibleUnboxObject(const T& src, Register dest, Label* fail);
  template <typename T>
  inline void fallibleUnboxString(const T& src, Register dest, Label* fail);
  template <typename T>
  inline void fallibleUnboxSymbol(const T& src, Register dest, Label* fail);
  template <typename T>
  inline void fallibleUnboxBigInt(const T& src, Register dest, Label* fail);

  inline void cmp32Move32(Condition cond, Register lhs, Register rhs,
                          Register src, Register dest)
      DEFINED_ON(arm, arm64, loong64, riscv64, wasm32, mips_shared, x86_shared);

  inline void cmp32Move32(Condition cond, Register lhs, const Address& rhs,
                          Register src, Register dest)
      DEFINED_ON(arm, arm64, loong64, riscv64, wasm32, mips_shared, x86_shared);

  inline void cmpPtrMovePtr(Condition cond, Register lhs, Register rhs,
                            Register src, Register dest) PER_ARCH;

  inline void cmpPtrMovePtr(Condition cond, Register lhs, const Address& rhs,
                            Register src, Register dest) PER_ARCH;

  inline void cmp32Load32(Condition cond, Register lhs, const Address& rhs,
                          const Address& src, Register dest)
      DEFINED_ON(arm, arm64, loong64, riscv64, mips_shared, x86_shared);

  inline void cmp32Load32(Condition cond, Register lhs, Register rhs,
                          const Address& src, Register dest)
      DEFINED_ON(arm, arm64, loong64, riscv64, mips_shared, x86_shared);

  inline void cmp32LoadPtr(Condition cond, const Address& lhs, Imm32 rhs,
                           const Address& src, Register dest)
      DEFINED_ON(arm, arm64, loong64, riscv64, wasm32, mips_shared, x86, x64);

  inline void cmp32MovePtr(Condition cond, Register lhs, Imm32 rhs,
                           Register src, Register dest)
      DEFINED_ON(arm, arm64, loong64, riscv64, wasm32, mips_shared, x86, x64);

  inline void test32LoadPtr(Condition cond, const Address& addr, Imm32 mask,
                            const Address& src, Register dest)
      DEFINED_ON(arm, arm64, loong64, riscv64, wasm32, mips_shared, x86, x64);

  inline void test32MovePtr(Condition cond, const Address& addr, Imm32 mask,
                            Register src, Register dest)
      DEFINED_ON(arm, arm64, loong64, riscv64, wasm32, mips_shared, x86, x64);

  // Conditional move for Spectre mitigations.
  inline void spectreMovePtr(Condition cond, Register src, Register dest)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);

  // Zeroes dest if the condition is true.
  inline void spectreZeroRegister(Condition cond, Register scratch,
                                  Register dest)
      DEFINED_ON(arm, arm64, mips_shared, x86_shared, loong64, riscv64, wasm32);

  // Performs a bounds check and zeroes the index register if out-of-bounds
  // (to mitigate Spectre).
 private:
  inline void spectreBoundsCheck32(Register index, const Operand& length,
                                   Register maybeScratch, Label* failure)
      DEFINED_ON(x86);

 public:
  inline void spectreBoundsCheck32(Register index, Register length,
                                   Register maybeScratch, Label* failure)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);
  inline void spectreBoundsCheck32(Register index, const Address& length,
                                   Register maybeScratch, Label* failure)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);

  inline void spectreBoundsCheckPtr(Register index, Register length,
                                    Register maybeScratch, Label* failure)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);
  inline void spectreBoundsCheckPtr(Register index, const Address& length,
                                    Register maybeScratch, Label* failure)
      DEFINED_ON(arm, arm64, mips_shared, x86, x64, loong64, riscv64, wasm32);

  // ========================================================================
  // Canonicalization primitives.
  inline void canonicalizeDouble(FloatRegister reg);
  inline void canonicalizeDoubleIfDeterministic(FloatRegister reg);

  inline void canonicalizeFloat(FloatRegister reg);
  inline void canonicalizeFloatIfDeterministic(FloatRegister reg);

 public:
  // ========================================================================
  // Memory access primitives.
  inline void storeUncanonicalizedDouble(FloatRegister src, const Address& dest)
      DEFINED_ON(x86_shared, arm, arm64, mips32, mips64, loong64, riscv64,
                 wasm32);
  inline void storeUncanonicalizedDouble(FloatRegister src,
                                         const BaseIndex& dest)
      DEFINED_ON(x86_shared, arm, arm64, mips32, mips64, loong64, riscv64,
                 wasm32);
  inline void storeUncanonicalizedDouble(FloatRegister src, const Operand& dest)
      DEFINED_ON(x86_shared);

  template <class T>
  inline void storeDouble(FloatRegister src, const T& dest);

  template <class T>
  inline void boxDouble(FloatRegister src, const T& dest);

  using MacroAssemblerSpecific::boxDouble;

  inline void storeUncanonicalizedFloat32(FloatRegister src,
                                          const Address& dest)
      DEFINED_ON(x86_shared, arm, arm64, mips32, mips64, loong64, riscv64,
                 wasm32);
  inline void storeUncanonicalizedFloat32(FloatRegister src,
                                          const BaseIndex& dest)
      DEFINED_ON(x86_shared, arm, arm64, mips32, mips64, loong64, riscv64,
                 wasm32);
  inline void storeUncanonicalizedFloat32(FloatRegister src,
                                          const Operand& dest)
      DEFINED_ON(x86_shared);

  template <class T>
  inline void storeFloat32(FloatRegister src, const T& dest);

  template <typename T>
  void storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                         const T& dest) PER_ARCH;

  inline void memoryBarrier(MemoryBarrierBits barrier) PER_SHARED_ARCH;

 public:
  // ========================================================================
  // Wasm SIMD
  //
  // Naming is "operationSimd128" when operate on the whole vector, otherwise
  // it's "operation<Type><Size>x<Lanes>".
  //
  // For microarchitectural reasons we can in principle get a performance win by
  // using int or float specific instructions in the operationSimd128 case when
  // we know that subsequent operations on the result are int or float oriented.
  // In practice, we don't care about that yet.
  //
  // The order of operations here follows those in the SIMD overview document,
  // https://github.com/WebAssembly/simd/blob/master/proposals/simd/SIMD.md.
  //
  // Since we must target Intel SSE indefinitely and SSE is one-address or
  // two-address, the x86 porting interfaces are nearly all one-address or
  // two-address.  Likewise there are two-address ARM64 interfaces to support
  // the baseline compiler.  But there are also three-address ARM64 interfaces
  // as the ARM64 Ion back-end can use those.  In the future, they may support
  // AVX2 or similar for x86.
  //
  // Conventions for argument order and naming and semantics:
  //  - Condition codes come first.
  //  - Other immediates (masks, shift counts) come next.
  //  - Operands come next:
  //    - For a binary two-address operator where the left-hand-side has the
  //      same type as the result, one register parameter is normally named
  //      `lhsDest` and is both the left-hand side and destination; the other
  //      parameter is named `rhs` and is the right-hand side.  `rhs` comes
  //      first, `lhsDest` second.  `rhs` and `lhsDest` may be the same register
  //      (if rhs is a register).
  //    - For a binary three-address operator the order is `lhs`, `rhs`, `dest`,
  //      and generally these registers may be the same.
  //    - For a unary operator, the input is named `src` and the output is named
  //      `dest`.  `src` comes first, `dest` second.  `src` and `dest` may be
  //      the same register (if `src` is a register).
  //  - Temp registers follow operands and are named `temp` if there's only one,
  //    otherwise `temp1`, `temp2`, etc regardless of type.  GPR temps precede
  //    FPU temps.  If there are several temps then they must be distinct
  //    registers, and they must be distinct from the operand registers unless
  //    noted.

  // Moves

  inline void moveSimd128(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Constants

  inline void loadConstantSimd128(const SimdConstant& v, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Splat

  inline void splatX16(Register src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void splatX16(uint32_t srcLane, FloatRegister src, FloatRegister dest)
      DEFINED_ON(arm64);

  inline void splatX8(Register src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void splatX8(uint32_t srcLane, FloatRegister src, FloatRegister dest)
      DEFINED_ON(arm64);

  inline void splatX4(Register src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void splatX4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void splatX2(Register64 src, FloatRegister dest)
      DEFINED_ON(x86, x64, arm64);

  inline void splatX2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Extract lane as scalar.  Float extraction does not canonicalize the value.

  inline void extractLaneInt8x16(uint32_t lane, FloatRegister src,
                                 Register dest) DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtractLaneInt8x16(uint32_t lane, FloatRegister src,
                                         Register dest)
      DEFINED_ON(x86_shared, arm64);

  inline void extractLaneInt16x8(uint32_t lane, FloatRegister src,
                                 Register dest) DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtractLaneInt16x8(uint32_t lane, FloatRegister src,
                                         Register dest)
      DEFINED_ON(x86_shared, arm64);

  inline void extractLaneInt32x4(uint32_t lane, FloatRegister src,
                                 Register dest) DEFINED_ON(x86_shared, arm64);

  inline void extractLaneInt64x2(uint32_t lane, FloatRegister src,
                                 Register64 dest) DEFINED_ON(x86, x64, arm64);

  inline void extractLaneFloat32x4(uint32_t lane, FloatRegister src,
                                   FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void extractLaneFloat64x2(uint32_t lane, FloatRegister src,
                                   FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Replace lane value

  inline void replaceLaneInt8x16(unsigned lane, FloatRegister lhs, Register rhs,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  inline void replaceLaneInt8x16(unsigned lane, Register rhs,
                                 FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void replaceLaneInt16x8(unsigned lane, FloatRegister lhs, Register rhs,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  inline void replaceLaneInt16x8(unsigned lane, Register rhs,
                                 FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void replaceLaneInt32x4(unsigned lane, FloatRegister lhs, Register rhs,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  inline void replaceLaneInt32x4(unsigned lane, Register rhs,
                                 FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void replaceLaneInt64x2(unsigned lane, FloatRegister lhs,
                                 Register64 rhs, FloatRegister dest)
      DEFINED_ON(x86, x64);

  inline void replaceLaneInt64x2(unsigned lane, Register64 rhs,
                                 FloatRegister lhsDest)
      DEFINED_ON(x86, x64, arm64);

  inline void replaceLaneFloat32x4(unsigned lane, FloatRegister lhs,
                                   FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared);

  inline void replaceLaneFloat32x4(unsigned lane, FloatRegister rhs,
                                   FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void replaceLaneFloat64x2(unsigned lane, FloatRegister lhs,
                                   FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared);

  inline void replaceLaneFloat64x2(unsigned lane, FloatRegister rhs,
                                   FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  // Shuffle - blend and permute with immediate indices, and its many
  // specializations.  Lane values other than those mentioned are illegal.

  // lane values 0..31
  inline void shuffleInt8x16(const uint8_t lanes[16], FloatRegister rhs,
                             FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void shuffleInt8x16(const uint8_t lanes[16], FloatRegister lhs,
                             FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Lane values must be 0 (select from lhs) or FF (select from rhs).
  // The behavior is undefined for lane values that are neither 0 nor FF.
  // on x86_shared: it is required that lhs == dest.
  inline void blendInt8x16(const uint8_t lanes[16], FloatRegister lhs,
                           FloatRegister rhs, FloatRegister dest,
                           FloatRegister temp) DEFINED_ON(x86_shared);

  // Lane values must be 0 (select from lhs) or FF (select from rhs).
  // The behavior is undefined for lane values that are neither 0 nor FF.
  inline void blendInt8x16(const uint8_t lanes[16], FloatRegister lhs,
                           FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(arm64);

  // Lane values must be 0 (select from lhs) or FFFF (select from rhs).
  // The behavior is undefined for lane values that are neither 0 nor FFFF.
  // on x86_shared: it is required that lhs == dest.
  inline void blendInt16x8(const uint16_t lanes[8], FloatRegister lhs,
                           FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Mask lane values must be ~0 or 0. The former selects from lhs and the
  // latter from rhs.
  // The implementation works effectively for I8x16, I16x8, I32x4, and I64x2.
  inline void laneSelectSimd128(FloatRegister mask, FloatRegister lhs,
                                FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void interleaveHighInt8x16(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void interleaveHighInt16x8(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void interleaveHighInt32x4(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void interleaveHighInt64x2(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void interleaveLowInt8x16(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void interleaveLowInt16x8(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void interleaveLowInt32x4(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void interleaveLowInt64x2(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Permute - permute with immediate indices.

  // lane values 0..15
  inline void permuteInt8x16(const uint8_t lanes[16], FloatRegister src,
                             FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  // lane values 0..7
  inline void permuteInt16x8(const uint16_t lanes[8], FloatRegister src,
                             FloatRegister dest) DEFINED_ON(arm64);

  // lane values 0..3 [sic].
  inline void permuteHighInt16x8(const uint16_t lanes[4], FloatRegister src,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  // lane values 0..3.
  inline void permuteLowInt16x8(const uint16_t lanes[4], FloatRegister src,
                                FloatRegister dest) DEFINED_ON(x86_shared);

  // lane values 0..3
  inline void permuteInt32x4(const uint32_t lanes[4], FloatRegister src,
                             FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  // Funnel shift by immediate count:
  //   low_16_bytes_of((lhs ++ rhs) >> shift*8), shift must be < 16
  inline void concatAndRightShiftSimd128(FloatRegister lhs, FloatRegister rhs,
                                         FloatRegister dest, uint32_t shift)
      DEFINED_ON(x86_shared, arm64);

  // Rotate right by immediate count:
  //   low_16_bytes_of((src ++ src) >> shift*8), shift must be < 16
  inline void rotateRightSimd128(FloatRegister src, FloatRegister dest,
                                 uint32_t shift) DEFINED_ON(arm64);

  // Shift bytes with immediate count, shifting in zeroes.  Shift count 0..15.

  inline void leftShiftSimd128(Imm32 count, FloatRegister src,
                               FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void rightShiftSimd128(Imm32 count, FloatRegister src,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Reverse bytes in lanes.

  inline void reverseInt16x8(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void reverseInt32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void reverseInt64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Swizzle - permute with variable indices.  `rhs` holds the lanes parameter.

  inline void swizzleInt8x16(FloatRegister lhs, FloatRegister rhs,
                             FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void swizzleInt8x16Relaxed(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Integer Add

  inline void addInt8x16(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void addInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void addInt16x8(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void addInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void addInt32x4(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void addInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void addInt64x2(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void addInt64x2(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  // Integer Subtract

  inline void subInt8x16(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void subInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void subInt16x8(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void subInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void subInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void subInt32x4(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void subInt64x2(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void subInt64x2(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  // Integer Multiply

  inline void mulInt16x8(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void mulInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void mulInt32x4(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void mulInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  // On x86_shared, it is required lhs == dest
  inline void mulInt64x2(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest, FloatRegister temp)
      DEFINED_ON(x86_shared);

  inline void mulInt64x2(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest, FloatRegister temp)
      DEFINED_ON(x86_shared);

  inline void mulInt64x2(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest, FloatRegister temp1,
                         FloatRegister temp2) DEFINED_ON(arm64);

  // Note for the extMul opcodes, the NxM designation is for the input lanes;
  // the output lanes are twice as wide.
  inline void extMulLowInt8x16(FloatRegister lhs, FloatRegister rhs,
                               FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void extMulHighInt8x16(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtMulLowInt8x16(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtMulHighInt8x16(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void extMulLowInt16x8(FloatRegister lhs, FloatRegister rhs,
                               FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void extMulHighInt16x8(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtMulLowInt16x8(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtMulHighInt16x8(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void extMulLowInt32x4(FloatRegister lhs, FloatRegister rhs,
                               FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void extMulHighInt32x4(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtMulLowInt32x4(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtMulHighInt32x4(FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void q15MulrSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Integer Negate

  inline void negInt8x16(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void negInt16x8(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void negInt32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void negInt64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Saturating integer add

  inline void addSatInt8x16(FloatRegister lhs, FloatRegister rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void addSatInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedAddSatInt8x16(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedAddSatInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                    FloatRegister dest) DEFINED_ON(x86_shared);

  inline void addSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void addSatInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedAddSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedAddSatInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                    FloatRegister dest) DEFINED_ON(x86_shared);

  // Saturating integer subtract

  inline void subSatInt8x16(FloatRegister lhs, FloatRegister rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void subSatInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedSubSatInt8x16(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedSubSatInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                    FloatRegister dest) DEFINED_ON(x86_shared);

  inline void subSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void subSatInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedSubSatInt16x8(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedSubSatInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                    FloatRegister dest) DEFINED_ON(x86_shared);

  // Lane-wise integer minimum

  inline void minInt8x16(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void minInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedMinInt8x16(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedMinInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  inline void minInt16x8(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void minInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedMinInt16x8(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedMinInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  inline void minInt32x4(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void minInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedMinInt32x4(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedMinInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  // Lane-wise integer maximum

  inline void maxInt8x16(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void maxInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedMaxInt8x16(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedMaxInt8x16(FloatRegister lhs, const SimdConstant& rhs,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  inline void maxInt16x8(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void maxInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedMaxInt16x8(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedMaxInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  inline void maxInt32x4(FloatRegister lhs, FloatRegister rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void maxInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                         FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedMaxInt32x4(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedMaxInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                                 FloatRegister dest) DEFINED_ON(x86_shared);

  // Lane-wise integer rounding average

  inline void unsignedAverageInt8x16(FloatRegister lhs, FloatRegister rhs,
                                     FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedAverageInt16x8(FloatRegister lhs, FloatRegister rhs,
                                     FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Lane-wise integer absolute value

  inline void absInt8x16(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void absInt16x8(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void absInt32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void absInt64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Left shift by scalar. Immediates and variable shifts must have been
  // masked; shifts of zero will work but may or may not generate code.

  inline void leftShiftInt8x16(Register rhs, FloatRegister lhsDest,
                               FloatRegister temp) DEFINED_ON(x86_shared);

  inline void leftShiftInt8x16(FloatRegister lhs, Register rhs,
                               FloatRegister dest) DEFINED_ON(arm64);

  inline void leftShiftInt8x16(Imm32 count, FloatRegister src,
                               FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void leftShiftInt16x8(Register rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared);

  inline void leftShiftInt16x8(FloatRegister lhs, Register rhs,
                               FloatRegister dest) DEFINED_ON(arm64);

  inline void leftShiftInt16x8(Imm32 count, FloatRegister src,
                               FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void leftShiftInt32x4(Register rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared);

  inline void leftShiftInt32x4(FloatRegister lhs, Register rhs,
                               FloatRegister dest) DEFINED_ON(arm64);

  inline void leftShiftInt32x4(Imm32 count, FloatRegister src,
                               FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void leftShiftInt64x2(Register rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared);

  inline void leftShiftInt64x2(FloatRegister lhs, Register rhs,
                               FloatRegister dest) DEFINED_ON(arm64);

  inline void leftShiftInt64x2(Imm32 count, FloatRegister src,
                               FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Right shift by scalar. Immediates and variable shifts must have been
  // masked; shifts of zero will work but may or may not generate code.

  inline void rightShiftInt8x16(Register rhs, FloatRegister lhsDest,
                                FloatRegister temp) DEFINED_ON(x86_shared);

  inline void rightShiftInt8x16(FloatRegister lhs, Register rhs,
                                FloatRegister dest) DEFINED_ON(arm64);

  inline void rightShiftInt8x16(Imm32 count, FloatRegister src,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedRightShiftInt8x16(Register rhs, FloatRegister lhsDest,
                                        FloatRegister temp)
      DEFINED_ON(x86_shared);

  inline void unsignedRightShiftInt8x16(FloatRegister lhs, Register rhs,
                                        FloatRegister dest) DEFINED_ON(arm64);

  inline void unsignedRightShiftInt8x16(Imm32 count, FloatRegister src,
                                        FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void rightShiftInt16x8(Register rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared);

  inline void rightShiftInt16x8(FloatRegister lhs, Register rhs,
                                FloatRegister dest) DEFINED_ON(arm64);

  inline void rightShiftInt16x8(Imm32 count, FloatRegister src,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedRightShiftInt16x8(Register rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared);

  inline void unsignedRightShiftInt16x8(FloatRegister lhs, Register rhs,
                                        FloatRegister dest) DEFINED_ON(arm64);

  inline void unsignedRightShiftInt16x8(Imm32 count, FloatRegister src,
                                        FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void rightShiftInt32x4(Register rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared);

  inline void rightShiftInt32x4(FloatRegister lhs, Register rhs,
                                FloatRegister dest) DEFINED_ON(arm64);

  inline void rightShiftInt32x4(Imm32 count, FloatRegister src,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedRightShiftInt32x4(Register rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared);

  inline void unsignedRightShiftInt32x4(FloatRegister lhs, Register rhs,
                                        FloatRegister dest) DEFINED_ON(arm64);

  inline void unsignedRightShiftInt32x4(Imm32 count, FloatRegister src,
                                        FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void rightShiftInt64x2(Register rhs, FloatRegister lhsDest,
                                FloatRegister temp) DEFINED_ON(x86_shared);

  inline void rightShiftInt64x2(Imm32 count, FloatRegister src,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void rightShiftInt64x2(FloatRegister lhs, Register rhs,
                                FloatRegister dest) DEFINED_ON(arm64);

  inline void unsignedRightShiftInt64x2(Register rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared);

  inline void unsignedRightShiftInt64x2(FloatRegister lhs, Register rhs,
                                        FloatRegister dest) DEFINED_ON(arm64);

  inline void unsignedRightShiftInt64x2(Imm32 count, FloatRegister src,
                                        FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Sign replication operation

  inline void signReplicationInt8x16(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared);

  inline void signReplicationInt16x8(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared);

  inline void signReplicationInt32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared);

  inline void signReplicationInt64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared);

  // Bitwise and, or, xor, not

  inline void bitwiseAndSimd128(FloatRegister rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void bitwiseAndSimd128(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void bitwiseAndSimd128(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) DEFINED_ON(x86_shared);

  inline void bitwiseOrSimd128(FloatRegister rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void bitwiseOrSimd128(FloatRegister lhs, FloatRegister rhs,
                               FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void bitwiseOrSimd128(FloatRegister lhs, const SimdConstant& rhs,
                               FloatRegister dest) DEFINED_ON(x86_shared);

  inline void bitwiseXorSimd128(FloatRegister rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void bitwiseXorSimd128(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void bitwiseXorSimd128(FloatRegister lhs, const SimdConstant& rhs,
                                FloatRegister dest) DEFINED_ON(x86_shared);

  inline void bitwiseNotSimd128(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Bitwise AND with compliment: dest = lhs & ~rhs, note only arm64 can do it.
  inline void bitwiseAndNotSimd128(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister lhsDest) DEFINED_ON(arm64);

  // Bitwise AND with complement: dest = ~lhs & rhs, note this is not what Wasm
  // wants but what the x86 hardware offers.  Hence the name.

  inline void bitwiseNotAndSimd128(FloatRegister rhs, FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void bitwiseNotAndSimd128(FloatRegister lhs, FloatRegister rhs,
                                   FloatRegister lhsDest)
      DEFINED_ON(x86_shared);

  // Bitwise select

  inline void bitwiseSelectSimd128(FloatRegister mask, FloatRegister onTrue,
                                   FloatRegister onFalse, FloatRegister dest,
                                   FloatRegister temp) DEFINED_ON(x86_shared);

  inline void bitwiseSelectSimd128(FloatRegister onTrue, FloatRegister onFalse,
                                   FloatRegister maskDest) DEFINED_ON(arm64);

  // Population count

  inline void popcntInt8x16(FloatRegister src, FloatRegister dest,
                            FloatRegister temp) DEFINED_ON(x86_shared);

  inline void popcntInt8x16(FloatRegister src, FloatRegister dest)
      DEFINED_ON(arm64);

  // Any lane true, ie, any bit set

  inline void anyTrueSimd128(FloatRegister src, Register dest)
      DEFINED_ON(x86_shared, arm64);

  // All lanes true

  inline void allTrueInt8x16(FloatRegister src, Register dest)
      DEFINED_ON(x86_shared, arm64);

  inline void allTrueInt16x8(FloatRegister src, Register dest)
      DEFINED_ON(x86_shared, arm64);

  inline void allTrueInt32x4(FloatRegister src, Register dest)
      DEFINED_ON(x86_shared, arm64);

  inline void allTrueInt64x2(FloatRegister src, Register dest)
      DEFINED_ON(x86_shared, arm64);

  // Bitmask, ie extract and compress high bits of all lanes

  inline void bitmaskInt8x16(FloatRegister src, Register dest)
      DEFINED_ON(x86_shared);

  inline void bitmaskInt8x16(FloatRegister src, Register dest,
                             FloatRegister temp) DEFINED_ON(arm64);

  inline void bitmaskInt16x8(FloatRegister src, Register dest)
      DEFINED_ON(x86_shared);

  inline void bitmaskInt16x8(FloatRegister src, Register dest,
                             FloatRegister temp) DEFINED_ON(arm64);

  inline void bitmaskInt32x4(FloatRegister src, Register dest)
      DEFINED_ON(x86_shared);

  inline void bitmaskInt32x4(FloatRegister src, Register dest,
                             FloatRegister temp) DEFINED_ON(arm64);

  inline void bitmaskInt64x2(FloatRegister src, Register dest)
      DEFINED_ON(x86_shared);

  inline void bitmaskInt64x2(FloatRegister src, Register dest,
                             FloatRegister temp) DEFINED_ON(arm64);

  // Comparisons (integer and floating-point)

  inline void compareInt8x16(Assembler::Condition cond, FloatRegister rhs,
                             FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  // On x86_shared, limited to !=, ==, <=, >
  inline void compareInt8x16(Assembler::Condition cond, FloatRegister lhs,
                             const SimdConstant& rhs, FloatRegister dest)
      DEFINED_ON(x86_shared);

  // On arm64, use any integer comparison condition.
  inline void compareInt8x16(Assembler::Condition cond, FloatRegister lhs,
                             FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void compareInt16x8(Assembler::Condition cond, FloatRegister rhs,
                             FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void compareInt16x8(Assembler::Condition cond, FloatRegister lhs,
                             FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // On x86_shared, limited to !=, ==, <=, >
  inline void compareInt16x8(Assembler::Condition cond, FloatRegister lhs,
                             const SimdConstant& rhs, FloatRegister dest)
      DEFINED_ON(x86_shared);

  // On x86_shared, limited to !=, ==, <=, >
  inline void compareInt32x4(Assembler::Condition cond, FloatRegister rhs,
                             FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void compareInt32x4(Assembler::Condition cond, FloatRegister lhs,
                             const SimdConstant& rhs, FloatRegister dest)
      DEFINED_ON(x86_shared);

  // On arm64, use any integer comparison condition.
  inline void compareInt32x4(Assembler::Condition cond, FloatRegister lhs,
                             FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void compareForEqualityInt64x2(Assembler::Condition cond,
                                        FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest)
      DEFINED_ON(x86_shared);

  inline void compareForOrderingInt64x2(Assembler::Condition cond,
                                        FloatRegister lhs, FloatRegister rhs,
                                        FloatRegister dest, FloatRegister temp1,
                                        FloatRegister temp2)
      DEFINED_ON(x86_shared);

  inline void compareInt64x2(Assembler::Condition cond, FloatRegister rhs,
                             FloatRegister lhsDest) DEFINED_ON(arm64);

  inline void compareInt64x2(Assembler::Condition cond, FloatRegister lhs,
                             FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(arm64);

  inline void compareFloat32x4(Assembler::Condition cond, FloatRegister rhs,
                               FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  // On x86_shared, limited to ==, !=, <, <=
  inline void compareFloat32x4(Assembler::Condition cond, FloatRegister lhs,
                               const SimdConstant& rhs, FloatRegister dest)
      DEFINED_ON(x86_shared);

  // On x86_shared, limited to ==, !=, <, <=
  // On arm64, use any float-point comparison condition.
  inline void compareFloat32x4(Assembler::Condition cond, FloatRegister lhs,
                               FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void compareFloat64x2(Assembler::Condition cond, FloatRegister rhs,
                               FloatRegister lhsDest)
      DEFINED_ON(x86_shared, arm64);

  // On x86_shared, limited to ==, !=, <, <=
  inline void compareFloat64x2(Assembler::Condition cond, FloatRegister lhs,
                               const SimdConstant& rhs, FloatRegister dest)
      DEFINED_ON(x86_shared);

  // On x86_shared, limited to ==, !=, <, <=
  // On arm64, use any float-point comparison condition.
  inline void compareFloat64x2(Assembler::Condition cond, FloatRegister lhs,
                               FloatRegister rhs, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Load

  inline void loadUnalignedSimd128(const Operand& src, FloatRegister dest)
      DEFINED_ON(x86_shared);

  inline void loadUnalignedSimd128(const Address& src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void loadUnalignedSimd128(const BaseIndex& src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Store

  inline void storeUnalignedSimd128(FloatRegister src, const Address& dest)
      DEFINED_ON(x86_shared, arm64);

  inline void storeUnalignedSimd128(FloatRegister src, const BaseIndex& dest)
      DEFINED_ON(x86_shared, arm64);

  // Floating point negation

  inline void negFloat32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void negFloat64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Floating point absolute value

  inline void absFloat32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void absFloat64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // NaN-propagating minimum

  inline void minFloat32x4(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest, FloatRegister temp1,
                           FloatRegister temp2) DEFINED_ON(x86_shared);

  inline void minFloat32x4(FloatRegister rhs, FloatRegister lhsDest)
      DEFINED_ON(arm64);

  inline void minFloat32x4(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(arm64);

  inline void minFloat64x2(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest, FloatRegister temp1,
                           FloatRegister temp2) DEFINED_ON(x86_shared);

  inline void minFloat64x2(FloatRegister rhs, FloatRegister lhsDest)
      DEFINED_ON(arm64);

  inline void minFloat64x2(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(arm64);

  // NaN-propagating maximum

  inline void maxFloat32x4(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest, FloatRegister temp1,
                           FloatRegister temp2) DEFINED_ON(x86_shared);

  inline void maxFloat32x4(FloatRegister rhs, FloatRegister lhsDest)
      DEFINED_ON(arm64);

  inline void maxFloat32x4(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(arm64);

  inline void maxFloat64x2(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest, FloatRegister temp1,
                           FloatRegister temp2) DEFINED_ON(x86_shared);

  inline void maxFloat64x2(FloatRegister rhs, FloatRegister lhsDest)
      DEFINED_ON(arm64);

  inline void maxFloat64x2(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(arm64);

  // Floating add

  inline void addFloat32x4(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void addFloat32x4(FloatRegister lhs, const SimdConstant& rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared);

  inline void addFloat64x2(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void addFloat64x2(FloatRegister lhs, const SimdConstant& rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared);

  // Floating subtract

  inline void subFloat32x4(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void subFloat32x4(FloatRegister lhs, const SimdConstant& rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared);

  inline void subFloat64x2(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void subFloat64x2(FloatRegister lhs, const SimdConstant& rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared);

  // Floating division

  inline void divFloat32x4(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void divFloat32x4(FloatRegister lhs, const SimdConstant& rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared);

  inline void divFloat64x2(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void divFloat64x2(FloatRegister lhs, const SimdConstant& rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared);

  // Floating Multiply

  inline void mulFloat32x4(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void mulFloat32x4(FloatRegister lhs, const SimdConstant& rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared);

  inline void mulFloat64x2(FloatRegister lhs, FloatRegister rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void mulFloat64x2(FloatRegister lhs, const SimdConstant& rhs,
                           FloatRegister dest) DEFINED_ON(x86_shared);

  // Pairwise add

  inline void extAddPairwiseInt8x16(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtAddPairwiseInt8x16(FloatRegister src,
                                            FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void extAddPairwiseInt16x8(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedExtAddPairwiseInt16x8(FloatRegister src,
                                            FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Floating square root

  inline void sqrtFloat32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void sqrtFloat64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Integer to floating point with rounding

  inline void convertInt32x4ToFloat32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedConvertInt32x4ToFloat32x4(FloatRegister src,
                                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void convertInt32x4ToFloat64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedConvertInt32x4ToFloat64x2(FloatRegister src,
                                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Floating point to integer with saturation

  inline void truncSatFloat32x4ToInt32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedTruncSatFloat32x4ToInt32x4(FloatRegister src,
                                                 FloatRegister dest,
                                                 FloatRegister temp)
      DEFINED_ON(x86_shared);

  inline void unsignedTruncSatFloat32x4ToInt32x4(FloatRegister src,
                                                 FloatRegister dest)
      DEFINED_ON(arm64);

  inline void truncSatFloat64x2ToInt32x4(FloatRegister src, FloatRegister dest,
                                         FloatRegister temp)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedTruncSatFloat64x2ToInt32x4(FloatRegister src,
                                                 FloatRegister dest,
                                                 FloatRegister temp)
      DEFINED_ON(x86_shared, arm64);

  inline void truncFloat32x4ToInt32x4Relaxed(FloatRegister src,
                                             FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedTruncFloat32x4ToInt32x4Relaxed(FloatRegister src,
                                                     FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void truncFloat64x2ToInt32x4Relaxed(FloatRegister src,
                                             FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedTruncFloat64x2ToInt32x4Relaxed(FloatRegister src,
                                                     FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Floating point narrowing

  inline void convertFloat64x2ToFloat32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Floating point widening

  inline void convertFloat32x4ToFloat64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Integer to integer narrowing

  inline void narrowInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared);

  inline void narrowInt16x8(FloatRegister lhs, FloatRegister rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void unsignedNarrowInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                                    FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedNarrowInt16x8(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void narrowInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared);

  inline void narrowInt32x4(FloatRegister lhs, FloatRegister rhs,
                            FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void unsignedNarrowInt32x4(FloatRegister lhs, const SimdConstant& rhs,
                                    FloatRegister dest) DEFINED_ON(x86_shared);

  inline void unsignedNarrowInt32x4(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Integer to integer widening

  inline void widenLowInt8x16(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void widenHighInt8x16(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedWidenLowInt8x16(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedWidenHighInt8x16(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void widenLowInt16x8(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void widenHighInt16x8(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedWidenLowInt16x8(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedWidenHighInt16x8(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void widenLowInt32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedWidenLowInt32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void widenHighInt32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void unsignedWidenHighInt32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Compare-based minimum/maximum
  //
  // On x86, the signature is (rhsDest, lhs); on arm64 it is (rhs, lhsDest).
  //
  // The masm preprocessor can't deal with multiple declarations with identical
  // signatures even if they are on different platforms, hence the weird
  // argument names.

  inline void pseudoMinFloat32x4(FloatRegister rhsOrRhsDest,
                                 FloatRegister lhsOrLhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void pseudoMinFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void pseudoMinFloat64x2(FloatRegister rhsOrRhsDest,
                                 FloatRegister lhsOrLhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void pseudoMinFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void pseudoMaxFloat32x4(FloatRegister rhsOrRhsDest,
                                 FloatRegister lhsOrLhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void pseudoMaxFloat32x4(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void pseudoMaxFloat64x2(FloatRegister rhsOrRhsDest,
                                 FloatRegister lhsOrLhsDest)
      DEFINED_ON(x86_shared, arm64);

  inline void pseudoMaxFloat64x2(FloatRegister lhs, FloatRegister rhs,
                                 FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Widening/pairwise integer dot product

  inline void widenDotInt16x8(FloatRegister lhs, FloatRegister rhs,
                              FloatRegister dest) DEFINED_ON(x86_shared, arm64);

  inline void widenDotInt16x8(FloatRegister lhs, const SimdConstant& rhs,
                              FloatRegister dest) DEFINED_ON(x86_shared);

  inline void dotInt8x16Int7x16(FloatRegister lhs, FloatRegister rhs,
                                FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void dotInt8x16Int7x16ThenAdd(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest)
      DEFINED_ON(x86_shared);

  inline void dotInt8x16Int7x16ThenAdd(FloatRegister lhs, FloatRegister rhs,
                                       FloatRegister dest, FloatRegister temp)
      DEFINED_ON(arm64);

  // Floating point rounding

  inline void ceilFloat32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void ceilFloat64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void floorFloat32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void floorFloat64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void truncFloat32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void truncFloat64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void nearestFloat32x4(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void nearestFloat64x2(FloatRegister src, FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  // Floating multiply-accumulate: srcDest [+-]= src1 * src2

  inline void fmaFloat32x4(FloatRegister src1, FloatRegister src2,
                           FloatRegister srcDest) DEFINED_ON(x86_shared, arm64);

  inline void fnmaFloat32x4(FloatRegister src1, FloatRegister src2,
                            FloatRegister srcDest)
      DEFINED_ON(x86_shared, arm64);

  inline void fmaFloat64x2(FloatRegister src1, FloatRegister src2,
                           FloatRegister srcDest) DEFINED_ON(x86_shared, arm64);

  inline void fnmaFloat64x2(FloatRegister src1, FloatRegister src2,
                            FloatRegister srcDest)
      DEFINED_ON(x86_shared, arm64);

  inline void minFloat32x4Relaxed(FloatRegister src, FloatRegister srcDest)
      DEFINED_ON(x86_shared, arm64);

  inline void minFloat32x4Relaxed(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void maxFloat32x4Relaxed(FloatRegister src, FloatRegister srcDest)
      DEFINED_ON(x86_shared, arm64);

  inline void maxFloat32x4Relaxed(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void minFloat64x2Relaxed(FloatRegister src, FloatRegister srcDest)
      DEFINED_ON(x86_shared, arm64);

  inline void minFloat64x2Relaxed(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void maxFloat64x2Relaxed(FloatRegister src, FloatRegister srcDest)
      DEFINED_ON(x86_shared, arm64);

  inline void maxFloat64x2Relaxed(FloatRegister lhs, FloatRegister rhs,
                                  FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

  inline void q15MulrInt16x8Relaxed(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister dest)
      DEFINED_ON(x86_shared, arm64);

 public:
  // ========================================================================
  // Truncate floating point.

  // Undefined behaviour when truncation is outside Int64 range.
  // Needs a temp register if SSE3 is not present.
  inline void truncateFloat32ToInt64(Address src, Address dest, Register temp)
      DEFINED_ON(x86_shared);
  inline void truncateFloat32ToUInt64(Address src, Address dest, Register temp,
                                      FloatRegister floatTemp)
      DEFINED_ON(x86, x64);
  inline void truncateDoubleToInt64(Address src, Address dest, Register temp)
      DEFINED_ON(x86_shared);
  inline void truncateDoubleToUInt64(Address src, Address dest, Register temp,
                                     FloatRegister floatTemp)
      DEFINED_ON(x86, x64);

 public:
  // ========================================================================
  // Convert floating point.

  // temp required on x86 and x64; must be undefined on mips64 and loong64.
  void convertUInt64ToFloat32(Register64 src, FloatRegister dest, Register temp)
      DEFINED_ON(arm64, mips64, loong64, riscv64, wasm32, x64, x86);

  void convertInt64ToFloat32(Register64 src, FloatRegister dest)
      DEFINED_ON(arm64, mips64, loong64, riscv64, wasm32, x64, x86);

  bool convertUInt64ToDoubleNeedsTemp() PER_ARCH;

  // temp required when convertUInt64ToDoubleNeedsTemp() returns true.
  void convertUInt64ToDouble(Register64 src, FloatRegister dest,
                             Register temp) PER_ARCH;

  void convertInt64ToDouble(Register64 src, FloatRegister dest) PER_ARCH;

  void convertIntPtrToDouble(Register src, FloatRegister dest) PER_ARCH;

 public:
  // ========================================================================
  // wasm support

  CodeOffset wasmTrapInstruction() PER_SHARED_ARCH;

  void wasmTrap(wasm::Trap trap, wasm::BytecodeOffset bytecodeOffset);

  // Load all pinned regs via InstanceReg.  If the trapOffset is something,
  // give the first load a trap descriptor with type IndirectCallToNull, so that
  // a null instance will cause a trap.
  void loadWasmPinnedRegsFromInstance(
      mozilla::Maybe<wasm::BytecodeOffset> trapOffset = mozilla::Nothing());

  // Returns a pair: the offset of the undefined (trapping) instruction, and
  // the number of extra bytes of stack allocated prior to the trap
  // instruction proper.
  std::pair<CodeOffset, uint32_t> wasmReserveStackChecked(
      uint32_t amount, wasm::BytecodeOffset trapOffset);

  // Emit a bounds check against the wasm heap limit, jumping to 'ok' if 'cond'
  // holds; this can be the label either of the access or of the trap.  The
  // label should name a code position greater than the position of the bounds
  // check.
  //
  // If JitOptions.spectreMaskIndex is true, a no-op speculation barrier is
  // emitted in the code stream after the check to prevent an OOB access from
  // being executed speculatively.  (On current tier-1 platforms the barrier is
  // a conditional saturation of 'index' to 'boundsCheckLimit', using the same
  // condition as the check.)  If the condition is such that the bounds check
  // branches out of line to the trap, the barrier will actually be executed
  // when the bounds check passes.
  //
  // On 32-bit systems for both wasm and asm.js, and on 64-bit systems for
  // asm.js, heap lengths are limited to 2GB.  On 64-bit systems for wasm,
  // 32-bit heap lengths are limited to 4GB, and 64-bit heap lengths will be
  // limited to something much larger.

  void wasmBoundsCheck32(Condition cond, Register index,
                         Register boundsCheckLimit, Label* ok)
      DEFINED_ON(arm, arm64, mips32, mips64, x86_shared, loong64, riscv64,
                 wasm32);

  void wasmBoundsCheck32(Condition cond, Register index,
                         Address boundsCheckLimit, Label* ok)
      DEFINED_ON(arm, arm64, mips32, mips64, x86_shared, loong64, riscv64,
                 wasm32);

  void wasmBoundsCheck64(Condition cond, Register64 index,
                         Register64 boundsCheckLimit, Label* ok)
      DEFINED_ON(arm64, mips64, x64, x86, arm, loong64, riscv64, wasm32);

  void wasmBoundsCheck64(Condition cond, Register64 index,
                         Address boundsCheckLimit, Label* ok)
      DEFINED_ON(arm64, mips64, x64, x86, arm, loong64, riscv64, wasm32);

  // Each wasm load/store instruction appends its own wasm::Trap::OutOfBounds.
  void wasmLoad(const wasm::MemoryAccessDesc& access, Operand srcAddr,
                AnyRegister out) DEFINED_ON(x86, x64);
  void wasmLoadI64(const wasm::MemoryAccessDesc& access, Operand srcAddr,
                   Register64 out) DEFINED_ON(x86, x64);
  void wasmStore(const wasm::MemoryAccessDesc& access, AnyRegister value,
                 Operand dstAddr) DEFINED_ON(x86, x64);
  void wasmStoreI64(const wasm::MemoryAccessDesc& access, Register64 value,
                    Operand dstAddr) DEFINED_ON(x86);

  // For all the ARM/MIPS/LOONG64 wasmLoad and wasmStore functions below, `ptr`
  // MUST equal `ptrScratch`, and that register will be updated based on
  // conditions listed below (where it is only mentioned as `ptr`).

  // `ptr` will be updated if access.offset() != 0 or access.type() ==
  // Scalar::Int64.
  void wasmLoad(const wasm::MemoryAccessDesc& access, Register memoryBase,
                Register ptr, Register ptrScratch, AnyRegister output)
      DEFINED_ON(arm, loong64, riscv64, mips_shared);
  void wasmLoadI64(const wasm::MemoryAccessDesc& access, Register memoryBase,
                   Register ptr, Register ptrScratch, Register64 output)
      DEFINED_ON(arm, mips32, mips64, loong64, riscv64);
  void wasmStore(const wasm::MemoryAccessDesc& access, AnyRegister value,
                 Register memoryBase, Register ptr, Register ptrScratch)
      DEFINED_ON(arm, loong64, riscv64, mips_shared);
  void wasmStoreI64(const wasm::MemoryAccessDesc& access, Register64 value,
                    Register memoryBase, Register ptr, Register ptrScratch)
      DEFINED_ON(arm, mips32, mips64, loong64, riscv64);

  // These accept general memoryBase + ptr + offset (in `access`); the offset is
  // always smaller than the guard region.  They will insert an additional add
  // if the offset is nonzero, and of course that add may require a temporary
  // register for the offset if the offset is large, and instructions to set it
  // up.
  void wasmLoad(const wasm::MemoryAccessDesc& access, Register memoryBase,
                Register ptr, AnyRegister output) DEFINED_ON(arm64);
  void wasmLoadI64(const wasm::MemoryAccessDesc& access, Register memoryBase,
                   Register ptr, Register64 output) DEFINED_ON(arm64);
  void wasmStore(const wasm::MemoryAccessDesc& access, AnyRegister value,
                 Register memoryBase, Register ptr) DEFINED_ON(arm64);
  void wasmStoreI64(const wasm::MemoryAccessDesc& access, Register64 value,
                    Register memoryBase, Register ptr) DEFINED_ON(arm64);

  // `ptr` will always be updated.
  void wasmUnalignedLoad(const wasm::MemoryAccessDesc& access,
                         Register memoryBase, Register ptr, Register ptrScratch,
                         Register output, Register tmp)
      DEFINED_ON(mips32, mips64);

  // MIPS: `ptr` will always be updated.
  void wasmUnalignedLoadFP(const wasm::MemoryAccessDesc& access,
                           Register memoryBase, Register ptr,
                           Register ptrScratch, FloatRegister output,
                           Register tmp1) DEFINED_ON(mips32, mips64);

  // `ptr` will always be updated.
  void wasmUnalignedLoadI64(const wasm::MemoryAccessDesc& access,
                            Register memoryBase, Register ptr,
                            Register ptrScratch, Register64 output,
                            Register tmp) DEFINED_ON(mips32, mips64);

  // MIPS: `ptr` will always be updated.
  void wasmUnalignedStore(const wasm::MemoryAccessDesc& access, Register value,
                          Register memoryBase, Register ptr,
                          Register ptrScratch, Register tmp)
      DEFINED_ON(mips32, mips64);

  // `ptr` will always be updated.
  void wasmUnalignedStoreFP(const wasm::MemoryAccessDesc& access,
                            FloatRegister floatValue, Register memoryBase,
                            Register ptr, Register ptrScratch, Register tmp)
      DEFINED_ON(mips32, mips64);

  // `ptr` will always be updated.
  void wasmUnalignedStoreI64(const wasm::MemoryAccessDesc& access,
                             Register64 value, Register memoryBase,
                             Register ptr, Register ptrScratch, Register tmp)
      DEFINED_ON(mips32, mips64);

  // wasm specific methods, used in both the wasm baseline compiler and ion.

  // The truncate-to-int32 methods do not bind the rejoin label; clients must
  // do so if oolWasmTruncateCheckF64ToI32() can jump to it.
  void wasmTruncateDoubleToUInt32(FloatRegister input, Register output,
                                  bool isSaturating, Label* oolEntry) PER_ARCH;
  void wasmTruncateDoubleToInt32(FloatRegister input, Register output,
                                 bool isSaturating,
                                 Label* oolEntry) PER_SHARED_ARCH;
  void oolWasmTruncateCheckF64ToI32(FloatRegister input, Register output,
                                    TruncFlags flags, wasm::BytecodeOffset off,
                                    Label* rejoin)
      DEFINED_ON(arm, arm64, x86_shared, mips_shared, loong64, riscv64, wasm32);

  void wasmTruncateFloat32ToUInt32(FloatRegister input, Register output,
                                   bool isSaturating, Label* oolEntry) PER_ARCH;
  void wasmTruncateFloat32ToInt32(FloatRegister input, Register output,
                                  bool isSaturating,
                                  Label* oolEntry) PER_SHARED_ARCH;
  void oolWasmTruncateCheckF32ToI32(FloatRegister input, Register output,
                                    TruncFlags flags, wasm::BytecodeOffset off,
                                    Label* rejoin)
      DEFINED_ON(arm, arm64, x86_shared, mips_shared, loong64, riscv64, wasm32);

  // The truncate-to-int64 methods will always bind the `oolRejoin` label
  // after the last emitted instruction.
  void wasmTruncateDoubleToInt64(FloatRegister input, Register64 output,
                                 bool isSaturating, Label* oolEntry,
                                 Label* oolRejoin, FloatRegister tempDouble)
      DEFINED_ON(arm64, x86, x64, mips64, loong64, riscv64, wasm32);
  void wasmTruncateDoubleToUInt64(FloatRegister input, Register64 output,
                                  bool isSaturating, Label* oolEntry,
                                  Label* oolRejoin, FloatRegister tempDouble)
      DEFINED_ON(arm64, x86, x64, mips64, loong64, riscv64, wasm32);
  void oolWasmTruncateCheckF64ToI64(FloatRegister input, Register64 output,
                                    TruncFlags flags, wasm::BytecodeOffset off,
                                    Label* rejoin)
      DEFINED_ON(arm, arm64, x86_shared, mips_shared, loong64, riscv64, wasm32);

  void wasmTruncateFloat32ToInt64(FloatRegister input, Register64 output,
                                  bool isSaturating, Label* oolEntry,
                                  Label* oolRejoin, FloatRegister tempDouble)
      DEFINED_ON(arm64, x86, x64, mips64, loong64, riscv64, wasm32);
  void wasmTruncateFloat32ToUInt64(FloatRegister input, Register64 output,
                                   bool isSaturating, Label* oolEntry,
                                   Label* oolRejoin, FloatRegister tempDouble)
      DEFINED_ON(arm64, x86, x64, mips64, loong64, riscv64, wasm32);
  void oolWasmTruncateCheckF32ToI64(FloatRegister input, Register64 output,
                                    TruncFlags flags, wasm::BytecodeOffset off,
                                    Label* rejoin)
      DEFINED_ON(arm, arm64, x86_shared, mips_shared, loong64, riscv64, wasm32);

  // This function takes care of loading the callee's instance and pinned regs
  // but it is the caller's responsibility to save/restore instance or pinned
  // regs.
  CodeOffset wasmCallImport(const wasm::CallSiteDesc& desc,
                            const wasm::CalleeDesc& callee);

  // WasmTableCallIndexReg must contain the index of the indirect call.  This is
  // for wasm calls only.
  //
  // Indirect calls use a dual-path mechanism where a run-time test determines
  // whether a context switch is needed (slow path) or not (fast path).  This
  // gives rise to two call instructions, both of which need safe points.  As
  // per normal, the call offsets are the code offsets at the end of the call
  // instructions (the return points).
  //
  // `boundsCheckFailedLabel` is non-null iff a bounds check is required.
  // `nullCheckFailedLabel` is non-null only on platforms that can't fold the
  // null check into the rest of the call instructions.
  void wasmCallIndirect(const wasm::CallSiteDesc& desc,
                        const wasm::CalleeDesc& callee,
                        Label* boundsCheckFailedLabel,
                        Label* nullCheckFailedLabel,
                        mozilla::Maybe<uint32_t> tableSize,
                        CodeOffset* fastCallOffset, CodeOffset* slowCallOffset);

  // This function takes care of loading the callee's instance and address from
  // pinned reg.
  void wasmCallRef(const wasm::CallSiteDesc& desc,
                   const wasm::CalleeDesc& callee, CodeOffset* fastCallOffset,
                   CodeOffset* slowCallOffset);

  // WasmTableCallIndexReg must contain the index of the indirect call.
  // This is for asm.js calls only.
  CodeOffset asmCallIndirect(const wasm::CallSiteDesc& desc,
                             const wasm::CalleeDesc& callee);

  // This function takes care of loading the pointer to the current instance
  // as the implicit first argument. It preserves instance and pinned registers.
  // (instance & pinned regs are non-volatile registers in the system ABI).
  CodeOffset wasmCallBuiltinInstanceMethod(const wasm::CallSiteDesc& desc,
                                           const ABIArg& instanceArg,
                                           wasm::SymbolicAddress builtin,
                                           wasm::FailureMode failureMode);

  // Perform a subtype check that `object` is a subtype of `type`, branching to
  // `label` depending on `onSuccess`. `type` must be in the `any` hierarchy.
  //
  // `superSuperTypeVector` is required iff the destination type is a concrete
  // type. `scratch1` is required iff the destination type is eq or lower and
  // not none. `scratch2` is required iff the destination type is a concrete
  // type and its `subTypingDepth` is >= wasm::MinSuperTypeVectorLength.
  //
  // `object` and `superSuperTypeVector` are preserved. Scratch registers are
  // clobbered.
  void branchWasmGcObjectIsRefType(Register object, wasm::RefType sourceType,
                                   wasm::RefType destType, Label* label,
                                   bool onSuccess,
                                   Register superSuperTypeVector,
                                   Register scratch1, Register scratch2);
  static bool needScratch1ForBranchWasmGcRefType(wasm::RefType type);
  static bool needScratch2ForBranchWasmGcRefType(wasm::RefType type);
  static bool needSuperSuperTypeVectorForBranchWasmGcRefType(
      wasm::RefType type);

  // Perform a subtype check that `subSuperTypeVector` is a subtype of
  // `superSuperTypeVector`, branching to `label` depending on `onSuccess`.
  // This method is a specialization of the general
  // `wasm::TypeDef::isSubTypeOf` method for the case where the
  // `superSuperTypeVector` is statically known, which is the case for all
  // wasm instructions.
  //
  // `scratch` is required iff the `subTypeDepth` is >=
  // wasm::MinSuperTypeVectorLength. `subSuperTypeVector` is clobbered by this
  // method.  `superSuperTypeVector` is preserved.
  void branchWasmSuperTypeVectorIsSubtype(Register subSuperTypeVector,
                                          Register superSuperTypeVector,
                                          Register scratch,
                                          uint32_t superTypeDepth, Label* label,
                                          bool onSuccess);

  // Compute ptr += (indexTemp32 << shift) where shift can be any value < 32.
  // May destroy indexTemp32.  The value of indexTemp32 must be positive, and it
  // is implementation-defined what happens if bits are lost or the value
  // becomes negative through the shift.  On 64-bit systems, the high 32 bits of
  // indexTemp32 must be zero, not garbage.
  void shiftIndex32AndAdd(Register indexTemp32, int shift,
                          Register pointer) PER_SHARED_ARCH;

  // The System ABI frequently states that the high bits of a 64-bit register
  // that holds a 32-bit return value are unpredictable, and C++ compilers will
  // indeed generate code that leaves garbage in the upper bits.
  //
  // Adjust the contents of the 64-bit register `r` to conform to our internal
  // convention, which requires predictable high bits.  In practice, this means
  // that the 32-bit value will be zero-extended or sign-extended to 64 bits as
  // appropriate for the platform.
  void widenInt32(Register r) DEFINED_ON(arm64, x64, mips64, loong64, riscv64);

  // As enterFakeExitFrame(), but using register conventions appropriate for
  // wasm stubs.
  void enterFakeExitFrameForWasm(Register cxreg, Register scratch,
                                 ExitFrameType type) PER_SHARED_ARCH;

 public:
  // ========================================================================
  // Barrier functions.

  void emitPreBarrierFastPath(JSRuntime* rt, MIRType type, Register temp1,
                              Register temp2, Register temp3, Label* noBarrier);

 public:
  // ========================================================================
  // Clamping functions.

  inline void clampIntToUint8(Register reg) PER_SHARED_ARCH;

 public:
  // ========================================================================
  // Primitive atomic operations.
  //
  // If the access is from JS and the eventual destination of the result is a
  // js::Value, it's probably best to use the JS-specific versions of these,
  // see further below.
  //
  // Temp registers must be defined unless otherwise noted in the per-function
  // constraints.

  // 8-bit, 16-bit, and 32-bit wide operations.
  //
  // The 8-bit and 16-bit operations zero-extend or sign-extend the result to
  // 32 bits, according to `type`. On 64-bit systems, the upper 32 bits of the
  // result will be zero on some platforms (eg, on x64) and will be the sign
  // extension of the lower bits on other platforms (eg, MIPS).

  // CompareExchange with memory.  Return the value that was in memory,
  // whether we wrote or not.
  //
  // x86-shared: `output` must be eax.
  // MIPS: `valueTemp`, `offsetTemp` and `maskTemp` must be defined for 8-bit
  // and 16-bit wide operations.

  void compareExchange(Scalar::Type type, const Synchronization& sync,
                       const Address& mem, Register expected,
                       Register replacement, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void compareExchange(Scalar::Type type, const Synchronization& sync,
                       const BaseIndex& mem, Register expected,
                       Register replacement, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void compareExchange(Scalar::Type type, const Synchronization& sync,
                       const Address& mem, Register expected,
                       Register replacement, Register valueTemp,
                       Register offsetTemp, Register maskTemp, Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void compareExchange(Scalar::Type type, const Synchronization& sync,
                       const BaseIndex& mem, Register expected,
                       Register replacement, Register valueTemp,
                       Register offsetTemp, Register maskTemp, Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  // x86: `expected` and `output` must be edx:eax; `replacement` is ecx:ebx.
  // x64: `output` must be rax.
  // ARM: Registers must be distinct; `replacement` and `output` must be
  // (even,odd) pairs.

  void compareExchange64(const Synchronization& sync, const Address& mem,
                         Register64 expected, Register64 replacement,
                         Register64 output)
      DEFINED_ON(arm, arm64, x64, x86, mips64, loong64, riscv64);

  void compareExchange64(const Synchronization& sync, const BaseIndex& mem,
                         Register64 expected, Register64 replacement,
                         Register64 output)
      DEFINED_ON(arm, arm64, x64, x86, mips64, loong64, riscv64);

  // Exchange with memory.  Return the value initially in memory.
  // MIPS: `valueTemp`, `offsetTemp` and `maskTemp` must be defined for 8-bit
  // and 16-bit wide operations.

  void atomicExchange(Scalar::Type type, const Synchronization& sync,
                      const Address& mem, Register value, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void atomicExchange(Scalar::Type type, const Synchronization& sync,
                      const BaseIndex& mem, Register value, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void atomicExchange(Scalar::Type type, const Synchronization& sync,
                      const Address& mem, Register value, Register valueTemp,
                      Register offsetTemp, Register maskTemp, Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void atomicExchange(Scalar::Type type, const Synchronization& sync,
                      const BaseIndex& mem, Register value, Register valueTemp,
                      Register offsetTemp, Register maskTemp, Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  // x86: `value` must be ecx:ebx; `output` must be edx:eax.
  // ARM: `value` and `output` must be distinct and (even,odd) pairs.
  // ARM64: `value` and `output` must be distinct.

  void atomicExchange64(const Synchronization& sync, const Address& mem,
                        Register64 value, Register64 output)
      DEFINED_ON(arm, arm64, x64, x86, mips64, loong64, riscv64);

  void atomicExchange64(const Synchronization& sync, const BaseIndex& mem,
                        Register64 value, Register64 output)
      DEFINED_ON(arm, arm64, x64, x86, mips64, loong64, riscv64);

  // Read-modify-write with memory.  Return the value in memory before the
  // operation.
  //
  // x86-shared:
  //   For 8-bit operations, `value` and `output` must have a byte subregister.
  //   For Add and Sub, `temp` must be invalid.
  //   For And, Or, and Xor, `output` must be eax and `temp` must have a byte
  //   subregister.
  //
  // ARM: Registers `value` and `output` must differ.
  // MIPS: `valueTemp`, `offsetTemp` and `maskTemp` must be defined for 8-bit
  // and 16-bit wide operations; `value` and `output` must differ.

  void atomicFetchOp(Scalar::Type type, const Synchronization& sync,
                     AtomicOp op, Register value, const Address& mem,
                     Register temp, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void atomicFetchOp(Scalar::Type type, const Synchronization& sync,
                     AtomicOp op, Imm32 value, const Address& mem,
                     Register temp, Register output) DEFINED_ON(x86_shared);

  void atomicFetchOp(Scalar::Type type, const Synchronization& sync,
                     AtomicOp op, Register value, const BaseIndex& mem,
                     Register temp, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void atomicFetchOp(Scalar::Type type, const Synchronization& sync,
                     AtomicOp op, Imm32 value, const BaseIndex& mem,
                     Register temp, Register output) DEFINED_ON(x86_shared);

  void atomicFetchOp(Scalar::Type type, const Synchronization& sync,
                     AtomicOp op, Register value, const Address& mem,
                     Register valueTemp, Register offsetTemp, Register maskTemp,
                     Register output) DEFINED_ON(mips_shared, loong64, riscv64);

  void atomicFetchOp(Scalar::Type type, const Synchronization& sync,
                     AtomicOp op, Register value, const BaseIndex& mem,
                     Register valueTemp, Register offsetTemp, Register maskTemp,
                     Register output) DEFINED_ON(mips_shared, loong64, riscv64);

  // x86:
  //   `temp` must be ecx:ebx; `output` must be edx:eax.
  // x64:
  //   For Add and Sub, `temp` is ignored.
  //   For And, Or, and Xor, `output` must be rax.
  // ARM:
  //   `temp` and `output` must be (even,odd) pairs and distinct from `value`.
  // ARM64:
  //   Registers `value`, `temp`, and `output` must all differ.

  void atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                       Register64 value, const Address& mem, Register64 temp,
                       Register64 output)
      DEFINED_ON(arm, arm64, x64, mips64, loong64, riscv64);

  void atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                       const Address& value, const Address& mem,
                       Register64 temp, Register64 output) DEFINED_ON(x86);

  void atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                       Register64 value, const BaseIndex& mem, Register64 temp,
                       Register64 output)
      DEFINED_ON(arm, arm64, x64, mips64, loong64, riscv64);

  void atomicFetchOp64(const Synchronization& sync, AtomicOp op,
                       const Address& value, const BaseIndex& mem,
                       Register64 temp, Register64 output) DEFINED_ON(x86);

  // x64:
  //   `value` can be any register.
  // ARM:
  //   `temp` must be an (even,odd) pair and distinct from `value`.
  // ARM64:
  //   Registers `value` and `temp` must differ.

  void atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                        Register64 value, const Address& mem) DEFINED_ON(x64);

  void atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                        Register64 value, const Address& mem, Register64 temp)
      DEFINED_ON(arm, arm64, mips64, loong64, riscv64);

  void atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                        Register64 value, const BaseIndex& mem) DEFINED_ON(x64);

  void atomicEffectOp64(const Synchronization& sync, AtomicOp op,
                        Register64 value, const BaseIndex& mem, Register64 temp)
      DEFINED_ON(arm, arm64, mips64, loong64, riscv64);

  // 64-bit atomic load. On 64-bit systems, use regular load with
  // Synchronization::Load, not this method.
  //
  // x86: `temp` must be ecx:ebx; `output` must be edx:eax.
  // ARM: `output` must be (even,odd) pair.

  void atomicLoad64(const Synchronization& sync, const Address& mem,
                    Register64 temp, Register64 output) DEFINED_ON(x86);

  void atomicLoad64(const Synchronization& sync, const BaseIndex& mem,
                    Register64 temp, Register64 output) DEFINED_ON(x86);

  void atomicLoad64(const Synchronization& sync, const Address& mem,
                    Register64 output) DEFINED_ON(arm);

  void atomicLoad64(const Synchronization& sync, const BaseIndex& mem,
                    Register64 output) DEFINED_ON(arm);

  // 64-bit atomic store. On 64-bit systems, use regular store with
  // Synchronization::Store, not this method.
  //
  // x86: `value` must be ecx:ebx; `temp` must be edx:eax.
  // ARM: `value` and `temp` must be (even,odd) pairs.

  void atomicStore64(const Synchronization& sync, const Address& mem,
                     Register64 value, Register64 temp) DEFINED_ON(x86, arm);

  void atomicStore64(const Synchronization& sync, const BaseIndex& mem,
                     Register64 value, Register64 temp) DEFINED_ON(x86, arm);

  // ========================================================================
  // Wasm atomic operations.
  //
  // Constraints, when omitted, are exactly as for the primitive operations
  // above.

  void wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                           const Address& mem, Register expected,
                           Register replacement, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                           const BaseIndex& mem, Register expected,
                           Register replacement, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                           const Address& mem, Register expected,
                           Register replacement, Register valueTemp,
                           Register offsetTemp, Register maskTemp,
                           Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                           const BaseIndex& mem, Register expected,
                           Register replacement, Register valueTemp,
                           Register offsetTemp, Register maskTemp,
                           Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                          const Address& mem, Register value, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                          const BaseIndex& mem, Register value, Register output)
      DEFINED_ON(arm, arm64, x86_shared);

  void wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                          const Address& mem, Register value,
                          Register valueTemp, Register offsetTemp,
                          Register maskTemp, Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                          const BaseIndex& mem, Register value,
                          Register valueTemp, Register offsetTemp,
                          Register maskTemp, Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                         Register value, const Address& mem, Register temp,
                         Register output) DEFINED_ON(arm, arm64, x86_shared);

  void wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                         Imm32 value, const Address& mem, Register temp,
                         Register output) DEFINED_ON(x86_shared);

  void wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                         Register value, const BaseIndex& mem, Register temp,
                         Register output) DEFINED_ON(arm, arm64, x86_shared);

  void wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                         Imm32 value, const BaseIndex& mem, Register temp,
                         Register output) DEFINED_ON(x86_shared);

  void wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                         Register value, const Address& mem, Register valueTemp,
                         Register offsetTemp, Register maskTemp,
                         Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                         Register value, const BaseIndex& mem,
                         Register valueTemp, Register offsetTemp,
                         Register maskTemp, Register output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  // Read-modify-write with memory.  Return no value.
  //
  // MIPS: `valueTemp`, `offsetTemp` and `maskTemp` must be defined for 8-bit
  // and 16-bit wide operations.

  void wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                          Register value, const Address& mem, Register temp)
      DEFINED_ON(arm, arm64, x86_shared);

  void wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                          Imm32 value, const Address& mem, Register temp)
      DEFINED_ON(x86_shared);

  void wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                          Register value, const BaseIndex& mem, Register temp)
      DEFINED_ON(arm, arm64, x86_shared);

  void wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                          Imm32 value, const BaseIndex& mem, Register temp)
      DEFINED_ON(x86_shared);

  void wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                          Register value, const Address& mem,
                          Register valueTemp, Register offsetTemp,
                          Register maskTemp)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access, AtomicOp op,
                          Register value, const BaseIndex& mem,
                          Register valueTemp, Register offsetTemp,
                          Register maskTemp)
      DEFINED_ON(mips_shared, loong64, riscv64);

  // 64-bit wide operations.

  // 64-bit atomic load.  On 64-bit systems, use regular wasm load with
  // Synchronization::Load, not this method.
  //
  // x86: `temp` must be ecx:ebx; `output` must be edx:eax.
  // ARM: `temp` should be invalid; `output` must be (even,odd) pair.
  // MIPS32: `temp` should be invalid.

  void wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                        const Address& mem, Register64 temp, Register64 output)
      DEFINED_ON(arm, mips32, x86, wasm32);

  void wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                        const BaseIndex& mem, Register64 temp,
                        Register64 output) DEFINED_ON(arm, mips32, x86, wasm32);

  // x86: `expected` must be the same as `output`, and must be edx:eax.
  // x86: `replacement` must be ecx:ebx.
  // x64: `output` must be rax.
  // ARM: Registers must be distinct; `replacement` and `output` must be
  // (even,odd) pairs.
  // ARM64: The base register in `mem` must not overlap `output`.
  // MIPS: Registers must be distinct.

  void wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                             const Address& mem, Register64 expected,
                             Register64 replacement,
                             Register64 output) PER_ARCH;

  void wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                             const BaseIndex& mem, Register64 expected,
                             Register64 replacement,
                             Register64 output) PER_ARCH;

  // x86: `value` must be ecx:ebx; `output` must be edx:eax.
  // ARM: Registers must be distinct; `value` and `output` must be (even,odd)
  // pairs.
  // MIPS: Registers must be distinct.

  void wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                            const Address& mem, Register64 value,
                            Register64 output) PER_ARCH;

  void wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                            const BaseIndex& mem, Register64 value,
                            Register64 output) PER_ARCH;

  // x86: `output` must be edx:eax, `temp` must be ecx:ebx.
  // x64: For And, Or, and Xor `output` must be rax.
  // ARM: Registers must be distinct; `temp` and `output` must be (even,odd)
  // pairs.
  // MIPS: Registers must be distinct.
  // MIPS32: `temp` should be invalid.

  void wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access, AtomicOp op,
                           Register64 value, const Address& mem,
                           Register64 temp, Register64 output)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, x64);

  void wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access, AtomicOp op,
                           Register64 value, const BaseIndex& mem,
                           Register64 temp, Register64 output)
      DEFINED_ON(arm, arm64, mips32, mips64, loong64, riscv64, x64);

  void wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access, AtomicOp op,
                           const Address& value, const Address& mem,
                           Register64 temp, Register64 output) DEFINED_ON(x86);

  void wasmAtomicFetchOp64(const wasm::MemoryAccessDesc& access, AtomicOp op,
                           const Address& value, const BaseIndex& mem,
                           Register64 temp, Register64 output) DEFINED_ON(x86);

  // Here `value` can be any register.

  void wasmAtomicEffectOp64(const wasm::MemoryAccessDesc& access, AtomicOp op,
                            Register64 value, const BaseIndex& mem)
      DEFINED_ON(x64);

  void wasmAtomicEffectOp64(const wasm::MemoryAccessDesc& access, AtomicOp op,
                            Register64 value, const BaseIndex& mem,
                            Register64 temp) DEFINED_ON(arm64);

  // ========================================================================
  // JS atomic operations.
  //
  // Here the arrayType must be a type that is valid for JS.  As of 2017 that
  // is an 8-bit, 16-bit, or 32-bit integer type.
  //
  // If arrayType is Scalar::Uint32 then:
  //
  //   - `output` must be a float register
  //   - if the operation takes one temp register then `temp` must be defined
  //   - if the operation takes two temp registers then `temp2` must be defined.
  //
  // Otherwise `output` must be a GPR and `temp`/`temp2` should be InvalidReg.
  // (`temp1` must always be valid.)
  //
  // For additional register constraints, see the primitive 32-bit operations
  // and/or wasm operations above.

  void compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                         const Address& mem, Register expected,
                         Register replacement, Register temp,
                         AnyRegister output) DEFINED_ON(arm, arm64, x86_shared);

  void compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                         const BaseIndex& mem, Register expected,
                         Register replacement, Register temp,
                         AnyRegister output) DEFINED_ON(arm, arm64, x86_shared);

  void compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                         const Address& mem, Register expected,
                         Register replacement, Register valueTemp,
                         Register offsetTemp, Register maskTemp, Register temp,
                         AnyRegister output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                         const BaseIndex& mem, Register expected,
                         Register replacement, Register valueTemp,
                         Register offsetTemp, Register maskTemp, Register temp,
                         AnyRegister output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                        const Address& mem, Register value, Register temp,
                        AnyRegister output) DEFINED_ON(arm, arm64, x86_shared);

  void atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                        const BaseIndex& mem, Register value, Register temp,
                        AnyRegister output) DEFINED_ON(arm, arm64, x86_shared);

  void atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                        const Address& mem, Register value, Register valueTemp,
                        Register offsetTemp, Register maskTemp, Register temp,
                        AnyRegister output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                        const BaseIndex& mem, Register value,
                        Register valueTemp, Register offsetTemp,
                        Register maskTemp, Register temp, AnyRegister output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync,
                       AtomicOp op, Register value, const Address& mem,
                       Register temp1, Register temp2, AnyRegister output)
      DEFINED_ON(arm, arm64, x86_shared);

  void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync,
                       AtomicOp op, Register value, const BaseIndex& mem,
                       Register temp1, Register temp2, AnyRegister output)
      DEFINED_ON(arm, arm64, x86_shared);

  void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync,
                       AtomicOp op, Imm32 value, const Address& mem,
                       Register temp1, Register temp2, AnyRegister output)
      DEFINED_ON(x86_shared);

  void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync,
                       AtomicOp op, Imm32 value, const BaseIndex& mem,
                       Register temp1, Register temp2, AnyRegister output)
      DEFINED_ON(x86_shared);

  void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync,
                       AtomicOp op, Register value, const Address& mem,
                       Register valueTemp, Register offsetTemp,
                       Register maskTemp, Register temp, AnyRegister output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync,
                       AtomicOp op, Register value, const BaseIndex& mem,
                       Register valueTemp, Register offsetTemp,
                       Register maskTemp, Register temp, AnyRegister output)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync,
                        AtomicOp op, Register value, const Address& mem,
                        Register temp) DEFINED_ON(arm, arm64, x86_shared);

  void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync,
                        AtomicOp op, Register value, const BaseIndex& mem,
                        Register temp) DEFINED_ON(arm, arm64, x86_shared);

  void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync,
                        AtomicOp op, Imm32 value, const Address& mem,
                        Register temp) DEFINED_ON(x86_shared);

  void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync,
                        AtomicOp op, Imm32 value, const BaseIndex& mem,
                        Register temp) DEFINED_ON(x86_shared);

  void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync,
                        AtomicOp op, Register value, const Address& mem,
                        Register valueTemp, Register offsetTemp,
                        Register maskTemp)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync,
                        AtomicOp op, Register value, const BaseIndex& mem,
                        Register valueTemp, Register offsetTemp,
                        Register maskTemp)
      DEFINED_ON(mips_shared, loong64, riscv64);

  void atomicIsLockFreeJS(Register value, Register output);

  // ========================================================================
  // Spectre Mitigations.
  //
  // Spectre attacks are side-channel attacks based on cache pollution or
  // slow-execution of some instructions. We have multiple spectre mitigations
  // possible:
  //
  //   - Stop speculative executions, with memory barriers. Memory barriers
  //     force all branches depending on loads to be resolved, and thus
  //     resolve all miss-speculated paths.
  //
  //   - Use conditional move instructions. Some CPUs have a branch predictor,
  //     and not a flag predictor. In such cases, using a conditional move
  //     instruction to zero some pointer/index is enough to add a
  //     data-dependency which prevents any futher executions until the load is
  //     resolved.

  void spectreMaskIndex32(Register index, Register length, Register output);
  void spectreMaskIndex32(Register index, const Address& length,
                          Register output);
  void spectreMaskIndexPtr(Register index, Register length, Register output);
  void spectreMaskIndexPtr(Register index, const Address& length,
                           Register output);

  // The length must be a power of two. Performs a bounds check and Spectre
  // index masking.
  void boundsCheck32PowerOfTwo(Register index, uint32_t length, Label* failure);

  void speculationBarrier() PER_SHARED_ARCH;

  //}}} check_macroassembler_decl_style
 public:
  // Unsafe here means the caller is responsible for Spectre mitigations if
  // needed. Prefer branchTestObjClass or one of the other masm helpers!
  inline void loadObjClassUnsafe(Register obj, Register dest);

  template <typename EmitPreBarrier>
  inline void storeObjShape(Register shape, Register obj,
                            EmitPreBarrier emitPreBarrier);
  template <typename EmitPreBarrier>
  inline void storeObjShape(Shape* shape, Register obj,
                            EmitPreBarrier emitPreBarrier);

  inline void loadObjProto(Register obj, Register dest);

  inline void loadStringLength(Register str, Register dest);

  void loadStringChars(Register str, Register dest, CharEncoding encoding);

  void loadNonInlineStringChars(Register str, Register dest,
                                CharEncoding encoding);
  void loadNonInlineStringCharsForStore(Register str, Register dest);
  void storeNonInlineStringChars(Register chars, Register str);

  void loadInlineStringChars(Register str, Register dest,
                             CharEncoding encoding);
  void loadInlineStringCharsForStore(Register str, Register dest);

 private:
  void loadRopeChild(Register str, Register index, Register output,
                     Label* isLinear);

 public:
  void branchIfCanLoadStringChar(Register str, Register index, Register scratch,
                                 Label* label);
  void branchIfNotCanLoadStringChar(Register str, Register index,
                                    Register scratch, Label* label);

  void loadStringChar(Register str, Register index, Register output,
                      Register scratch1, Register scratch2, Label* fail);

  void loadRopeLeftChild(Register str, Register dest);
  void loadRopeRightChild(Register str, Register dest);
  void storeRopeChildren(Register left, Register right, Register str);

  void loadDependentStringBase(Register str, Register dest);
  void storeDependentStringBase(Register base, Register str);

  void loadStringIndexValue(Register str, Register dest, Label* fail);

  /**
   * Store the character in |src| to |dest|.
   */
  template <typename T>
  void storeChar(const T& src, Address dest, CharEncoding encoding) {
    if (encoding == CharEncoding::Latin1) {
      store8(src, dest);
    } else {
      store16(src, dest);
    }
  }

  /**
   * Load the character at |src| into |dest|.
   */
  template <typename T>
  void loadChar(const T& src, Register dest, CharEncoding encoding) {
    if (encoding == CharEncoding::Latin1) {
      load8ZeroExtend(src, dest);
    } else {
      load16ZeroExtend(src, dest);
    }
  }

  /**
   * Load the character at |chars[index + offset]| into |dest|. The optional
   * offset argument is not scaled to the character encoding.
   */
  void loadChar(Register chars, Register index, Register dest,
                CharEncoding encoding, int32_t offset = 0);

  /**
   * Add |index| to |chars| so that |chars| now points at |chars[index]|.
   */
  void addToCharPtr(Register chars, Register index, CharEncoding encoding);

 private:
  void loadStringFromUnit(Register unit, Register dest,
                          const StaticStrings& staticStrings);
  void loadLengthTwoString(Register c1, Register c2, Register dest,
                           const StaticStrings& staticStrings);

 public:
  /**
   * Load the string representation of |input| in base |base|. Jumps to |fail|
   * when the string representation needs to be allocated dynamically.
   */
  void loadInt32ToStringWithBase(Register input, Register base, Register dest,
                                 Register scratch1, Register scratch2,
                                 const StaticStrings& staticStrings,
                                 const LiveRegisterSet& volatileRegs,
                                 Label* fail);
  void loadInt32ToStringWithBase(Register input, int32_t base, Register dest,
                                 Register scratch1, Register scratch2,
                                 const StaticStrings& staticStrings,
                                 Label* fail);

  /**
   * Load the BigInt digits from |bigInt| into |digits|.
   */
  void loadBigIntDigits(Register bigInt, Register digits);

  /**
   * Load the first [u]int64 value from |bigInt| into |dest|.
   */
  void loadBigInt64(Register bigInt, Register64 dest);

  /**
   * Load the first digit from |bigInt| into |dest|. Handles the case when the
   * BigInt digits length is zero.
   *
   * Note: A BigInt digit is a pointer-sized value.
   */
  void loadFirstBigIntDigitOrZero(Register bigInt, Register dest);

  /**
   * Load the number stored in |bigInt| into |dest|. Handles the case when the
   * BigInt digits length is zero. Jumps to |fail| when the number can't be
   * saved into a single pointer-sized register.
   */
  void loadBigInt(Register bigInt, Register dest, Label* fail);

  /**
   * Load the number stored in |bigInt| into |dest|. Doesn't handle the case
   * when the BigInt digits length is zero. Jumps to |fail| when the number
   * can't be saved into a single pointer-sized register.
   */
  void loadBigIntNonZero(Register bigInt, Register dest, Label* fail);

  /**
   * Load the absolute number stored in |bigInt| into |dest|. Handles the case
   * when the BigInt digits length is zero. Jumps to |fail| when the number
   * can't be saved into a single pointer-sized register.
   */
  void loadBigIntAbsolute(Register bigInt, Register dest, Label* fail);

  /**
   * In-place modifies the BigInt digit to a signed pointer-sized value. Jumps
   * to |fail| when the digit exceeds the representable range.
   */
  void bigIntDigitToSignedPtr(Register bigInt, Register digit, Label* fail);

  /**
   * Initialize a BigInt from |val|. Clobbers |val|!
   */
  void initializeBigInt64(Scalar::Type type, Register bigInt, Register64 val);

  /**
   * Initialize a BigInt from the signed, pointer-sized register |val|.
   * Clobbers |val|!
   */
  void initializeBigInt(Register bigInt, Register val);

  /**
   * Initialize a BigInt from the pointer-sized register |val|.
   */
  void initializeBigIntAbsolute(Register bigInt, Register val);

  /**
   * Copy a BigInt. Jumps to |fail| on allocation failure or when the BigInt
   * digits need to be heap allocated.
   */
  void copyBigIntWithInlineDigits(Register src, Register dest, Register temp,
                                  gc::Heap initialHeap, Label* fail);

  /**
   * Compare a BigInt and an Int32 value. Falls through to the false case.
   */
  void compareBigIntAndInt32(JSOp op, Register bigInt, Register int32,
                             Register scratch1, Register scratch2,
                             Label* ifTrue, Label* ifFalse);

  /**
   * Compare two BigInts for equality. Falls through if both BigInts are equal
   * to each other.
   *
   * - When we jump to |notSameLength|, |temp1| holds the length of the right
   *   operand.
   * - When we jump to |notSameDigit|, |temp2| points to the current digit of
   *   the left operand and |temp4| holds the current digit of the right
   *   operand.
   */
  void equalBigInts(Register left, Register right, Register temp1,
                    Register temp2, Register temp3, Register temp4,
                    Label* notSameSign, Label* notSameLength,
                    Label* notSameDigit);

  void loadJSContext(Register dest);

  void switchToRealm(Register realm);
  void switchToRealm(const void* realm, Register scratch);
  void switchToObjectRealm(Register obj, Register scratch);
  void switchToBaselineFrameRealm(Register scratch);
  void switchToWasmInstanceRealm(Register scratch1, Register scratch2);
  void debugAssertContextRealm(const void* realm, Register scratch);

  void loadJitActivation(Register dest);

  void guardSpecificAtom(Register str, JSAtom* atom, Register scratch,
                         const LiveRegisterSet& volatileRegs, Label* fail);

  void guardStringToInt32(Register str, Register output, Register scratch,
                          LiveRegisterSet volatileRegs, Label* fail);

  template <typename T>
  void loadTypedOrValue(const T& src, TypedOrValueRegister dest) {
    if (dest.hasValue()) {
      loadValue(src, dest.valueReg());
    } else {
      loadUnboxedValue(src, dest.type(), dest.typedReg());
    }
  }

  template <typename T>
  void storeTypedOrValue(TypedOrValueRegister src, const T& dest) {
    if (src.hasValue()) {
      storeValue(src.valueReg(), dest);
    } else if (IsFloatingPointType(src.type())) {
      FloatRegister reg = src.typedReg().fpu();
      if (src.type() == MIRType::Float32) {
        ScratchDoubleScope fpscratch(*this);
        convertFloat32ToDouble(reg, fpscratch);
        boxDouble(fpscratch, dest);
      } else {
        boxDouble(reg, dest);
      }
    } else {
      storeValue(ValueTypeFromMIRType(src.type()), src.typedReg().gpr(), dest);
    }
  }

  template <typename T>
  void storeConstantOrRegister(const ConstantOrRegister& src, const T& dest) {
    if (src.constant()) {
      storeValue(src.value(), dest);
    } else {
      storeTypedOrValue(src.reg(), dest);
    }
  }

  void storeCallPointerResult(Register reg) {
    if (reg != ReturnReg) {
      mov(ReturnReg, reg);
    }
  }

  inline void storeCallBoolResult(Register reg);
  inline void storeCallInt32Result(Register reg);

  void storeCallFloatResult(FloatRegister reg) {
    if (reg != ReturnDoubleReg) {
      moveDouble(ReturnDoubleReg, reg);
    }
  }

  inline void storeCallResultValue(AnyRegister dest, JSValueType type);

  void storeCallResultValue(ValueOperand dest) {
#if defined(JS_NUNBOX32)
    // reshuffle the return registers used for a call result to store into
    // dest, using ReturnReg as a scratch register if necessary. This must
    // only be called after returning from a call, at a point when the
    // return register is not live. XXX would be better to allow wrappers
    // to store the return value to different places.
    if (dest.typeReg() == JSReturnReg_Data) {
      if (dest.payloadReg() == JSReturnReg_Type) {
        // swap the two registers.
        mov(JSReturnReg_Type, ReturnReg);
        mov(JSReturnReg_Data, JSReturnReg_Type);
        mov(ReturnReg, JSReturnReg_Data);
      } else {
        mov(JSReturnReg_Data, dest.payloadReg());
        mov(JSReturnReg_Type, dest.typeReg());
      }
    } else {
      mov(JSReturnReg_Type, dest.typeReg());
      mov(JSReturnReg_Data, dest.payloadReg());
    }
#elif defined(JS_PUNBOX64)
    if (dest.valueReg() != JSReturnReg) {
      mov(JSReturnReg, dest.valueReg());
    }
#else
#  error "Bad architecture"
#endif
  }

  inline void storeCallResultValue(TypedOrValueRegister dest);

 private:
  TrampolinePtr preBarrierTrampoline(MIRType type);

  template <typename T>
  void unguardedCallPreBarrier(const T& address, MIRType type) {
    Label done;
    if (type == MIRType::Value) {
      branchTestGCThing(Assembler::NotEqual, address, &done);
    } else if (type == MIRType::Object || type == MIRType::String) {
      branchPtr(Assembler::Equal, address, ImmWord(0), &done);
    }

    Push(PreBarrierReg);
    computeEffectiveAddress(address, PreBarrierReg);

    TrampolinePtr preBarrier = preBarrierTrampoline(type);

    call(preBarrier);
    Pop(PreBarrierReg);
    // On arm64, SP may be < PSP now (that's OK).
    // eg testcase: tests/auto-regress/bug702915.js
    bind(&done);
  }

 public:
  template <typename T>
  void guardedCallPreBarrier(const T& address, MIRType type) {
    Label done;
    branchTestNeedsIncrementalBarrier(Assembler::Zero, &done);
    unguardedCallPreBarrier(address, type);
    bind(&done);
  }

  // Like guardedCallPreBarrier, but unlike guardedCallPreBarrier this can be
  // called from runtime-wide trampolines because it loads cx->zone (instead of
  // baking in the current Zone) if JitContext::realm is nullptr.
  template <typename T>
  void guardedCallPreBarrierAnyZone(const T& address, MIRType type,
                                    Register scratch) {
    Label done;
    branchTestNeedsIncrementalBarrierAnyZone(Assembler::Zero, &done, scratch);
    unguardedCallPreBarrier(address, type);
    bind(&done);
  }

  enum class Uint32Mode { FailOnDouble, ForceDouble };

  void boxUint32(Register source, ValueOperand dest, Uint32Mode uint32Mode,
                 Label* fail);

  template <typename T>
  void loadFromTypedArray(Scalar::Type arrayType, const T& src,
                          AnyRegister dest, Register temp, Label* fail);

  template <typename T>
  void loadFromTypedArray(Scalar::Type arrayType, const T& src,
                          const ValueOperand& dest, Uint32Mode uint32Mode,
                          Register temp, Label* fail);

  template <typename T>
  void loadFromTypedBigIntArray(Scalar::Type arrayType, const T& src,
                                Register bigInt, Register64 temp);

  template <typename S, typename T>
  void storeToTypedIntArray(Scalar::Type arrayType, const S& value,
                            const T& dest) {
    switch (arrayType) {
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
        store8(value, dest);
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        store16(value, dest);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        store32(value, dest);
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
  }

  void storeToTypedFloatArray(Scalar::Type arrayType, FloatRegister value,
                              const BaseIndex& dest);
  void storeToTypedFloatArray(Scalar::Type arrayType, FloatRegister value,
                              const Address& dest);

  void storeToTypedBigIntArray(Scalar::Type arrayType, Register64 value,
                               const BaseIndex& dest);
  void storeToTypedBigIntArray(Scalar::Type arrayType, Register64 value,
                               const Address& dest);

  void memoryBarrierBefore(const Synchronization& sync);
  void memoryBarrierAfter(const Synchronization& sync);

  void debugAssertIsObject(const ValueOperand& val);
  void debugAssertObjHasFixedSlots(Register obj, Register scratch);

  void debugAssertObjectHasClass(Register obj, Register scratch,
                                 const JSClass* clasp);

  void branchArrayIsNotPacked(Register array, Register temp1, Register temp2,
                              Label* label);

  void setIsPackedArray(Register obj, Register output, Register temp);

  void packedArrayPop(Register array, ValueOperand output, Register temp1,
                      Register temp2, Label* fail);
  void packedArrayShift(Register array, ValueOperand output, Register temp1,
                        Register temp2, LiveRegisterSet volatileRegs,
                        Label* fail);

  void loadArgumentsObjectElement(Register obj, Register index,
                                  ValueOperand output, Register temp,
                                  Label* fail);
  void loadArgumentsObjectElementHole(Register obj, Register index,
                                      ValueOperand output, Register temp,
                                      Label* fail);
  void loadArgumentsObjectElementExists(Register obj, Register index,
                                        Register output, Register temp,
                                        Label* fail);

  void loadArgumentsObjectLength(Register obj, Register output, Label* fail);

  void branchTestArgumentsObjectFlags(Register obj, Register temp,
                                      uint32_t flags, Condition cond,
                                      Label* label);

  void typedArrayElementSize(Register obj, Register output);
  void branchIfClassIsNotTypedArray(Register clasp, Label* notTypedArray);

  void branchIfHasDetachedArrayBuffer(Register obj, Register temp,
                                      Label* label);

  void branchIfNativeIteratorNotReusable(Register ni, Label* notReusable);
  void branchNativeIteratorIndices(Condition cond, Register ni, Register temp,
                                   NativeIteratorIndices kind, Label* label);

  void maybeLoadIteratorFromShape(Register obj, Register dest, Register temp,
                                  Register temp2, Register temp3,
                                  Label* failure);

  void iteratorMore(Register obj, ValueOperand output, Register temp);
  void iteratorClose(Register obj, Register temp1, Register temp2,
                     Register temp3);
  void registerIterator(Register enumeratorsList, Register iter, Register temp);

  void toHashableNonGCThing(ValueOperand value, ValueOperand result,
                            FloatRegister tempFloat);

  void toHashableValue(ValueOperand value, ValueOperand result,
                       FloatRegister tempFloat, Label* atomizeString,
                       Label* tagString);

 private:
  void scrambleHashCode(Register result);

 public:
  void prepareHashNonGCThing(ValueOperand value, Register result,
                             Register temp);
  void prepareHashString(Register str, Register result, Register temp);
  void prepareHashSymbol(Register sym, Register result);
  void prepareHashBigInt(Register bigInt, Register result, Register temp1,
                         Register temp2, Register temp3);
  void prepareHashObject(Register setObj, ValueOperand value, Register result,
                         Register temp1, Register temp2, Register temp3,
                         Register temp4);
  void prepareHashValue(Register setObj, ValueOperand value, Register result,
                        Register temp1, Register temp2, Register temp3,
                        Register temp4);

 private:
  enum class IsBigInt { No, Yes, Maybe };

  /**
   * Search for a value in a OrderedHashTable.
   *
   * When we jump to |found|, |entryTemp| holds the found hashtable entry.
   */
  template <typename OrderedHashTable>
  void orderedHashTableLookup(Register setOrMapObj, ValueOperand value,
                              Register hash, Register entryTemp, Register temp1,
                              Register temp3, Register temp4, Register temp5,
                              Label* found, IsBigInt isBigInt);

  void setObjectHas(Register setObj, ValueOperand value, Register hash,
                    Register result, Register temp1, Register temp2,
                    Register temp3, Register temp4, IsBigInt isBigInt);

  void mapObjectHas(Register mapObj, ValueOperand value, Register hash,
                    Register result, Register temp1, Register temp2,
                    Register temp3, Register temp4, IsBigInt isBigInt);

  void mapObjectGet(Register mapObj, ValueOperand value, Register hash,
                    ValueOperand result, Register temp1, Register temp2,
                    Register temp3, Register temp4, Register temp5,
                    IsBigInt isBigInt);

 public:
  void setObjectHasNonBigInt(Register setObj, ValueOperand value, Register hash,
                             Register result, Register temp1, Register temp2) {
    return setObjectHas(setObj, value, hash, result, temp1, temp2, InvalidReg,
                        InvalidReg, IsBigInt::No);
  }
  void setObjectHasBigInt(Register setObj, ValueOperand value, Register hash,
                          Register result, Register temp1, Register temp2,
                          Register temp3, Register temp4) {
    return setObjectHas(setObj, value, hash, result, temp1, temp2, temp3, temp4,
                        IsBigInt::Yes);
  }
  void setObjectHasValue(Register setObj, ValueOperand value, Register hash,
                         Register result, Register temp1, Register temp2,
                         Register temp3, Register temp4) {
    return setObjectHas(setObj, value, hash, result, temp1, temp2, temp3, temp4,
                        IsBigInt::Maybe);
  }

  void mapObjectHasNonBigInt(Register mapObj, ValueOperand value, Register hash,
                             Register result, Register temp1, Register temp2) {
    return mapObjectHas(mapObj, value, hash, result, temp1, temp2, InvalidReg,
                        InvalidReg, IsBigInt::No);
  }
  void mapObjectHasBigInt(Register mapObj, ValueOperand value, Register hash,
                          Register result, Register temp1, Register temp2,
                          Register temp3, Register temp4) {
    return mapObjectHas(mapObj, value, hash, result, temp1, temp2, temp3, temp4,
                        IsBigInt::Yes);
  }
  void mapObjectHasValue(Register mapObj, ValueOperand value, Register hash,
                         Register result, Register temp1, Register temp2,
                         Register temp3, Register temp4) {
    return mapObjectHas(mapObj, value, hash, result, temp1, temp2, temp3, temp4,
                        IsBigInt::Maybe);
  }

  void mapObjectGetNonBigInt(Register mapObj, ValueOperand value, Register hash,
                             ValueOperand result, Register temp1,
                             Register temp2, Register temp3) {
    return mapObjectGet(mapObj, value, hash, result, temp1, temp2, temp3,
                        InvalidReg, InvalidReg, IsBigInt::No);
  }
  void mapObjectGetBigInt(Register mapObj, ValueOperand value, Register hash,
                          ValueOperand result, Register temp1, Register temp2,
                          Register temp3, Register temp4, Register temp5) {
    return mapObjectGet(mapObj, value, hash, result, temp1, temp2, temp3, temp4,
                        temp5, IsBigInt::Yes);
  }
  void mapObjectGetValue(Register mapObj, ValueOperand value, Register hash,
                         ValueOperand result, Register temp1, Register temp2,
                         Register temp3, Register temp4, Register temp5) {
    return mapObjectGet(mapObj, value, hash, result, temp1, temp2, temp3, temp4,
                        temp5, IsBigInt::Maybe);
  }

 private:
  template <typename OrderedHashTable>
  void loadOrderedHashTableCount(Register setOrMapObj, Register result);

 public:
  void loadSetObjectSize(Register setObj, Register result);
  void loadMapObjectSize(Register mapObj, Register result);

  // Inline version of js_TypedArray_uint8_clamp_double.
  // This function clobbers the input register.
  void clampDoubleToUint8(FloatRegister input, Register output) PER_ARCH;

  using MacroAssemblerSpecific::ensureDouble;

  template <typename S>
  void ensureDouble(const S& source, FloatRegister dest, Label* failure) {
    Label isDouble, done;
    branchTestDouble(Assembler::Equal, source, &isDouble);
    branchTestInt32(Assembler::NotEqual, source, failure);

    convertInt32ToDouble(source, dest);
    jump(&done);

    bind(&isDouble);
    unboxDouble(source, dest);

    bind(&done);
  }

  // Inline allocation.
 private:
  void checkAllocatorState(Label* fail);
  bool shouldNurseryAllocate(gc::AllocKind allocKind, gc::Heap initialHeap);
  void nurseryAllocateObject(
      Register result, Register temp, gc::AllocKind allocKind,
      size_t nDynamicSlots, Label* fail,
      const AllocSiteInput& allocSite = AllocSiteInput());
  void bumpPointerAllocate(Register result, Register temp, Label* fail,
                           CompileZone* zone, JS::TraceKind traceKind,
                           uint32_t size,
                           const AllocSiteInput& allocSite = AllocSiteInput());
  void updateAllocSite(Register temp, Register result, CompileZone* zone,
                       Register site);

  void freeListAllocate(Register result, Register temp, gc::AllocKind allocKind,
                        Label* fail);
  void allocateObject(Register result, Register temp, gc::AllocKind allocKind,
                      uint32_t nDynamicSlots, gc::Heap initialHeap, Label* fail,
                      const AllocSiteInput& allocSite = AllocSiteInput());
  void nurseryAllocateString(Register result, Register temp,
                             gc::AllocKind allocKind, Label* fail);
  void allocateString(Register result, Register temp, gc::AllocKind allocKind,
                      gc::Heap initialHeap, Label* fail);
  void nurseryAllocateBigInt(Register result, Register temp, Label* fail);
  void copySlotsFromTemplate(Register obj,
                             const TemplateNativeObject& templateObj,
                             uint32_t start, uint32_t end);
  void fillSlotsWithConstantValue(Address addr, Register temp, uint32_t start,
                                  uint32_t end, const Value& v);
  void fillSlotsWithUndefined(Address addr, Register temp, uint32_t start,
                              uint32_t end);
  void fillSlotsWithUninitialized(Address addr, Register temp, uint32_t start,
                                  uint32_t end);

  void initGCSlots(Register obj, Register temp,
                   const TemplateNativeObject& templateObj);

 public:
  void callFreeStub(Register slots);
  void createGCObject(Register result, Register temp,
                      const TemplateObject& templateObj, gc::Heap initialHeap,
                      Label* fail, bool initContents = true);

  void createPlainGCObject(Register result, Register shape, Register temp,
                           Register temp2, uint32_t numFixedSlots,
                           uint32_t numDynamicSlots, gc::AllocKind allocKind,
                           gc::Heap initialHeap, Label* fail,
                           const AllocSiteInput& allocSite,
                           bool initContents = true);

  void createArrayWithFixedElements(
      Register result, Register shape, Register temp, uint32_t arrayLength,
      uint32_t arrayCapacity, gc::AllocKind allocKind, gc::Heap initialHeap,
      Label* fail, const AllocSiteInput& allocSite = AllocSiteInput());

  void initGCThing(Register obj, Register temp,
                   const TemplateObject& templateObj, bool initContents = true);

  enum class TypedArrayLength { Fixed, Dynamic };

  void initTypedArraySlots(Register obj, Register temp, Register lengthReg,
                           LiveRegisterSet liveRegs, Label* fail,
                           TypedArrayObject* templateObj,
                           TypedArrayLength lengthKind);

  void newGCString(Register result, Register temp, gc::Heap initialHeap,
                   Label* fail);
  void newGCFatInlineString(Register result, Register temp,
                            gc::Heap initialHeap, Label* fail);

  void newGCBigInt(Register result, Register temp, gc::Heap initialHeap,
                   Label* fail);

  // Compares two strings for equality based on the JSOP.
  // This checks for identical pointers, atoms and length and fails for
  // everything else.
  void compareStrings(JSOp op, Register left, Register right, Register result,
                      Label* fail);

  // Result of the typeof operation. Falls back to slow-path for proxies.
  void typeOfObject(Register objReg, Register scratch, Label* slow,
                    Label* isObject, Label* isCallable, Label* isUndefined);

  // Implementation of IsCallable. Doesn't handle proxies.
  void isCallable(Register obj, Register output, Label* isProxy) {
    isCallableOrConstructor(true, obj, output, isProxy);
  }
  void isConstructor(Register obj, Register output, Label* isProxy) {
    isCallableOrConstructor(false, obj, output, isProxy);
  }

  void setIsCrossRealmArrayConstructor(Register obj, Register output);

  void setIsDefinitelyTypedArrayConstructor(Register obj, Register output);

  void loadMegamorphicCache(Register dest);
  void loadStringToAtomCacheLastLookups(Register dest);
  void loadMegamorphicSetPropCache(Register dest);

  void loadAtomOrSymbolAndHash(ValueOperand value, Register outId,
                               Register outHash, Label* cacheMiss);

  void loadAtomHash(Register id, Register hash, Label* done);

  void emitExtractValueFromMegamorphicCacheEntry(
      Register obj, Register entry, Register scratch1, Register scratch2,
      ValueOperand output, Label* cacheHit, Label* cacheMiss);

  template <typename IdOperandType>
  void emitMegamorphicCacheLookupByValueCommon(
      IdOperandType id, Register obj, Register scratch1, Register scratch2,
      Register outEntryPtr, Label* cacheMiss, Label* cacheMissWithEntry);

  void emitMegamorphicCacheLookup(PropertyKey id, Register obj,
                                  Register scratch1, Register scratch2,
                                  Register outEntryPtr, ValueOperand output,
                                  Label* cacheHit);

  // NOTE: |id| must either be a ValueOperand or a Register. If it is a
  // Register, we assume that it is an atom.
  template <typename IdOperandType>
  void emitMegamorphicCacheLookupByValue(IdOperandType id, Register obj,
                                         Register scratch1, Register scratch2,
                                         Register outEntryPtr,
                                         ValueOperand output, Label* cacheHit);

  void emitMegamorphicCacheLookupExists(ValueOperand id, Register obj,
                                        Register scratch1, Register scratch2,
                                        Register outEntryPtr, Register output,
                                        Label* cacheHit, bool hasOwn);

  // Given a PropertyIteratorObject with valid indices, extract the current
  // PropertyIndex, storing the index in |outIndex| and the kind in |outKind|
  void extractCurrentIndexAndKindFromIterator(Register iterator,
                                              Register outIndex,
                                              Register outKind);

  template <typename IdType>
#ifdef JS_CODEGEN_X86
  // See MegamorphicSetElement in LIROps.yaml
  void emitMegamorphicCachedSetSlot(IdType id, Register obj, Register scratch1,
                                    ValueOperand value, Label* cacheHit,
                                    void (*emitPreBarrier)(MacroAssembler&,
                                                           const Address&,
                                                           MIRType));
#else
  void emitMegamorphicCachedSetSlot(
      IdType id, Register obj, Register scratch1, Register scratch2,
      Register scratch3, ValueOperand value, Label* cacheHit,
      void (*emitPreBarrier)(MacroAssembler&, const Address&, MIRType));
#endif

  void loadDOMExpandoValueGuardGeneration(
      Register obj, ValueOperand output,
      JS::ExpandoAndGeneration* expandoAndGeneration, uint64_t generation,
      Label* fail);

  void guardNonNegativeIntPtrToInt32(Register reg, Label* fail);

  void loadArrayBufferByteLengthIntPtr(Register obj, Register output);
  void loadArrayBufferViewByteOffsetIntPtr(Register obj, Register output);
  void loadArrayBufferViewLengthIntPtr(Register obj, Register output);

 private:
  void isCallableOrConstructor(bool isCallable, Register obj, Register output,
                               Label* isProxy);

 public:
  // Generates code used to complete a bailout.
  void generateBailoutTail(Register scratch, Register bailoutInfo);

 public:
#ifndef JS_CODEGEN_ARM64
  // StackPointer manipulation functions.
  // On ARM64, the StackPointer is implemented as two synchronized registers.
  // Code shared across platforms must use these functions to be valid.
  template <typename T>
  inline void addToStackPtr(T t);
  template <typename T>
  inline void addStackPtrTo(T t);

  void subFromStackPtr(Imm32 imm32)
      DEFINED_ON(mips32, mips64, loong64, riscv64, wasm32, arm, x86, x64);
  void subFromStackPtr(Register reg);

  template <typename T>
  void subStackPtrFrom(T t) {
    subPtr(getStackPointer(), t);
  }

  template <typename T>
  void andToStackPtr(T t) {
    andPtr(t, getStackPointer());
  }

  template <typename T>
  void moveToStackPtr(T t) {
    movePtr(t, getStackPointer());
  }
  template <typename T>
  void moveStackPtrTo(T t) {
    movePtr(getStackPointer(), t);
  }

  template <typename T>
  void loadStackPtr(T t) {
    loadPtr(t, getStackPointer());
  }
  template <typename T>
  void storeStackPtr(T t) {
    storePtr(getStackPointer(), t);
  }

  // StackPointer testing functions.
  // On ARM64, sp can function as the zero register depending on context.
  // Code shared across platforms must use these functions to be valid.
  template <typename T>
  inline void branchTestStackPtr(Condition cond, T t, Label* label);
  template <typename T>
  inline void branchStackPtr(Condition cond, T rhs, Label* label);
  template <typename T>
  inline void branchStackPtrRhs(Condition cond, T lhs, Label* label);

  // Move the stack pointer based on the requested amount.
  inline void reserveStack(uint32_t amount);
#else  // !JS_CODEGEN_ARM64
  void reserveStack(uint32_t amount);
#endif

 public:
  void enableProfilingInstrumentation() {
    emitProfilingInstrumentation_ = true;
  }

 private:
  // This class is used to surround call sites throughout the assembler. This
  // is used by callWithABI, and callJit functions, except if suffixed by
  // NoProfiler.
  class MOZ_RAII AutoProfilerCallInstrumentation {
   public:
    explicit AutoProfilerCallInstrumentation(MacroAssembler& masm);
    ~AutoProfilerCallInstrumentation() = default;
  };
  friend class AutoProfilerCallInstrumentation;

  void appendProfilerCallSite(CodeOffset label) {
    propagateOOM(profilerCallSites_.append(label));
  }

  // Fix up the code pointers to be written for locations where profilerCallSite
  // emitted moves of RIP to a register.
  void linkProfilerCallSites(JitCode* code);

  // This field is used to manage profiling instrumentation output. If
  // provided and enabled, then instrumentation will be emitted around call
  // sites.
  bool emitProfilingInstrumentation_;

  // Record locations of the call sites.
  Vector<CodeOffset, 0, SystemAllocPolicy> profilerCallSites_;

 public:
  void loadJitCodeRaw(Register func, Register dest);
  void loadBaselineJitCodeRaw(Register func, Register dest,
                              Label* failure = nullptr);
  void storeICScriptInJSContext(Register icScript);

  void loadBaselineFramePtr(Register framePtr, Register dest);

  void pushBaselineFramePtr(Register framePtr, Register scratch) {
    loadBaselineFramePtr(framePtr, scratch);
    push(scratch);
  }

  void PushBaselineFramePtr(Register framePtr, Register scratch) {
    loadBaselineFramePtr(framePtr, scratch);
    Push(scratch);
  }

  using MacroAssemblerSpecific::movePtr;

  void movePtr(TrampolinePtr ptr, Register dest) {
    movePtr(ImmPtr(ptr.value), dest);
  }

 private:
  void handleFailure();

 public:
  Label* exceptionLabel() {
    // Exceptions are currently handled the same way as sequential failures.
    return &failureLabel_;
  }

  Label* failureLabel() { return &failureLabel_; }

  void finish();
  void link(JitCode* code);

  void assumeUnreachable(const char* output);

  void printf(const char* output);
  void printf(const char* output, Register value);

#define DISPATCH_FLOATING_POINT_OP(method, type, arg1d, arg1f, arg2) \
  MOZ_ASSERT(IsFloatingPointType(type));                             \
  if (type == MIRType::Double)                                       \
    method##Double(arg1d, arg2);                                     \
  else                                                               \
    method##Float32(arg1f, arg2);

  void loadConstantFloatingPoint(double d, float f, FloatRegister dest,
                                 MIRType destType) {
    DISPATCH_FLOATING_POINT_OP(loadConstant, destType, d, f, dest);
  }
  void boolValueToFloatingPoint(ValueOperand value, FloatRegister dest,
                                MIRType destType) {
    DISPATCH_FLOATING_POINT_OP(boolValueTo, destType, value, value, dest);
  }
  void int32ValueToFloatingPoint(ValueOperand value, FloatRegister dest,
                                 MIRType destType) {
    DISPATCH_FLOATING_POINT_OP(int32ValueTo, destType, value, value, dest);
  }
  void convertInt32ToFloatingPoint(Register src, FloatRegister dest,
                                   MIRType destType) {
    DISPATCH_FLOATING_POINT_OP(convertInt32To, destType, src, src, dest);
  }

#undef DISPATCH_FLOATING_POINT_OP

  void convertValueToFloatingPoint(ValueOperand value, FloatRegister output,
                                   Label* fail, MIRType outputType);

  void outOfLineTruncateSlow(FloatRegister src, Register dest,
                             bool widenFloatToDouble, bool compilingWasm,
                             wasm::BytecodeOffset callOffset);

  void convertInt32ValueToDouble(ValueOperand val);

  void convertValueToDouble(ValueOperand value, FloatRegister output,
                            Label* fail) {
    convertValueToFloatingPoint(value, output, fail, MIRType::Double);
  }

  void convertValueToFloat(ValueOperand value, FloatRegister output,
                           Label* fail) {
    convertValueToFloatingPoint(value, output, fail, MIRType::Float32);
  }

  //
  // Functions for converting values to int.
  //
  void convertDoubleToInt(FloatRegister src, Register output,
                          FloatRegister temp, Label* truncateFail, Label* fail,
                          IntConversionBehavior behavior);

  // Strings may be handled by providing labels to jump to when the behavior
  // is truncation or clamping. The subroutine, usually an OOL call, is
  // passed the unboxed string in |stringReg| and should convert it to a
  // double store into |temp|.
  void convertValueToInt(
      ValueOperand value, Label* handleStringEntry, Label* handleStringRejoin,
      Label* truncateDoubleSlow, Register stringReg, FloatRegister temp,
      Register output, Label* fail, IntConversionBehavior behavior,
      IntConversionInputKind conversion = IntConversionInputKind::Any);

  // This carries over the MToNumberInt32 operation on the ValueOperand
  // input; see comment at the top of this class.
  void convertValueToInt32(
      ValueOperand value, FloatRegister temp, Register output, Label* fail,
      bool negativeZeroCheck,
      IntConversionInputKind conversion = IntConversionInputKind::Any) {
    convertValueToInt(
        value, nullptr, nullptr, nullptr, InvalidReg, temp, output, fail,
        negativeZeroCheck ? IntConversionBehavior::NegativeZeroCheck
                          : IntConversionBehavior::Normal,
        conversion);
  }

  // This carries over the MTruncateToInt32 operation on the ValueOperand
  // input; see the comment at the top of this class.
  void truncateValueToInt32(ValueOperand value, Label* handleStringEntry,
                            Label* handleStringRejoin,
                            Label* truncateDoubleSlow, Register stringReg,
                            FloatRegister temp, Register output, Label* fail) {
    convertValueToInt(value, handleStringEntry, handleStringRejoin,
                      truncateDoubleSlow, stringReg, temp, output, fail,
                      IntConversionBehavior::Truncate);
  }

  void truncateValueToInt32(ValueOperand value, FloatRegister temp,
                            Register output, Label* fail) {
    truncateValueToInt32(value, nullptr, nullptr, nullptr, InvalidReg, temp,
                         output, fail);
  }

  // Convenience functions for clamping values to uint8.
  void clampValueToUint8(ValueOperand value, Label* handleStringEntry,
                         Label* handleStringRejoin, Register stringReg,
                         FloatRegister temp, Register output, Label* fail) {
    convertValueToInt(value, handleStringEntry, handleStringRejoin, nullptr,
                      stringReg, temp, output, fail,
                      IntConversionBehavior::ClampToUint8);
  }

  [[nodiscard]] bool icBuildOOLFakeExitFrame(void* fakeReturnAddr,
                                             AutoSaveLiveRegisters& save);

  // Align the stack pointer based on the number of arguments which are pushed
  // on the stack, such that the JitFrameLayout would be correctly aligned on
  // the JitStackAlignment.
  void alignJitStackBasedOnNArgs(Register nargs, bool countIncludesThis);
  void alignJitStackBasedOnNArgs(uint32_t argc, bool countIncludesThis);

  inline void assertStackAlignment(uint32_t alignment, int32_t offset = 0);

  void touchFrameValues(Register numStackValues, Register scratch1,
                        Register scratch2);

#ifdef JS_64BIT
  // See comment block "64-bit GPRs carrying 32-bit values" above.  This asserts
  // that the high bits of the register are appropriate for the architecture and
  // the value in the low bits.
  void debugAssertCanonicalInt32(Register r);
#endif
};

// StackMacroAssembler checks no GC will happen while it's on the stack.
class MOZ_RAII StackMacroAssembler : public MacroAssembler {
  JS::AutoCheckCannotGC nogc;

 public:
  StackMacroAssembler(JSContext* cx, TempAllocator& alloc);
};

// WasmMacroAssembler does not contain GC pointers, so it doesn't need the no-GC
// checking StackMacroAssembler has.
class MOZ_RAII WasmMacroAssembler : public MacroAssembler {
 public:
  explicit WasmMacroAssembler(TempAllocator& alloc, bool limitedSize = true);
  explicit WasmMacroAssembler(TempAllocator& alloc,
                              const wasm::ModuleEnvironment& env,
                              bool limitedSize = true);
  ~WasmMacroAssembler() { assertNoGCThings(); }
};

// Heap-allocated MacroAssembler used for Ion off-thread code generation.
// GC cancels off-thread compilations.
class IonHeapMacroAssembler : public MacroAssembler {
 public:
  IonHeapMacroAssembler(TempAllocator& alloc, CompileRealm* realm);
};

//{{{ check_macroassembler_style
inline uint32_t MacroAssembler::framePushed() const { return framePushed_; }

inline void MacroAssembler::setFramePushed(uint32_t framePushed) {
  framePushed_ = framePushed;
}

inline void MacroAssembler::adjustFrame(int32_t value) {
  MOZ_ASSERT_IF(value < 0, framePushed_ >= uint32_t(-value));
  setFramePushed(framePushed_ + value);
}

inline void MacroAssembler::implicitPop(uint32_t bytes) {
  MOZ_ASSERT(bytes % sizeof(intptr_t) == 0);
  MOZ_ASSERT(bytes <= INT32_MAX);
  adjustFrame(-int32_t(bytes));
}
//}}} check_macroassembler_style

static inline Assembler::DoubleCondition JSOpToDoubleCondition(JSOp op) {
  switch (op) {
    case JSOp::Eq:
    case JSOp::StrictEq:
      return Assembler::DoubleEqual;
    case JSOp::Ne:
    case JSOp::StrictNe:
      return Assembler::DoubleNotEqualOrUnordered;
    case JSOp::Lt:
      return Assembler::DoubleLessThan;
    case JSOp::Le:
      return Assembler::DoubleLessThanOrEqual;
    case JSOp::Gt:
      return Assembler::DoubleGreaterThan;
    case JSOp::Ge:
      return Assembler::DoubleGreaterThanOrEqual;
    default:
      MOZ_CRASH("Unexpected comparison operation");
  }
}

// Note: the op may have been inverted during lowering (to put constants in a
// position where they can be immediates), so it is important to use the
// lir->jsop() instead of the mir->jsop() when it is present.
static inline Assembler::Condition JSOpToCondition(JSOp op, bool isSigned) {
  if (isSigned) {
    switch (op) {
      case JSOp::Eq:
      case JSOp::StrictEq:
        return Assembler::Equal;
      case JSOp::Ne:
      case JSOp::StrictNe:
        return Assembler::NotEqual;
      case JSOp::Lt:
        return Assembler::LessThan;
      case JSOp::Le:
        return Assembler::LessThanOrEqual;
      case JSOp::Gt:
        return Assembler::GreaterThan;
      case JSOp::Ge:
        return Assembler::GreaterThanOrEqual;
      default:
        MOZ_CRASH("Unrecognized comparison operation");
    }
  } else {
    switch (op) {
      case JSOp::Eq:
      case JSOp::StrictEq:
        return Assembler::Equal;
      case JSOp::Ne:
      case JSOp::StrictNe:
        return Assembler::NotEqual;
      case JSOp::Lt:
        return Assembler::Below;
      case JSOp::Le:
        return Assembler::BelowOrEqual;
      case JSOp::Gt:
        return Assembler::Above;
      case JSOp::Ge:
        return Assembler::AboveOrEqual;
      default:
        MOZ_CRASH("Unrecognized comparison operation");
    }
  }
}

static inline size_t StackDecrementForCall(uint32_t alignment,
                                           size_t bytesAlreadyPushed,
                                           size_t bytesToPush) {
  return bytesToPush +
         ComputeByteAlignment(bytesAlreadyPushed + bytesToPush, alignment);
}

// Helper for generatePreBarrier.
inline DynFn JitPreWriteBarrier(MIRType type);
}  // namespace jit

}  // namespace js

#endif /* jit_MacroAssembler_h */
