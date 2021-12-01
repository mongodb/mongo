/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MacroAssembler_h
#define jit_MacroAssembler_h

#include "mozilla/EndianUtils.h"
#include "mozilla/MacroForEach.h"
#include "mozilla/MathAlgorithms.h"

#include "vm/JSCompartment.h"

#if defined(JS_CODEGEN_X86)
# include "jit/x86/MacroAssembler-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/MacroAssembler-x64.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/MacroAssembler-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/MacroAssembler-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/MacroAssembler-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/MacroAssembler-mips64.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/MacroAssembler-none.h"
#else
# error "Unknown architecture!"
#endif
#include "jit/AtomicOp.h"
#include "jit/IonInstrumentation.h"
#include "jit/IonTypes.h"
#include "jit/JitCompartment.h"
#include "jit/VMFunctions.h"
#include "vm/ProxyObject.h"
#include "vm/Shape.h"
#include "vm/TypedArrayObject.h"
#include "vm/UnboxedObject.h"

using mozilla::FloatingPoint;

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
// The script check_macroassembler_style.py (check-masm target of the Makefile)
// is used to verify that method definitions are matching the annotation added
// to the method declarations.  If there is any difference, then you either
// forgot to define the method in one of the macro assembler, or you forgot to
// update the annotation of the macro assembler declaration.
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
// definition, for example like so:
//
//   //{{{ check_macroassembler_style
//   inline uint32_t
//   MacroAssembler::framePushed() const
//   {
//       return framePushed_;
//   }
//   ////}}} check_macroassembler_style


# define ALL_ARCH mips32, mips64, arm, arm64, x86, x64
# define ALL_SHARED_ARCH arm, arm64, x86_shared, mips_shared

// * How this macro works:
//
// DEFINED_ON is a macro which check if, for the current architecture, the
// method is defined on the macro assembler or not.
//
// For each architecture, we have a macro named DEFINED_ON_arch.  This macro is
// empty if this is not the current architecture.  Otherwise it must be either
// set to "define" or "crash" (only use for the none target so-far).
//
// The DEFINED_ON macro maps the list of architecture names given as argument to
// a list of macro names.  For example,
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
// which result is either no annotation, a MOZ_CRASH(), or a "= delete"
// annotation on the method declaration.

# define DEFINED_ON_x86
# define DEFINED_ON_x64
# define DEFINED_ON_x86_shared
# define DEFINED_ON_arm
# define DEFINED_ON_arm64
# define DEFINED_ON_mips32
# define DEFINED_ON_mips64
# define DEFINED_ON_mips_shared
# define DEFINED_ON_none

// Specialize for each architecture.
#if defined(JS_CODEGEN_X86)
# undef DEFINED_ON_x86
# define DEFINED_ON_x86 define
# undef DEFINED_ON_x86_shared
# define DEFINED_ON_x86_shared define
#elif defined(JS_CODEGEN_X64)
# undef DEFINED_ON_x64
# define DEFINED_ON_x64 define
# undef DEFINED_ON_x86_shared
# define DEFINED_ON_x86_shared define
#elif defined(JS_CODEGEN_ARM)
# undef DEFINED_ON_arm
# define DEFINED_ON_arm define
#elif defined(JS_CODEGEN_ARM64)
# undef DEFINED_ON_arm64
# define DEFINED_ON_arm64 define
#elif defined(JS_CODEGEN_MIPS32)
# undef DEFINED_ON_mips32
# define DEFINED_ON_mips32 define
# undef DEFINED_ON_mips_shared
# define DEFINED_ON_mips_shared define
#elif defined(JS_CODEGEN_MIPS64)
# undef DEFINED_ON_mips64
# define DEFINED_ON_mips64 define
# undef DEFINED_ON_mips_shared
# define DEFINED_ON_mips_shared define
#elif defined(JS_CODEGEN_NONE)
# undef DEFINED_ON_none
# define DEFINED_ON_none crash
#else
# error "Unknown architecture!"
#endif

# define DEFINED_ON_RESULT_crash   { MOZ_CRASH(); }
# define DEFINED_ON_RESULT_define
# define DEFINED_ON_RESULT_        = delete

# define DEFINED_ON_DISPATCH_RESULT_2(Macro, Result) \
    Macro ## Result
# define DEFINED_ON_DISPATCH_RESULT(...)     \
    DEFINED_ON_DISPATCH_RESULT_2(DEFINED_ON_RESULT_, __VA_ARGS__)

// We need to let the evaluation of MOZ_FOR_EACH terminates.
# define DEFINED_ON_EXPAND_ARCH_RESULTS_3(ParenResult)  \
    DEFINED_ON_DISPATCH_RESULT ParenResult
# define DEFINED_ON_EXPAND_ARCH_RESULTS_2(ParenResult)  \
    DEFINED_ON_EXPAND_ARCH_RESULTS_3 (ParenResult)
# define DEFINED_ON_EXPAND_ARCH_RESULTS(ParenResult)    \
    DEFINED_ON_EXPAND_ARCH_RESULTS_2 (ParenResult)

# define DEFINED_ON_FWDARCH(Arch) DEFINED_ON_ ## Arch
# define DEFINED_ON_MAP_ON_ARCHS(ArchList)              \
    DEFINED_ON_EXPAND_ARCH_RESULTS(                     \
      (MOZ_FOR_EACH(DEFINED_ON_FWDARCH, (), ArchList)))

# define DEFINED_ON(...)                                \
    DEFINED_ON_MAP_ON_ARCHS((none, __VA_ARGS__))

# define PER_ARCH DEFINED_ON(ALL_ARCH)
# define PER_SHARED_ARCH DEFINED_ON(ALL_SHARED_ARCH)
# define OOL_IN_HEADER

#if MOZ_LITTLE_ENDIAN
#define IMM32_16ADJ(X) (X) << 16
#else
#define IMM32_16ADJ(X) (X)
#endif

namespace js {
namespace jit {

// Defined in JitFrames.h
enum class ExitFrameType : uint8_t;

class AutoSaveLiveRegisters;

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

enum class CharEncoding { Latin1, TwoByte };

// The public entrypoint for emitting assembly. Note that a MacroAssembler can
// use cx->lifoAlloc, so take care not to interleave masm use with other
// lifoAlloc use if one will be destroyed before the other.
class MacroAssembler : public MacroAssemblerSpecific
{
    MacroAssembler* thisFromCtor() {
        return this;
    }

  public:
    class AutoRooter : public JS::AutoGCRooter
    {
        MacroAssembler* masm_;

      public:
        AutoRooter(JSContext* cx, MacroAssembler* masm)
          : JS::AutoGCRooter(cx, IONMASM),
            masm_(masm)
        { }

        MacroAssembler* masm() const {
            return masm_;
        }
    };

    /*
     * Base class for creating a branch.
     */
    class Branch
    {
        bool init_;
        Condition cond_;
        Label* jump_;
        Register reg_;

      public:
        Branch()
          : init_(false),
            cond_(Equal),
            jump_(nullptr),
            reg_(Register::FromCode(0))      // Quell compiler warnings.
        { }

        Branch(Condition cond, Register reg, Label* jump)
          : init_(true),
            cond_(cond),
            jump_(jump),
            reg_(reg)
        { }

        bool isInitialized() const {
            return init_;
        }

        Condition cond() const {
            return cond_;
        }

        Label* jump() const {
            return jump_;
        }

        Register reg() const {
            return reg_;
        }

        void invertCondition() {
            cond_ = InvertCondition(cond_);
        }

        void relink(Label* jump) {
            jump_ = jump;
        }
    };

    /*
     * Creates a branch based on a GCPtr.
     */
    class BranchGCPtr : public Branch
    {
        ImmGCPtr ptr_;

      public:
        BranchGCPtr()
          : Branch(),
            ptr_(ImmGCPtr(nullptr))
        { }

        BranchGCPtr(Condition cond, Register reg, ImmGCPtr ptr, Label* jump)
          : Branch(cond, reg, jump),
            ptr_(ptr)
        { }

        void emit(MacroAssembler& masm);
    };

    mozilla::Maybe<AutoRooter> autoRooter_;
    mozilla::Maybe<JitContext> jitContext_;
    mozilla::Maybe<AutoJitContextAlloc> alloc_;

  private:
    // Labels for handling exceptions and failures.
    NonAssertingLabel failureLabel_;

  public:
    MacroAssembler()
      : framePushed_(0),
#ifdef DEBUG
        inCall_(false),
#endif
        emitProfilingInstrumentation_(false)
    {
        JitContext* jcx = GetJitContext();
        JSContext* cx = jcx->cx;
        if (cx)
            constructRoot(cx);

        if (!jcx->temp) {
            MOZ_ASSERT(cx);
            alloc_.emplace(cx);
        }

        moveResolver_.setAllocator(*jcx->temp);

#if defined(JS_CODEGEN_ARM)
        initWithAllocator();
        m_buffer.id = jcx->getNextAssemblerId();
#elif defined(JS_CODEGEN_ARM64)
        initWithAllocator();
        armbuffer_.id = jcx->getNextAssemblerId();
#endif
    }

    // This constructor should only be used when there is no JitContext active
    // (for example, Trampoline-$(ARCH).cpp and IonCaches.cpp).
    explicit MacroAssembler(JSContext* cx, IonScript* ion = nullptr,
                            JSScript* script = nullptr, jsbytecode* pc = nullptr);

    // wasm compilation handles its own JitContext-pushing
    struct WasmToken {};
    explicit MacroAssembler(WasmToken, TempAllocator& alloc)
      : framePushed_(0),
#ifdef DEBUG
        inCall_(false),
#endif
        emitProfilingInstrumentation_(false)
    {
        moveResolver_.setAllocator(alloc);

#if defined(JS_CODEGEN_ARM)
        initWithAllocator();
        m_buffer.id = 0;
#elif defined(JS_CODEGEN_ARM64)
        initWithAllocator();
        armbuffer_.id = 0;
#endif
    }

#ifdef DEBUG
    bool isRooted() const {
        return autoRooter_.isSome();
    }
#endif

    void constructRoot(JSContext* cx) {
        autoRooter_.emplace(cx, this);
    }

    MoveResolver& moveResolver() {
        return moveResolver_;
    }

    size_t instructionsSize() const {
        return size();
    }

#ifdef JS_HAS_HIDDEN_SP
    void Push(RegisterOrSP reg);
#endif

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
    // Stack manipulation functions.

    void PushRegsInMask(LiveRegisterSet set)
                            DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);
    void PushRegsInMask(LiveGeneralRegisterSet set);

    // Like PushRegsInMask, but instead of pushing the registers, store them to
    // |dest|. |dest| should point to the end of the reserved space, so the
    // first register will be stored at |dest.offset - sizeof(register)|.
    void storeRegsInMask(LiveRegisterSet set, Address dest, Register scratch)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    void PopRegsInMask(LiveRegisterSet set);
    void PopRegsInMask(LiveGeneralRegisterSet set);
    void PopRegsInMaskIgnore(LiveRegisterSet set, LiveRegisterSet ignore)
                                 DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    void Push(const Operand op) DEFINED_ON(x86_shared);
    void Push(Register reg) PER_SHARED_ARCH;
    void Push(Register reg1, Register reg2, Register reg3, Register reg4) DEFINED_ON(arm64);
    void Push(const Imm32 imm) PER_SHARED_ARCH;
    void Push(const ImmWord imm) PER_SHARED_ARCH;
    void Push(const ImmPtr imm) PER_SHARED_ARCH;
    void Push(const ImmGCPtr ptr) PER_SHARED_ARCH;
    void Push(FloatRegister reg) PER_SHARED_ARCH;
    void PushFlags() DEFINED_ON(x86_shared);
    void Push(jsid id, Register scratchReg);
    void Push(TypedOrValueRegister v);
    void Push(const ConstantOrRegister& v);
    void Push(const ValueOperand& val);
    void Push(const Value& val);
    void Push(JSValueType type, Register reg);
    void PushValue(const Address& addr);
    void PushEmptyRooted(VMFunction::RootType rootType);
    inline CodeOffset PushWithPatch(ImmWord word);
    inline CodeOffset PushWithPatch(ImmPtr imm);

    void Pop(const Operand op) DEFINED_ON(x86_shared);
    void Pop(Register reg) PER_SHARED_ARCH;
    void Pop(FloatRegister t) PER_SHARED_ARCH;
    void Pop(const ValueOperand& val) PER_SHARED_ARCH;
    void PopFlags() DEFINED_ON(x86_shared);
    void PopStackPtr() PER_SHARED_ARCH;
    void popRooted(VMFunction::RootType rootType, Register cellReg, const ValueOperand& valueReg);

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
#endif // DEBUG

  public:
    // ===============================================================
    // Simple call functions.

    CodeOffset call(Register reg) PER_SHARED_ARCH;
    CodeOffset call(Label* label) PER_SHARED_ARCH;
    void call(const Address& addr) PER_SHARED_ARCH;
    void call(ImmWord imm) PER_SHARED_ARCH;
    // Call a target native function, which is neither traceable nor movable.
    void call(ImmPtr imm) PER_SHARED_ARCH;
    void call(wasm::SymbolicAddress imm) PER_SHARED_ARCH;
    inline void call(const wasm::CallSiteDesc& desc, wasm::SymbolicAddress imm);

    // Call a target JitCode, which must be traceable, and may be movable.
    void call(JitCode* c) PER_SHARED_ARCH;

    inline void call(TrampolinePtr code);

    inline void call(const wasm::CallSiteDesc& desc, const Register reg);
    inline void call(const wasm::CallSiteDesc& desc, uint32_t funcDefIndex);
    inline void call(const wasm::CallSiteDesc& desc, wasm::Trap trap);

    CodeOffset callWithPatch() PER_SHARED_ARCH;
    void patchCall(uint32_t callerOffset, uint32_t calleeOffset) PER_SHARED_ARCH;

    // Push the return address and make a call. On platforms where this function
    // is not defined, push the link register (pushReturnAddress) at the entry
    // point of the callee.
    void callAndPushReturnAddress(Register reg) DEFINED_ON(x86_shared);
    void callAndPushReturnAddress(Label* label) DEFINED_ON(x86_shared);

    void pushReturnAddress() DEFINED_ON(mips_shared, arm, arm64);
    void popReturnAddress() DEFINED_ON(mips_shared, arm, arm64);

  public:
    // ===============================================================
    // Patchable near/far jumps.

    // "Far jumps" provide the ability to jump to any uint32_t offset from any
    // other uint32_t offset without using a constant pool (thus returning a
    // simple CodeOffset instead of a CodeOffsetJump).
    CodeOffset farJumpWithPatch() PER_SHARED_ARCH;
    void patchFarJump(CodeOffset farJump, uint32_t targetOffset) PER_SHARED_ARCH;
    static void repatchFarJump(uint8_t* code, uint32_t farJumpOffset, uint32_t targetOffset) PER_SHARED_ARCH;

    // Emit a nop that can be patched to and from a nop and a jump with an int8
    // relative displacement.
    CodeOffset nopPatchableToNearJump() PER_SHARED_ARCH;
    static void patchNopToNearJump(uint8_t* jump, uint8_t* target) PER_SHARED_ARCH;
    static void patchNearJumpToNop(uint8_t* jump) PER_SHARED_ARCH;

    // Emit a nop that can be patched to and from a nop and a call with int32
    // relative displacement.
    CodeOffset nopPatchableToCall(const wasm::CallSiteDesc& desc) PER_SHARED_ARCH;
    static void patchNopToCall(uint8_t* callsite, uint8_t* target) PER_SHARED_ARCH;
    static void patchCallToNop(uint8_t* callsite) PER_SHARED_ARCH;

  public:
    // ===============================================================
    // ABI function calls.

    // Setup a call to C/C++ code, given the assumption that the framePushed
    // accruately define the state of the stack, and that the top of the stack
    // was properly aligned. Note that this only supports cdecl.
    void setupAlignedABICall(); // CRASH_ON(arm64)

    // As setupAlignedABICall, but for WebAssembly native ABI calls, which pass
    // through a builtin thunk that uses the wasm ABI. All the wasm ABI calls
    // can be native, since we always know the stack alignment a priori.
    void setupWasmABICall(); // CRASH_ON(arm64)

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

    inline void callWithABI(void* fun, MoveOp::Type result = MoveOp::GENERAL,
                            CheckUnsafeCallWithABI check = CheckUnsafeCallWithABI::Check);
    inline void callWithABI(Register fun, MoveOp::Type result = MoveOp::GENERAL);
    inline void callWithABI(const Address& fun, MoveOp::Type result = MoveOp::GENERAL);

    void callWithABI(wasm::BytecodeOffset offset, wasm::SymbolicAddress fun,
                     MoveOp::Type result = MoveOp::GENERAL);

  private:
    // Reinitialize the variables which have to be cleared before making a call
    // with callWithABI.
    void setupABICall();

    // Reserve the stack and resolve the arguments move.
    void callWithABIPre(uint32_t* stackAdjust, bool callFromWasm = false) PER_ARCH;

    // Emits a call to a C/C++ function, resolving all argument moves.
    void callWithABINoProfiler(void* fun, MoveOp::Type result, CheckUnsafeCallWithABI check);
    void callWithABINoProfiler(Register fun, MoveOp::Type result) PER_ARCH;
    void callWithABINoProfiler(const Address& fun, MoveOp::Type result) PER_ARCH;

    // Restore the stack to its state before the setup function call.
    void callWithABIPost(uint32_t stackAdjust, MoveOp::Type result, bool callFromWasm = false) PER_ARCH;

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

    // The frame descriptor is the second field of all Jit frames, pushed before
    // calling the Jit function.  It is a composite value defined in JitFrames.h
    inline void makeFrameDescriptor(Register frameSizeReg, FrameType type, uint32_t headerSize);

    // Push the frame descriptor, based on the statically known framePushed.
    inline void pushStaticFrameDescriptor(FrameType type, uint32_t headerSize);

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
    // This function returns the offset of the /fake/ return address, in order to use
    // the return address to index the safepoints, which are used to list all
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
    // See JitFrames.h, and MarkJitExitFrame in JitFrames.cpp.

    // Push stub code and the VMFunction pointer.
    inline void enterExitFrame(Register cxreg, Register scratch, const VMFunction* f);

    // Push an exit frame token to identify which fake exit frame this footer
    // corresponds to.
    inline void enterFakeExitFrame(Register cxreg, Register scratch, ExitFrameType type);

    // Push an exit frame token for a native call.
    inline void enterFakeExitFrameForNative(Register cxreg, Register scratch, bool isConstructing);

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

    inline void moveFloat32ToGPR(FloatRegister src, Register dest) PER_SHARED_ARCH;
    inline void moveGPRToFloat32(Register src, FloatRegister dest) PER_SHARED_ARCH;

    inline void moveDoubleToGPR64(FloatRegister src, Register64 dest) PER_ARCH;
    inline void moveGPR64ToDouble(Register64 src, FloatRegister dest) PER_ARCH;

    inline void move8SignExtend(Register src, Register dest) PER_SHARED_ARCH;
    inline void move16SignExtend(Register src, Register dest) PER_SHARED_ARCH;

    // move64To32 will clear the high bits of `dest` on 64-bit systems.
    inline void move64To32(Register64 src, Register dest) PER_ARCH;

    inline void move32To64ZeroExtend(Register src, Register64 dest) PER_ARCH;

    // On x86, `dest` must be edx:eax for the sign extend operations.
    inline void move8To64SignExtend(Register src, Register64 dest) PER_ARCH;
    inline void move16To64SignExtend(Register src, Register64 dest) PER_ARCH;
    inline void move32To64SignExtend(Register src, Register64 dest) PER_ARCH;

    // Copy a constant, typed-register, or a ValueOperand into a ValueOperand
    // destination.
    inline void moveValue(const ConstantOrRegister& src, const ValueOperand& dest);
    void moveValue(const TypedOrValueRegister& src, const ValueOperand& dest) PER_ARCH;
    void moveValue(const ValueOperand& src, const ValueOperand& dest) PER_ARCH;
    void moveValue(const Value& src, const ValueOperand& dest) PER_ARCH;

  public:
    // ===============================================================
    // Logical instructions

    inline void not32(Register reg) PER_SHARED_ARCH;

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

    inline void xorPtr(Register src, Register dest) PER_ARCH;
    inline void xorPtr(Imm32 imm, Register dest) PER_ARCH;

    inline void and64(const Operand& src, Register64 dest) DEFINED_ON(x64, mips64);
    inline void or64(const Operand& src, Register64 dest) DEFINED_ON(x64, mips64);
    inline void xor64(const Operand& src, Register64 dest) DEFINED_ON(x64, mips64);

    // ===============================================================
    // Arithmetic functions

    inline void add32(Register src, Register dest) PER_SHARED_ARCH;
    inline void add32(Imm32 imm, Register dest) PER_SHARED_ARCH;
    inline void add32(Imm32 imm, const Address& dest) PER_SHARED_ARCH;
    inline void add32(Imm32 imm, const AbsoluteAddress& dest) DEFINED_ON(x86_shared);

    inline void addPtr(Register src, Register dest) PER_ARCH;
    inline void addPtr(Register src1, Register src2, Register dest) DEFINED_ON(arm64);
    inline void addPtr(Imm32 imm, Register dest) PER_ARCH;
    inline void addPtr(Imm32 imm, Register src, Register dest) DEFINED_ON(arm64);
    inline void addPtr(ImmWord imm, Register dest) PER_ARCH;
    inline void addPtr(ImmPtr imm, Register dest);
    inline void addPtr(Imm32 imm, const Address& dest) DEFINED_ON(mips_shared, arm, arm64, x86, x64);
    inline void addPtr(Imm32 imm, const AbsoluteAddress& dest) DEFINED_ON(x86, x64);
    inline void addPtr(const Address& src, Register dest) DEFINED_ON(mips_shared, arm, arm64, x86, x64);

    inline void add64(Register64 src, Register64 dest) PER_ARCH;
    inline void add64(Imm32 imm, Register64 dest) PER_ARCH;
    inline void add64(Imm64 imm, Register64 dest) PER_ARCH;
    inline void add64(const Operand& src, Register64 dest) DEFINED_ON(x64, mips64);

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
    inline void subPtr(Register src, const Address& dest) DEFINED_ON(mips_shared, arm, arm64, x86, x64);
    inline void subPtr(Imm32 imm, Register dest) PER_ARCH;
    inline void subPtr(ImmWord imm, Register dest) DEFINED_ON(x64);
    inline void subPtr(const Address& addr, Register dest) DEFINED_ON(mips_shared, arm, arm64, x86, x64);

    inline void sub64(Register64 src, Register64 dest) PER_ARCH;
    inline void sub64(Imm64 imm, Register64 dest) PER_ARCH;
    inline void sub64(const Operand& src, Register64 dest) DEFINED_ON(x64, mips64);

    inline void subFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

    inline void subDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

    // On x86-shared, srcDest must be eax and edx will be clobbered.
    inline void mul32(Register rhs, Register srcDest) PER_SHARED_ARCH;

    inline void mul32(Register src1, Register src2, Register dest, Label* onOver, Label* onZero) DEFINED_ON(arm64);

    inline void mul64(const Operand& src, const Register64& dest) DEFINED_ON(x64);
    inline void mul64(const Operand& src, const Register64& dest, const Register temp)
        DEFINED_ON(x64, mips64);
    inline void mul64(Imm64 imm, const Register64& dest) PER_ARCH;
    inline void mul64(Imm64 imm, const Register64& dest, const Register temp)
        DEFINED_ON(x86, x64, arm, mips32, mips64);
    inline void mul64(const Register64& src, const Register64& dest, const Register temp)
        PER_ARCH;

    inline void mulBy3(Register src, Register dest) PER_ARCH;

    inline void mulFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;
    inline void mulDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

    inline void mulDoublePtr(ImmPtr imm, Register temp, FloatRegister dest) DEFINED_ON(mips_shared, arm, arm64, x86, x64);

    // Perform an integer division, returning the integer part rounded toward zero.
    // rhs must not be zero, and the division must not overflow.
    //
    // On x86_shared, srcDest must be eax and edx will be clobbered.
    // On ARM, the chip must have hardware division instructions.
    inline void quotient32(Register rhs, Register srcDest, bool isUnsigned) PER_SHARED_ARCH;

    // Perform an integer division, returning the remainder part.
    // rhs must not be zero, and the division must not overflow.
    //
    // On x86_shared, srcDest must be eax and edx will be clobbered.
    // On ARM, the chip must have hardware division instructions.
    inline void remainder32(Register rhs, Register srcDest, bool isUnsigned) PER_SHARED_ARCH;

    inline void divFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;
    inline void divDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

    inline void inc64(AbsoluteAddress dest) PER_ARCH;

    inline void neg32(Register reg) PER_SHARED_ARCH;
    inline void neg64(Register64 reg) DEFINED_ON(x86, x64, arm, mips32, mips64);

    inline void negateFloat(FloatRegister reg) PER_SHARED_ARCH;

    inline void negateDouble(FloatRegister reg) PER_SHARED_ARCH;

    inline void absFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;
    inline void absDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

    inline void sqrtFloat32(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;
    inline void sqrtDouble(FloatRegister src, FloatRegister dest) PER_SHARED_ARCH;

    // srcDest = {min,max}{Float32,Double}(srcDest, other)
    // For min and max, handle NaN specially if handleNaN is true.

    inline void minFloat32(FloatRegister other, FloatRegister srcDest, bool handleNaN) PER_SHARED_ARCH;
    inline void minDouble(FloatRegister other, FloatRegister srcDest, bool handleNaN) PER_SHARED_ARCH;

    inline void maxFloat32(FloatRegister other, FloatRegister srcDest, bool handleNaN) PER_SHARED_ARCH;
    inline void maxDouble(FloatRegister other, FloatRegister srcDest, bool handleNaN) PER_SHARED_ARCH;

    // ===============================================================
    // Shift functions

    // For shift-by-register there may be platform-specific
    // variations, for example, x86 will perform the shift mod 32 but
    // ARM will perform the shift mod 256.
    //
    // For shift-by-immediate the platform assembler may restrict the
    // immediate, for example, the ARM assembler requires the count
    // for 32-bit shifts to be in the range [0,31].

    inline void lshift32(Imm32 shift, Register srcDest) PER_SHARED_ARCH;
    inline void rshift32(Imm32 shift, Register srcDest) PER_SHARED_ARCH;
    inline void rshift32Arithmetic(Imm32 shift, Register srcDest) PER_SHARED_ARCH;

    inline void lshiftPtr(Imm32 imm, Register dest) PER_ARCH;
    inline void rshiftPtr(Imm32 imm, Register dest) PER_ARCH;
    inline void rshiftPtr(Imm32 imm, Register src, Register dest) DEFINED_ON(arm64);
    inline void rshiftPtrArithmetic(Imm32 imm, Register dest) PER_ARCH;

    inline void lshift64(Imm32 imm, Register64 dest) PER_ARCH;
    inline void rshift64(Imm32 imm, Register64 dest) PER_ARCH;
    inline void rshift64Arithmetic(Imm32 imm, Register64 dest) PER_ARCH;

    // On x86_shared these have the constraint that shift must be in CL.
    inline void lshift32(Register shift, Register srcDest) PER_SHARED_ARCH;
    inline void rshift32(Register shift, Register srcDest) PER_SHARED_ARCH;
    inline void rshift32Arithmetic(Register shift, Register srcDest) PER_SHARED_ARCH;

    inline void lshift64(Register shift, Register64 srcDest) PER_ARCH;
    inline void rshift64(Register shift, Register64 srcDest) PER_ARCH;
    inline void rshift64Arithmetic(Register shift, Register64 srcDest) PER_ARCH;

    // ===============================================================
    // Rotation functions
    // Note: - on x86 and x64 the count register must be in CL.
    //       - on x64 the temp register should be InvalidReg.

    inline void rotateLeft(Imm32 count, Register input, Register dest) PER_SHARED_ARCH;
    inline void rotateLeft(Register count, Register input, Register dest) PER_SHARED_ARCH;
    inline void rotateLeft64(Imm32 count, Register64 input, Register64 dest) DEFINED_ON(x64);
    inline void rotateLeft64(Register count, Register64 input, Register64 dest) DEFINED_ON(x64);
    inline void rotateLeft64(Imm32 count, Register64 input, Register64 dest, Register temp)
        PER_ARCH;
    inline void rotateLeft64(Register count, Register64 input, Register64 dest, Register temp)
        PER_ARCH;

    inline void rotateRight(Imm32 count, Register input, Register dest) PER_SHARED_ARCH;
    inline void rotateRight(Register count, Register input, Register dest) PER_SHARED_ARCH;
    inline void rotateRight64(Imm32 count, Register64 input, Register64 dest) DEFINED_ON(x64);
    inline void rotateRight64(Register count, Register64 input, Register64 dest) DEFINED_ON(x64);
    inline void rotateRight64(Imm32 count, Register64 input, Register64 dest, Register temp)
        PER_ARCH;
    inline void rotateRight64(Register count, Register64 input, Register64 dest, Register temp)
        PER_ARCH;

    // ===============================================================
    // Bit counting functions

    // knownNotZero may be true only if the src is known not to be zero.
    inline void clz32(Register src, Register dest, bool knownNotZero) PER_SHARED_ARCH;
    inline void ctz32(Register src, Register dest, bool knownNotZero) PER_SHARED_ARCH;

    inline void clz64(Register64 src, Register dest) PER_ARCH;
    inline void ctz64(Register64 src, Register dest) PER_ARCH;

    // On x86_shared, temp may be Invalid only if the chip has the POPCNT instruction.
    // On ARM, temp may never be Invalid.
    inline void popcnt32(Register src, Register dest, Register temp) PER_SHARED_ARCH;

    // temp may be invalid only if the chip has the POPCNT instruction.
    inline void popcnt64(Register64 src, Register64 dest, Register temp) PER_ARCH;

    // ===============================================================
    // Condition functions

    template <typename T1, typename T2>
    inline void cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest)
        DEFINED_ON(x86_shared, arm, arm64, mips32, mips64);

    template <typename T1, typename T2>
    inline void cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest)
        PER_ARCH;

    // ===============================================================
    // Branch functions

    template <class L>
    inline void branch32(Condition cond, Register lhs, Register rhs, L label) PER_SHARED_ARCH;
    template <class L>
    inline void branch32(Condition cond, Register lhs, Imm32 rhs, L label) PER_SHARED_ARCH;

    inline void branch32(Condition cond, const Address& lhs, Register rhs, Label* label) PER_SHARED_ARCH;
    inline void branch32(Condition cond, const Address& lhs, Imm32 rhs, Label* label) PER_SHARED_ARCH;

    inline void branch32(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);
    inline void branch32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    inline void branch32(Condition cond, const BaseIndex& lhs, Register rhs, Label* label)
        DEFINED_ON(x86_shared);
    inline void branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs, Label* label) PER_SHARED_ARCH;

    inline void branch32(Condition cond, const Operand& lhs, Register rhs, Label* label) DEFINED_ON(x86_shared);
    inline void branch32(Condition cond, const Operand& lhs, Imm32 rhs, Label* label) DEFINED_ON(x86_shared);

    inline void branch32(Condition cond, wasm::SymbolicAddress lhs, Imm32 rhs, Label* label)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    // The supported condition are Equal, NotEqual, LessThan(orEqual), GreaterThan(orEqual),
    // Below(orEqual) and Above(orEqual).
    // When a fail label is not defined it will fall through to next instruction,
    // else jump to the fail label.
    inline void branch64(Condition cond, Register64 lhs, Imm64 val, Label* success,
                         Label* fail = nullptr) PER_ARCH;
    inline void branch64(Condition cond, Register64 lhs, Register64 rhs, Label* success,
                         Label* fail = nullptr) PER_ARCH;
    // On x86 and x64 NotEqual and Equal conditions are allowed for the branch64 variants
    // with Address as lhs. On others only the NotEqual condition.
    inline void branch64(Condition cond, const Address& lhs, Imm64 val, Label* label) PER_ARCH;

    // Compare the value at |lhs| with the value at |rhs|.  The scratch
    // register *must not* be the base of |lhs| or |rhs|.
    inline void branch64(Condition cond, const Address& lhs, const Address& rhs, Register scratch,
                         Label* label) PER_ARCH;

    template <class L>
    inline void branchPtr(Condition cond, Register lhs, Register rhs, L label) PER_SHARED_ARCH;
    inline void branchPtr(Condition cond, Register lhs, Imm32 rhs, Label* label) PER_SHARED_ARCH;
    inline void branchPtr(Condition cond, Register lhs, ImmPtr rhs, Label* label) PER_SHARED_ARCH;
    inline void branchPtr(Condition cond, Register lhs, ImmGCPtr rhs, Label* label) PER_SHARED_ARCH;
    inline void branchPtr(Condition cond, Register lhs, ImmWord rhs, Label* label) PER_SHARED_ARCH;

    template <class L>
    inline void branchPtr(Condition cond, const Address& lhs, Register rhs, L label) PER_SHARED_ARCH;
    inline void branchPtr(Condition cond, const Address& lhs, ImmPtr rhs, Label* label) PER_SHARED_ARCH;
    inline void branchPtr(Condition cond, const Address& lhs, ImmGCPtr rhs, Label* label) PER_SHARED_ARCH;
    inline void branchPtr(Condition cond, const Address& lhs, ImmWord rhs, Label* label) PER_SHARED_ARCH;

    inline void branchPtr(Condition cond, const BaseIndex& lhs, ImmWord rhs, Label* label) PER_SHARED_ARCH;

    inline void branchPtr(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);
    inline void branchPtr(Condition cond, const AbsoluteAddress& lhs, ImmWord rhs, Label* label)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    inline void branchPtr(Condition cond, wasm::SymbolicAddress lhs, Register rhs, Label* label)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    // Given a pointer to a GC Cell, retrieve the StoreBuffer pointer from its
    // chunk trailer, or nullptr if it is in the tenured heap.
    void loadStoreBuffer(Register ptr, Register buffer) PER_ARCH;

    template <typename T>
    inline CodeOffsetJump branchPtrWithPatch(Condition cond, Register lhs, T rhs, RepatchLabel* label) PER_SHARED_ARCH;
    template <typename T>
    inline CodeOffsetJump branchPtrWithPatch(Condition cond, Address lhs, T rhs, RepatchLabel* label) PER_SHARED_ARCH;

    void branchPtrInNurseryChunk(Condition cond, Register ptr, Register temp, Label* label)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);
    void branchPtrInNurseryChunk(Condition cond, const Address& address, Register temp, Label* label)
        DEFINED_ON(x86);
    void branchValueIsNurseryObject(Condition cond, ValueOperand value, Register temp, Label* label) PER_ARCH;
    void branchValueIsNurseryCell(Condition cond, const Address& address, Register temp, Label* label) PER_ARCH;
    void branchValueIsNurseryCell(Condition cond, ValueOperand value, Register temp, Label* label) PER_ARCH;

    // This function compares a Value (lhs) which is having a private pointer
    // boxed inside a js::Value, with a raw pointer (rhs).
    inline void branchPrivatePtr(Condition cond, const Address& lhs, Register rhs, Label* label) PER_ARCH;

    inline void branchFloat(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                            Label* label) PER_SHARED_ARCH;

    // Truncate a double/float32 to int32 and when it doesn't fit an int32 it will jump to
    // the failure label. This particular variant is allowed to return the value module 2**32,
    // which isn't implemented on all architectures.
    // E.g. the x64 variants will do this only in the int64_t range.
    inline void branchTruncateFloat32MaybeModUint32(FloatRegister src, Register dest, Label* fail)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);
    inline void branchTruncateDoubleMaybeModUint32(FloatRegister src, Register dest, Label* fail)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    // Truncate a double/float32 to intptr and when it doesn't fit jump to the failure label.
    inline void branchTruncateFloat32ToPtr(FloatRegister src, Register dest, Label* fail)
        DEFINED_ON(x86, x64);
    inline void branchTruncateDoubleToPtr(FloatRegister src, Register dest, Label* fail)
        DEFINED_ON(x86, x64);

    // Truncate a double/float32 to int32 and when it doesn't fit jump to the failure label.
    inline void branchTruncateFloat32ToInt32(FloatRegister src, Register dest, Label* fail)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);
    inline void branchTruncateDoubleToInt32(FloatRegister src, Register dest, Label* fail)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    inline void branchDouble(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                             Label* label) PER_SHARED_ARCH;

    inline void branchDoubleNotInInt64Range(Address src, Register temp, Label* fail);
    inline void branchDoubleNotInUInt64Range(Address src, Register temp, Label* fail);
    inline void branchFloat32NotInInt64Range(Address src, Register temp, Label* fail);
    inline void branchFloat32NotInUInt64Range(Address src, Register temp, Label* fail);

    template <typename T, typename L>
    inline void branchAdd32(Condition cond, T src, Register dest, L label) PER_SHARED_ARCH;
    template <typename T>
    inline void branchSub32(Condition cond, T src, Register dest, Label* label) PER_SHARED_ARCH;

    inline void decBranchPtr(Condition cond, Register lhs, Imm32 rhs, Label* label) PER_SHARED_ARCH;

    template <class L>
    inline void branchTest32(Condition cond, Register lhs, Register rhs, L label) PER_SHARED_ARCH;
    template <class L>
    inline void branchTest32(Condition cond, Register lhs, Imm32 rhs, L label) PER_SHARED_ARCH;
    inline void branchTest32(Condition cond, const Address& lhs, Imm32 rhh, Label* label) PER_SHARED_ARCH;
    inline void branchTest32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    template <class L>
    inline void branchTestPtr(Condition cond, Register lhs, Register rhs, L label) PER_SHARED_ARCH;
    inline void branchTestPtr(Condition cond, Register lhs, Imm32 rhs, Label* label) PER_SHARED_ARCH;
    inline void branchTestPtr(Condition cond, const Address& lhs, Imm32 rhs, Label* label) PER_SHARED_ARCH;

    template <class L>
    inline void branchTest64(Condition cond, Register64 lhs, Register64 rhs, Register temp,
                             L label) PER_ARCH;

    // Branches to |label| if |reg| is false. |reg| should be a C++ bool.
    template <class L>
    inline void branchIfFalseBool(Register reg, L label);

    // Branches to |label| if |reg| is true. |reg| should be a C++ bool.
    inline void branchIfTrueBool(Register reg, Label* label);

    inline void branchIfRope(Register str, Label* label);
    inline void branchIfRopeOrExternal(Register str, Register temp, Label* label);

    inline void branchIfNotRope(Register str, Label* label);

    inline void branchLatin1String(Register string, Label* label);
    inline void branchTwoByteString(Register string, Label* label);

    inline void branchIfFunctionHasNoJitEntry(Register fun, bool isConstructing, Label* label);
    inline void branchIfInterpreted(Register fun, Label* label);

    inline void branchFunctionKind(Condition cond, JSFunction::FunctionKind kind, Register fun,
                                   Register scratch, Label* label);

    void branchIfNotInterpretedConstructor(Register fun, Register scratch, Label* label);

    inline void branchIfObjectEmulatesUndefined(Register objReg, Register scratch, Label* slowCheck,
                                                Label* label);

    // For all methods below: spectreRegToZero is a register that will be zeroed
    // on speculatively executed code paths (when the branch should be taken but
    // branch prediction speculates it isn't). Usually this will be the object
    // register but the caller may pass a different register.

    inline void branchTestObjClass(Condition cond, Register obj, const js::Class* clasp,
                                   Register scratch, Register spectreRegToZero, Label* label);
    inline void branchTestObjClassNoSpectreMitigations(Condition cond, Register obj,
                                                       const js::Class* clasp, Register scratch,
                                                       Label* label);

    inline void branchTestObjClass(Condition cond, Register obj, const Address& clasp,
                                   Register scratch, Register spectreRegToZero, Label* label);
    inline void branchTestObjClassNoSpectreMitigations(Condition cond, Register obj,
                                                       const Address& clasp, Register scratch,
                                                       Label* label);

    inline void branchTestObjShape(Condition cond, Register obj, const Shape* shape,
                                   Register scratch, Register spectreRegToZero, Label* label);
    inline void branchTestObjShapeNoSpectreMitigations(Condition cond, Register obj,
                                                       const Shape* shape, Label* label);

    inline void branchTestObjShape(Condition cond, Register obj, Register shape, Register scratch,
                                   Register spectreRegToZero, Label* label);
    inline void branchTestObjShapeNoSpectreMitigations(Condition cond, Register obj,
                                                       Register shape, Label* label);

    inline void branchTestObjGroup(Condition cond, Register obj, const ObjectGroup* group,
                                   Register scratch, Register spectreRegToZero, Label* label);
    inline void branchTestObjGroupNoSpectreMitigations(Condition cond, Register obj,
                                                       const ObjectGroup* group, Label* label);

    inline void branchTestObjGroup(Condition cond, Register obj, Register group, Register scratch,
                                   Register spectreRegToZero, Label* label);
    inline void branchTestObjGroupNoSpectreMitigations(Condition cond, Register obj,
                                                       Register group, Label* label);

    void branchTestObjGroup(Condition cond, Register obj, const Address& group, Register scratch,
                            Register spectreRegToZero, Label* label);
    void branchTestObjGroupNoSpectreMitigations(Condition cond, Register obj, const Address& group,
                                                Register scratch, Label* label);

    // TODO: audit/fix callers to be Spectre safe.
    inline void branchTestObjShapeUnsafe(Condition cond, Register obj, Register shape, Label* label);
    inline void branchTestObjGroupUnsafe(Condition cond, Register obj, const ObjectGroup* group,
                                         Label* label);

    void branchTestObjCompartment(Condition cond, Register obj, const Address& compartment,
                                  Register scratch, Label* label);
    void branchTestObjCompartment(Condition cond, Register obj, const JSCompartment* compartment,
                                  Register scratch, Label* label);
    void branchIfObjGroupHasNoAddendum(Register obj, Register scratch, Label* label);
    void branchIfPretenuredGroup(const ObjectGroup* group, Register scratch, Label* label);

    void branchIfNonNativeObj(Register obj, Register scratch, Label* label);

    void branchIfInlineTypedObject(Register obj, Register scratch, Label* label);

    void branchIfNotSimdObject(Register obj, Register scratch, SimdType simdType, Label* label);

    inline void branchTestClassIsProxy(bool proxy, Register clasp, Label* label);

    inline void branchTestObjectIsProxy(bool proxy, Register object, Register scratch, Label* label);

    inline void branchTestProxyHandlerFamily(Condition cond, Register proxy, Register scratch,
                                             const void* handlerp, Label* label);

    void copyObjGroupNoPreBarrier(Register sourceObj, Register destObj, Register scratch);

    void loadTypedObjectDescr(Register obj, Register dest);
    void loadTypedObjectLength(Register obj, Register dest);

    // Emit type case branch on tag matching if the type tag in the definition
    // might actually be that type.
    void maybeBranchTestType(MIRType type, MDefinition* maybeDef, Register tag, Label* label);

    inline void branchTestNeedsIncrementalBarrier(Condition cond, Label* label);

    // Perform a type-test on a tag of a Value (32bits boxing), or the tagged
    // value (64bits boxing).
    inline void branchTestUndefined(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;
    inline void branchTestInt32(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;
    inline void branchTestDouble(Condition cond, Register tag, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);
    inline void branchTestNumber(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;
    inline void branchTestBoolean(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;
    inline void branchTestString(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;
    inline void branchTestSymbol(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;
    inline void branchTestNull(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;
    inline void branchTestObject(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;
    inline void branchTestPrimitive(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;
    inline void branchTestMagic(Condition cond, Register tag, Label* label) PER_SHARED_ARCH;

    // Perform a type-test on a Value, addressed by Address or BaseIndex, or
    // loaded into ValueOperand.
    // BaseIndex and ValueOperand variants clobber the ScratchReg on x64.
    // All Variants clobber the ScratchReg on arm64.
    inline void branchTestUndefined(Condition cond, const Address& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestUndefined(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestUndefined(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestInt32(Condition cond, const Address& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestInt32(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestInt32(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestDouble(Condition cond, const Address& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestDouble(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestDouble(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestNumber(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestBoolean(Condition cond, const Address& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestBoolean(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestBoolean(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestString(Condition cond, const Address& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestString(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestString(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestSymbol(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestSymbol(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestNull(Condition cond, const Address& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestNull(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestNull(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    // Clobbers the ScratchReg on x64.
    inline void branchTestObject(Condition cond, const Address& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestObject(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestObject(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestGCThing(Condition cond, const Address& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestGCThing(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;

    inline void branchTestPrimitive(Condition cond, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestMagic(Condition cond, const Address& address, Label* label) PER_SHARED_ARCH;
    inline void branchTestMagic(Condition cond, const BaseIndex& address, Label* label) PER_SHARED_ARCH;
    template <class L>
    inline void branchTestMagic(Condition cond, const ValueOperand& value, L label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    inline void branchTestMagic(Condition cond, const Address& valaddr, JSWhyMagic why, Label* label) PER_ARCH;

    inline void branchTestMagicValue(Condition cond, const ValueOperand& val, JSWhyMagic why,
                                     Label* label);

    void branchTestValue(Condition cond, const ValueOperand& lhs,
                         const Value& rhs, Label* label) PER_ARCH;

    // Checks if given Value is evaluated to true or false in a condition.
    // The type of the value should match the type of the method.
    inline void branchTestInt32Truthy(bool truthy, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);
    inline void branchTestDoubleTruthy(bool truthy, FloatRegister reg, Label* label) PER_SHARED_ARCH;
    inline void branchTestBooleanTruthy(bool truthy, const ValueOperand& value, Label* label) PER_ARCH;
    inline void branchTestStringTruthy(bool truthy, const ValueOperand& value, Label* label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86_shared);

    // Create an unconditional branch to the address given as argument.
    inline void branchToComputedAddress(const BaseIndex& address) PER_ARCH;

  private:

    template <typename T, typename S, typename L>
    inline void branchPtrImpl(Condition cond, const T& lhs, const S& rhs, L label)
        DEFINED_ON(x86_shared);

    void branchPtrInNurseryChunkImpl(Condition cond, Register ptr, Label* label)
        DEFINED_ON(x86);
    template <typename T>
    void branchValueIsNurseryCellImpl(Condition cond, const T& value, Register temp, Label* label)
        DEFINED_ON(arm64, x64);

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
    inline void branchTestNullImpl(Condition cond, const T& t, Label* label)
        DEFINED_ON(arm, arm64, x86_shared);
    template <typename T>
    inline void branchTestObjectImpl(Condition cond, const T& t, Label* label)
        DEFINED_ON(arm, arm64, x86_shared);
    template <typename T>
    inline void branchTestGCThingImpl(Condition cond, const T& t, Label* label)
        DEFINED_ON(arm, arm64, x86_shared);
    template <typename T>
    inline void branchTestPrimitiveImpl(Condition cond, const T& t, Label* label)
        DEFINED_ON(arm, arm64, x86_shared);
    template <typename T, class L>
    inline void branchTestMagicImpl(Condition cond, const T& t, L label)
        DEFINED_ON(arm, arm64, x86_shared);

  public:

    inline void cmp32Move32(Condition cond, Register lhs, Register rhs, Register src,
                            Register dest)
        DEFINED_ON(arm, arm64, mips_shared, x86_shared);

    inline void cmp32Move32(Condition cond, Register lhs, const Address& rhs, Register src,
                            Register dest)
        DEFINED_ON(arm, arm64, mips_shared, x86_shared);

    inline void cmp32MovePtr(Condition cond, Register lhs, Imm32 rhs, Register src,
                             Register dest)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    inline void test32LoadPtr(Condition cond, const Address& addr, Imm32 mask, const Address& src,
                              Register dest)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    inline void test32MovePtr(Condition cond, const Address& addr, Imm32 mask, Register src,
                              Register dest)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    // Conditional move for Spectre mitigations.
    inline void spectreMovePtr(Condition cond, Register src, Register dest)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    // Zeroes dest if the condition is true.
    inline void spectreZeroRegister(Condition cond, Register scratch, Register dest)
        DEFINED_ON(arm, arm64, mips_shared, x86_shared);

    // Performs a bounds check and zeroes the index register if out-of-bounds
    // (to mitigate Spectre).
  private:

    inline void spectreBoundsCheck32(Register index, const Operand& length, Register maybeScratch,
                                     Label* failure)
        DEFINED_ON(x86);

  public:

    inline void spectreBoundsCheck32(Register index, Register length, Register maybeScratch,
                                     Label* failure)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);
    inline void spectreBoundsCheck32(Register index, const Address& length, Register maybeScratch,
                                     Label* failure)
        DEFINED_ON(arm, arm64, mips_shared, x86, x64);

    // ========================================================================
    // Canonicalization primitives.
    inline void canonicalizeDouble(FloatRegister reg);
    inline void canonicalizeDoubleIfDeterministic(FloatRegister reg);

    inline void canonicalizeFloat(FloatRegister reg);
    inline void canonicalizeFloatIfDeterministic(FloatRegister reg);

    inline void canonicalizeFloat32x4(FloatRegister reg, FloatRegister scratch)
        DEFINED_ON(x86_shared);

  public:
    // ========================================================================
    // Memory access primitives.
    inline void storeUncanonicalizedDouble(FloatRegister src, const Address& dest)
        DEFINED_ON(x86_shared, arm, arm64, mips32, mips64);
    inline void storeUncanonicalizedDouble(FloatRegister src, const BaseIndex& dest)
        DEFINED_ON(x86_shared, arm, arm64, mips32, mips64);
    inline void storeUncanonicalizedDouble(FloatRegister src, const Operand& dest)
        DEFINED_ON(x86_shared);

    template<class T>
    inline void storeDouble(FloatRegister src, const T& dest);

    inline void boxDouble(FloatRegister src, const Address& dest);
    using MacroAssemblerSpecific::boxDouble;

    inline void storeUncanonicalizedFloat32(FloatRegister src, const Address& dest)
        DEFINED_ON(x86_shared, arm, arm64, mips32, mips64);
    inline void storeUncanonicalizedFloat32(FloatRegister src, const BaseIndex& dest)
        DEFINED_ON(x86_shared, arm, arm64, mips32, mips64);
    inline void storeUncanonicalizedFloat32(FloatRegister src, const Operand& dest)
        DEFINED_ON(x86_shared);

    template<class T>
    inline void storeFloat32(FloatRegister src, const T& dest);

    inline void storeFloat32x3(FloatRegister src, const Address& dest) PER_SHARED_ARCH;
    inline void storeFloat32x3(FloatRegister src, const BaseIndex& dest) PER_SHARED_ARCH;

    template <typename T>
    void storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType, const T& dest,
                           MIRType slotType) PER_ARCH;

    inline void memoryBarrier(MemoryBarrierBits barrier) PER_SHARED_ARCH;

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

    // temp required on x86 and x64; must be undefined on mips64.
    void convertUInt64ToFloat32(Register64 src, FloatRegister dest, Register temp)
        DEFINED_ON(arm64, mips64, x64, x86);

    void convertInt64ToFloat32(Register64 src, FloatRegister dest)
        DEFINED_ON(arm64, mips64, x64, x86);

    bool convertUInt64ToDoubleNeedsTemp() PER_ARCH;

    // temp required when convertUInt64ToDoubleNeedsTemp() returns true.
    void convertUInt64ToDouble(Register64 src, FloatRegister dest, Register temp) PER_ARCH;

    void convertInt64ToDouble(Register64 src, FloatRegister dest)
        DEFINED_ON(arm64, mips64, x64, x86);

  public:
    // ========================================================================
    // wasm support

    CodeOffset wasmTrapInstruction() PER_SHARED_ARCH;

    void wasmTrap(wasm::Trap trap, wasm::BytecodeOffset bytecodeOffset);

    // Emit a bounds check against the wasm heap limit, jumping to 'label' if
    // 'cond' holds. Required when WASM_HUGE_MEMORY is not defined. If
    // JitOptions.spectreMaskIndex is true, in speculative executions 'index' is
    // saturated in-place to 'boundsCheckLimit'.
    template <class L>
    inline void wasmBoundsCheck(Condition cond, Register index, Register boundsCheckLimit, L label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86);

    template <class L>
    inline void wasmBoundsCheck(Condition cond, Register index, Address boundsCheckLimit, L label)
        DEFINED_ON(arm, arm64, mips32, mips64, x86);

    // On x86, each instruction adds its own wasm::MemoryAccess's to the
    // wasm::MemoryAccessVector (there can be multiple when i64 is involved).
    // On x64, only some asm.js accesses need a wasm::MemoryAccess so the caller
    // is responsible for doing this instead.
    void wasmLoad(const wasm::MemoryAccessDesc& access, Operand srcAddr, AnyRegister out) DEFINED_ON(x86, x64);
    void wasmLoadI64(const wasm::MemoryAccessDesc& access, Operand srcAddr, Register64 out) DEFINED_ON(x86, x64);
    void wasmStore(const wasm::MemoryAccessDesc& access, AnyRegister value, Operand dstAddr) DEFINED_ON(x86, x64);
    void wasmStoreI64(const wasm::MemoryAccessDesc& access, Register64 value, Operand dstAddr) DEFINED_ON(x86);

    // For all the ARM and ARM64 wasmLoad and wasmStore functions, `ptr` MUST
    // equal `ptrScratch`, and that register will be updated based on conditions
    // listed below (where it is only mentioned as `ptr`).

    // `ptr` will be updated if access.offset() != 0 or access.type() == Scalar::Int64.
    void wasmLoad(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                  Register ptrScratch, AnyRegister output)
        DEFINED_ON(arm, arm64, mips_shared);
    void wasmLoadI64(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                     Register ptrScratch, Register64 output)
        DEFINED_ON(arm, arm64, mips32, mips64);
    void wasmStore(const wasm::MemoryAccessDesc& access, AnyRegister value, Register memoryBase,
                   Register ptr, Register ptrScratch)
        DEFINED_ON(arm, arm64, mips_shared);
    void wasmStoreI64(const wasm::MemoryAccessDesc& access, Register64 value, Register memoryBase,
                      Register ptr, Register ptrScratch)
        DEFINED_ON(arm, arm64, mips32, mips64);

    // `ptr` will always be updated.
    void wasmUnalignedLoad(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                           Register ptrScratch, Register output, Register tmp)
        DEFINED_ON(arm, mips32, mips64);

    // ARM: `ptr` will always be updated and `tmp1` is always needed.  `tmp2` is
    // needed for Float32; `tmp2` and `tmp3` are needed for Float64.  Temps must
    // be Invalid when they are not needed.
    // MIPS: `ptr` will always be updated.
    void wasmUnalignedLoadFP(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                             Register ptrScratch, FloatRegister output, Register tmp1, Register tmp2,
                             Register tmp3)
        DEFINED_ON(arm, mips32, mips64);

    // `ptr` will always be updated.
    void wasmUnalignedLoadI64(const wasm::MemoryAccessDesc& access, Register memoryBase, Register ptr,
                              Register ptrScratch, Register64 output, Register tmp)
        DEFINED_ON(arm, mips32, mips64);

    // ARM: `ptr` and `value` will always be updated.  'tmp' must be Invalid.
    // MIPS: `ptr` will always be updated.
    void wasmUnalignedStore(const wasm::MemoryAccessDesc& access, Register value, Register memoryBase,
                            Register ptr, Register ptrScratch, Register tmp)
        DEFINED_ON(arm, mips32, mips64);

    // `ptr` will always be updated.
    void wasmUnalignedStoreFP(const wasm::MemoryAccessDesc& access, FloatRegister floatValue,
                              Register memoryBase, Register ptr, Register ptrScratch, Register tmp)
        DEFINED_ON(arm, mips32, mips64);

    // `ptr` will always be updated.
    void wasmUnalignedStoreI64(const wasm::MemoryAccessDesc& access, Register64 value,
                               Register memoryBase, Register ptr, Register ptrScratch,
                               Register tmp)
        DEFINED_ON(arm, mips32, mips64);

    // wasm specific methods, used in both the wasm baseline compiler and ion.

    // The truncate-to-int32 methods do not bind the rejoin label; clients must
    // do so if oolWasmTruncateCheckF64ToI32() can jump to it.
    void wasmTruncateDoubleToUInt32(FloatRegister input, Register output, bool isSaturating,
                                    Label* oolEntry) PER_ARCH;
    void wasmTruncateDoubleToInt32(FloatRegister input, Register output, bool isSaturating,
                                   Label* oolEntry) PER_SHARED_ARCH;
    void oolWasmTruncateCheckF64ToI32(FloatRegister input, Register output, TruncFlags flags,
                                      wasm::BytecodeOffset off, Label* rejoin)
        DEFINED_ON(arm, arm64, x86_shared, mips_shared);

    void wasmTruncateFloat32ToUInt32(FloatRegister input, Register output, bool isSaturating,
                                     Label* oolEntry) PER_ARCH;
    void wasmTruncateFloat32ToInt32(FloatRegister input, Register output, bool isSaturating,
                                    Label* oolEntry) PER_SHARED_ARCH;
    void oolWasmTruncateCheckF32ToI32(FloatRegister input, Register output, TruncFlags flags,
                                      wasm::BytecodeOffset off, Label* rejoin)
        DEFINED_ON(arm, arm64, x86_shared, mips_shared);

    // The truncate-to-int64 methods will always bind the `oolRejoin` label
    // after the last emitted instruction.
    void wasmTruncateDoubleToInt64(FloatRegister input, Register64 output, bool isSaturating,
                                   Label* oolEntry, Label* oolRejoin, FloatRegister tempDouble)
        DEFINED_ON(arm64, x86, x64, mips64);
    void wasmTruncateDoubleToUInt64(FloatRegister input, Register64 output, bool isSaturating,
                                    Label* oolEntry, Label* oolRejoin, FloatRegister tempDouble)
        DEFINED_ON(arm64, x86, x64, mips64);
    void oolWasmTruncateCheckF64ToI64(FloatRegister input, Register64 output, TruncFlags flags,
                                      wasm::BytecodeOffset off, Label* rejoin)
        DEFINED_ON(arm, arm64, x86_shared, mips_shared);

    void wasmTruncateFloat32ToInt64(FloatRegister input, Register64 output, bool isSaturating,
                                    Label* oolEntry, Label* oolRejoin, FloatRegister tempDouble)
        DEFINED_ON(arm64, x86, x64, mips64);
    void wasmTruncateFloat32ToUInt64(FloatRegister input, Register64 output, bool isSaturating,
                                     Label* oolEntry, Label* oolRejoin, FloatRegister tempDouble)
        DEFINED_ON(arm64, x86, x64, mips64);
    void oolWasmTruncateCheckF32ToI64(FloatRegister input, Register64 output, TruncFlags flags,
                                      wasm::BytecodeOffset off, Label* rejoin)
        DEFINED_ON(arm, arm64, x86_shared, mips_shared);

    // This function takes care of loading the callee's TLS and pinned regs but
    // it is the caller's responsibility to save/restore TLS or pinned regs.
    void wasmCallImport(const wasm::CallSiteDesc& desc, const wasm::CalleeDesc& callee);

    // WasmTableCallIndexReg must contain the index of the indirect call.
    void wasmCallIndirect(const wasm::CallSiteDesc& desc, const wasm::CalleeDesc& callee, bool needsBoundsCheck);

    // This function takes care of loading the pointer to the current instance
    // as the implicit first argument. It preserves TLS and pinned registers.
    // (TLS & pinned regs are non-volatile registers in the system ABI).
    void wasmCallBuiltinInstanceMethod(const wasm::CallSiteDesc& desc, const ABIArg& instanceArg,
                                       wasm::SymbolicAddress builtin);

    // Emit the out-of-line trap code to which trapping jumps/branches are
    // bound. This should be called once per function after all other codegen,
    // including "normal" OutOfLineCode.
    void wasmEmitOldTrapOutOfLineCode();

  public:
    // ========================================================================
    // Barrier functions.

    void emitPreBarrierFastPath(JSRuntime* rt, MIRType type, Register temp1, Register temp2,
                                Register temp3, Label* noBarrier);

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

    void compareExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                         Register expected, Register replacement, Register output)
        DEFINED_ON(arm, arm64, x86_shared);

    void compareExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                         Register expected, Register replacement, Register output)
        DEFINED_ON(arm, arm64, x86_shared);


    void compareExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                         Register expected, Register replacement, Register valueTemp,
                         Register offsetTemp, Register maskTemp, Register output)
        DEFINED_ON(mips_shared);

    void compareExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                         Register expected, Register replacement, Register valueTemp,
                         Register offsetTemp, Register maskTemp, Register output)
        DEFINED_ON(mips_shared);

    // Exchange with memory.  Return the value initially in memory.
    // MIPS: `valueTemp`, `offsetTemp` and `maskTemp` must be defined for 8-bit
    // and 16-bit wide operations.

    void atomicExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                        Register value, Register output)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                        Register value, Register output)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicExchange(Scalar::Type type, const Synchronization& sync, const Address& mem,
                        Register value, Register valueTemp, Register offsetTemp, Register maskTemp,
                        Register output)
        DEFINED_ON(mips_shared);

    void atomicExchange(Scalar::Type type, const Synchronization& sync, const BaseIndex& mem,
                        Register value, Register valueTemp, Register offsetTemp, Register maskTemp,
                        Register output)
        DEFINED_ON(mips_shared);

    // Read-modify-write with memory.  Return the value in memory before the
    // operation.
    //
    // x86-shared:
    //   For 8-bit operations, `value` and `output` must have a byte subregister.
    //   For Add and Sub, `temp` must be invalid.
    //   For And, Or, and Xor, `output` must be eax and `temp` must have a byte subregister.
    //
    // ARM: Registers `value` and `output` must differ.
    // MIPS: `valueTemp`, `offsetTemp` and `maskTemp` must be defined for 8-bit
    // and 16-bit wide operations; `value` and `output` must differ.

    void atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                       Register value, const Address& mem, Register temp, Register output)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                       Imm32 value, const Address& mem, Register temp, Register output)
        DEFINED_ON(x86_shared);

    void atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                       Register value, const BaseIndex& mem, Register temp, Register output)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                       Imm32 value, const BaseIndex& mem, Register temp, Register output)
        DEFINED_ON(x86_shared);

    void atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                       Register value, const Address& mem, Register valueTemp,
                       Register offsetTemp, Register maskTemp, Register output)
        DEFINED_ON(mips_shared);

    void atomicFetchOp(Scalar::Type type, const Synchronization& sync, AtomicOp op,
                       Register value, const BaseIndex& mem, Register valueTemp,
                       Register offsetTemp, Register maskTemp, Register output)
        DEFINED_ON(mips_shared);

    // Read-modify-write with memory.  Return no value.
    // MIPS: `valueTemp`, `offsetTemp` and `maskTemp` must be defined for 8-bit
    // and 16-bit wide operations.

    void atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op, Register value,
                        const Address& mem, Register temp)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op, Imm32 value,
                        const Address& mem, Register temp)
        DEFINED_ON(x86_shared);

    void atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op, Register value,
                        const BaseIndex& mem, Register temp)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op, Imm32 value,
                        const BaseIndex& mem, Register temp)
        DEFINED_ON(x86_shared);


    void atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op, Register value,
                    const Address& mem, Register valueTemp, Register offsetTemp, Register maskTemp)
        DEFINED_ON(mips_shared);

    void atomicEffectOp(Scalar::Type type, const Synchronization& sync, AtomicOp op, Register value,
                    const BaseIndex& mem, Register valueTemp, Register offsetTemp, Register maskTemp)
        DEFINED_ON(mips_shared);

    // 64-bit wide operations.

    // 64-bit atomic load.  On 64-bit systems, use regular wasm load with
    // Synchronization::Load, not this method.
    //
    // x86: `temp` must be ecx:ebx; `output` must be edx:eax.
    // ARM: `temp` should be invalid; `output` must be (even,odd) pair.
    // MIPS32: `temp` should be invalid.

    void atomicLoad64(const Synchronization& sync, const Address& mem, Register64 temp,
                      Register64 output)
        DEFINED_ON(arm, mips32, x86);

    void atomicLoad64(const Synchronization& sync, const BaseIndex& mem, Register64 temp,
                      Register64 output)
        DEFINED_ON(arm, mips32, x86);

    // x86: `expected` must be the same as `output`, and must be edx:eax
    // x86: `replacement` must be ecx:ebx
    // x64: `output` must be rax.
    // ARM: Registers must be distinct; `replacement` and `output` must be (even,odd) pairs.
    // MIPS: Registers must be distinct.

    void compareExchange64(const Synchronization& sync, const Address& mem, Register64 expected,
                           Register64 replacement, Register64 output) PER_ARCH;

    void compareExchange64(const Synchronization& sync, const BaseIndex& mem, Register64 expected,
                           Register64 replacement, Register64 output) PER_ARCH;

    // x86: `value` must be ecx:ebx; `output` must be edx:eax.
    // ARM: Registers must be distinct; `value` and `output` must be (even,odd) pairs.
    // MIPS: Registers must be distinct.

    void atomicExchange64(const Synchronization& sync, const Address& mem, Register64 value,
                          Register64 output) PER_ARCH;

    void atomicExchange64(const Synchronization& sync, const BaseIndex& mem, Register64 value,
                          Register64 output) PER_ARCH;

    // x86: `output` must be edx:eax, `temp` must be ecx:ebx.
    // x64: For And, Or, and Xor `output` must be rax.
    // ARM: Registers must be distinct; `temp` and `output` must be (even,odd) pairs.
    // MIPS: Registers must be distinct.
    // MIPS32: `temp` should be invalid.

    void atomicFetchOp64(const Synchronization& sync, AtomicOp op, Register64 value,
                         const Address& mem, Register64 temp, Register64 output)
        DEFINED_ON(arm, arm64, mips32, mips64, x64);

    void atomicFetchOp64(const Synchronization& sync, AtomicOp op, Register64 value,
                         const BaseIndex& mem, Register64 temp, Register64 output)
        DEFINED_ON(arm, arm64, mips32, mips64, x64);

    void atomicFetchOp64(const Synchronization& sync, AtomicOp op, const Address& value,
                         const Address& mem, Register64 temp, Register64 output)
        DEFINED_ON(x86);

    void atomicFetchOp64(const Synchronization& sync, AtomicOp op, const Address& value,
                         const BaseIndex& mem, Register64 temp, Register64 output)
        DEFINED_ON(x86);

    void atomicEffectOp64(const Synchronization& sync, AtomicOp op, Register64 value,
                          const BaseIndex& mem)
        DEFINED_ON(x64);

    // ========================================================================
    // JS atomic operations.
    //
    // Here the arrayType must be a type that is valid for JS.  As of 2017 that
    // is an 8-bit, 16-bit, or 32-bit integer type.
    //
    // If arrayType is Scalar::Uint32 then:
    //
    //   - `output` must be a float register (this is bug 1077305)
    //   - if the operation takes one temp register then `temp` must be defined
    //   - if the operation takes two temp registers then `temp2` must be defined.
    //
    // Otherwise `output` must be a GPR and `temp`/`temp2` should be InvalidReg.
    // (`temp1` must always be valid.)
    //
    // For additional register constraints, see the primitive 32-bit operations
    // above.

    void compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync, const Address& mem,
                           Register expected, Register replacement, Register temp,
                           AnyRegister output)
        DEFINED_ON(arm, arm64, x86_shared);

    void compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync,
                           const BaseIndex& mem, Register expected, Register replacement,
                           Register temp, AnyRegister output)
        DEFINED_ON(arm, arm64, x86_shared);

    void compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync, const Address& mem,
                           Register expected, Register replacement, Register valueTemp,
                           Register offsetTemp, Register maskTemp, Register temp,
                           AnyRegister output)
        DEFINED_ON(mips_shared);

    void compareExchangeJS(Scalar::Type arrayType, const Synchronization& sync, const BaseIndex& mem,
                           Register expected, Register replacement, Register valueTemp,
                           Register offsetTemp, Register maskTemp, Register temp,
                           AnyRegister output)
        DEFINED_ON(mips_shared);

    void atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync, const Address& mem,
                          Register value, Register temp, AnyRegister output)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync, const BaseIndex& mem,
                          Register value, Register temp,  AnyRegister output)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync, const Address& mem,
                          Register value, Register valueTemp, Register offsetTemp,
                          Register maskTemp, Register temp, AnyRegister output)
        DEFINED_ON(mips_shared);

    void atomicExchangeJS(Scalar::Type arrayType, const Synchronization& sync, const BaseIndex& mem,
                          Register value, Register valueTemp, Register offsetTemp,
                          Register maskTemp, Register temp, AnyRegister output)
        DEFINED_ON(mips_shared);


    void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                         Register value, const Address& mem, Register temp1, Register temp2,
                         AnyRegister output)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                         Register value, const BaseIndex& mem, Register temp1, Register temp2,
                         AnyRegister output)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                         Imm32 value, const Address& mem, Register temp1, Register temp2,
                         AnyRegister output)
        DEFINED_ON(x86_shared);

    void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                         Imm32 value, const BaseIndex& mem, Register temp1, Register temp2,
                         AnyRegister output)
        DEFINED_ON(x86_shared);

    void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                         Register value, const Address& mem, Register valueTemp,
                         Register offsetTemp, Register maskTemp, Register temp,
                         AnyRegister output)
        DEFINED_ON(mips_shared);

    void atomicFetchOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                         Register value, const BaseIndex& mem, Register valueTemp,
                         Register offsetTemp, Register maskTemp, Register temp,
                         AnyRegister output)
        DEFINED_ON(mips_shared);

    void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                          Register value, const Address& mem, Register temp)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                          Register value, const BaseIndex& mem, Register temp)
        DEFINED_ON(arm, arm64, x86_shared);

    void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                          Imm32 value, const Address& mem, Register temp)
        DEFINED_ON(x86_shared);

    void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                          Imm32 value, const BaseIndex& mem, Register temp)
        DEFINED_ON(x86_shared);

    void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                          Register value, const Address& mem, Register valueTemp,
                          Register offsetTemp, Register maskTemp)
        DEFINED_ON(mips_shared);

    void atomicEffectOpJS(Scalar::Type arrayType, const Synchronization& sync, AtomicOp op,
                          Register value, const BaseIndex& mem, Register valueTemp,
                          Register offsetTemp, Register maskTemp)
        DEFINED_ON(mips_shared);

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

    void spectreMaskIndex(Register index, Register length, Register output);
    void spectreMaskIndex(Register index, const Address& length, Register output);

    // The length must be a power of two. Performs a bounds check and Spectre index
    // masking.
    void boundsCheck32PowerOfTwo(Register index, uint32_t length, Label* failure);

    void speculationBarrier() PER_SHARED_ARCH;

    //}}} check_macroassembler_decl_style
  public:

    // Emits a test of a value against all types in a TypeSet. A scratch
    // register is required.
    template <typename Source>
    void guardTypeSet(const Source& address, const TypeSet* types, BarrierKind kind,
                      Register unboxScratch, Register objScratch, Register spectreRegToZero,
                      Label* miss);

    void guardObjectType(Register obj, const TypeSet* types, Register scratch,
                         Register spectreRegToZero, Label* miss);

#ifdef DEBUG
    void guardTypeSetMightBeIncomplete(const TypeSet* types, Register obj, Register scratch,
                                       Label* label);
#endif

    // Unsafe here means the caller is responsible for Spectre mitigations if
    // needed. Prefer branchTestObjGroup or one of the other masm helpers!
    void loadObjGroupUnsafe(Register obj, Register dest) {
        loadPtr(Address(obj, JSObject::offsetOfGroup()), dest);
    }
    void loadObjClassUnsafe(Register obj, Register dest) {
        loadPtr(Address(obj, JSObject::offsetOfGroup()), dest);
        loadPtr(Address(dest, ObjectGroup::offsetOfClasp()), dest);
    }

    template <typename EmitPreBarrier>
    inline void storeObjGroup(Register group, Register obj, EmitPreBarrier emitPreBarrier);
    template <typename EmitPreBarrier>
    inline void storeObjGroup(ObjectGroup* group, Register obj, EmitPreBarrier emitPreBarrier);
    template <typename EmitPreBarrier>
    inline void storeObjShape(Register shape, Register obj, EmitPreBarrier emitPreBarrier);
    template <typename EmitPreBarrier>
    inline void storeObjShape(Shape* shape, Register obj, EmitPreBarrier emitPreBarrier);

    void loadObjPrivate(Register obj, uint32_t nfixed, Register dest) {
        loadPtr(Address(obj, NativeObject::getPrivateDataOffset(nfixed)), dest);
    }

    void loadObjProto(Register obj, Register dest) {
        loadPtr(Address(obj, JSObject::offsetOfGroup()), dest);
        loadPtr(Address(dest, ObjectGroup::offsetOfProto()), dest);
    }

    void loadStringLength(Register str, Register dest) {
        load32(Address(str, JSString::offsetOfLength()), dest);
    }

    void loadStringChars(Register str, Register dest, CharEncoding encoding);

    void loadNonInlineStringChars(Register str, Register dest, CharEncoding encoding);
    void loadNonInlineStringCharsForStore(Register str, Register dest);
    void storeNonInlineStringChars(Register chars, Register str);

    void loadInlineStringChars(Register str, Register dest, CharEncoding encoding);
    void loadInlineStringCharsForStore(Register str, Register dest);

    void loadStringChar(Register str, Register index, Register output, Register scratch,
                        Label* fail);

    void loadRopeLeftChild(Register str, Register dest);
    void storeRopeChildren(Register left, Register right, Register str);

    void loadDependentStringBase(Register str, Register dest);
    void storeDependentStringBase(Register base, Register str);
    void leaNewDependentStringBase(Register str, Register dest);

    void loadStringIndexValue(Register str, Register dest, Label* fail);

    void loadJSContext(Register dest);
    void loadJitActivation(Register dest) {
        loadJSContext(dest);
        loadPtr(Address(dest, offsetof(JSContext, activation_)), dest);
    }

    void guardGroupHasUnanalyzedNewScript(Register group, Register scratch, Label* fail);

    void loadWasmTlsRegFromFrame(Register dest = WasmTlsReg);

    template<typename T>
    void loadTypedOrValue(const T& src, TypedOrValueRegister dest) {
        if (dest.hasValue())
            loadValue(src, dest.valueReg());
        else
            loadUnboxedValue(src, dest.type(), dest.typedReg());
    }

    template<typename T>
    void loadElementTypedOrValue(const T& src, TypedOrValueRegister dest, bool holeCheck,
                                 Label* hole) {
        if (dest.hasValue()) {
            loadValue(src, dest.valueReg());
            if (holeCheck)
                branchTestMagic(Assembler::Equal, dest.valueReg(), hole);
        } else {
            if (holeCheck)
                branchTestMagic(Assembler::Equal, src, hole);
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
                convertFloat32ToDouble(reg, ScratchDoubleReg);
                reg = ScratchDoubleReg;
            }
            storeDouble(reg, dest);
        } else {
            storeValue(ValueTypeFromMIRType(src.type()), src.typedReg().gpr(), dest);
        }
    }

    template <typename T>
    inline void storeObjectOrNull(Register src, const T& dest);

    template <typename T>
    void storeConstantOrRegister(const ConstantOrRegister& src, const T& dest) {
        if (src.constant())
            storeValue(src.value(), dest);
        else
            storeTypedOrValue(src.reg(), dest);
    }

    void storeCallPointerResult(Register reg) {
        if (reg != ReturnReg)
            mov(ReturnReg, reg);
    }

    inline void storeCallBoolResult(Register reg);
    inline void storeCallInt32Result(Register reg);

    void storeCallFloatResult(FloatRegister reg) {
        if (reg != ReturnDoubleReg)
            moveDouble(ReturnDoubleReg, reg);
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
        if (dest.valueReg() != JSReturnReg)
            mov(JSReturnReg, dest.valueReg());
#else
#error "Bad architecture"
#endif
    }

    inline void storeCallResultValue(TypedOrValueRegister dest);

    template <typename T>
    void guardedCallPreBarrier(const T& address, MIRType type) {
        Label done;

        branchTestNeedsIncrementalBarrier(Assembler::Zero, &done);

        if (type == MIRType::Value)
            branchTestGCThing(Assembler::NotEqual, address, &done);
        else if (type == MIRType::Object || type == MIRType::String)
            branchPtr(Assembler::Equal, address, ImmWord(0), &done);

        Push(PreBarrierReg);
        computeEffectiveAddress(address, PreBarrierReg);

        const JitRuntime* rt = GetJitContext()->runtime->jitRuntime();
        TrampolinePtr preBarrier = rt->preBarrier(type);

        call(preBarrier);
        Pop(PreBarrierReg);

        bind(&done);
    }

    template<typename T>
    void loadFromTypedArray(Scalar::Type arrayType, const T& src, AnyRegister dest, Register temp, Label* fail,
                            bool canonicalizeDoubles = true, unsigned numElems = 0);

    template<typename T>
    void loadFromTypedArray(Scalar::Type arrayType, const T& src, const ValueOperand& dest, bool allowDouble,
                            Register temp, Label* fail);

    template<typename S, typename T>
    void storeToTypedIntArray(Scalar::Type arrayType, const S& value, const T& dest) {
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

    void storeToTypedFloatArray(Scalar::Type arrayType, FloatRegister value, const BaseIndex& dest,
                                unsigned numElems = 0);
    void storeToTypedFloatArray(Scalar::Type arrayType, FloatRegister value, const Address& dest,
                                unsigned numElems = 0);

    void memoryBarrierBefore(const Synchronization& sync);
    void memoryBarrierAfter(const Synchronization& sync);

    // Load a property from an UnboxedPlainObject or UnboxedArrayObject.
    template <typename T>
    void loadUnboxedProperty(T address, JSValueType type, TypedOrValueRegister output);

    // Store a property to an UnboxedPlainObject, without triggering barriers.
    // If failure is null, the value definitely has a type suitable for storing
    // in the property.
    template <typename T>
    void storeUnboxedProperty(T address, JSValueType type,
                              const ConstantOrRegister& value, Label* failure);

    void debugAssertIsObject(const ValueOperand& val);
    void debugAssertObjHasFixedSlots(Register obj, Register scratch);

    using MacroAssemblerSpecific::extractTag;
    Register extractTag(const TypedOrValueRegister& reg, Register scratch) {
        if (reg.hasValue())
            return extractTag(reg.valueReg(), scratch);
        mov(ImmWord(MIRTypeToTag(reg.type())), scratch);
        return scratch;
    }

    using MacroAssemblerSpecific::extractObject;
    Register extractObject(const TypedOrValueRegister& reg, Register scratch) {
        if (reg.hasValue())
            return extractObject(reg.valueReg(), scratch);
        MOZ_ASSERT(reg.type() == MIRType::Object);
        return reg.typedReg().gpr();
    }

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
    bool shouldNurseryAllocate(gc::AllocKind allocKind, gc::InitialHeap initialHeap);
    void nurseryAllocateObject(Register result, Register temp, gc::AllocKind allocKind,
                               size_t nDynamicSlots, Label* fail);
    void freeListAllocate(Register result, Register temp, gc::AllocKind allocKind, Label* fail);
    void allocateObject(Register result, Register temp, gc::AllocKind allocKind,
                        uint32_t nDynamicSlots, gc::InitialHeap initialHeap, Label* fail);
    void nurseryAllocateString(Register result, Register temp, gc::AllocKind allocKind,
                               Label* fail);
    void allocateString(Register result, Register temp, gc::AllocKind allocKind,
                        gc::InitialHeap initialHeap, Label* fail);
    void allocateNonObject(Register result, Register temp, gc::AllocKind allocKind, Label* fail);
    void copySlotsFromTemplate(Register obj, const NativeObject* templateObj,
                               uint32_t start, uint32_t end);
    void fillSlotsWithConstantValue(Address addr, Register temp, uint32_t start, uint32_t end,
                                    const Value& v);
    void fillSlotsWithUndefined(Address addr, Register temp, uint32_t start, uint32_t end);
    void fillSlotsWithUninitialized(Address addr, Register temp, uint32_t start, uint32_t end);

    void initGCSlots(Register obj, Register temp, NativeObject* templateObj, bool initContents);

  public:
    void callMallocStub(size_t nbytes, Register result, Label* fail);
    void callFreeStub(Register slots);
    void createGCObject(Register result, Register temp, JSObject* templateObj,
                        gc::InitialHeap initialHeap, Label* fail, bool initContents = true,
                        bool convertDoubleElements = false);

    void initGCThing(Register obj, Register temp, JSObject* templateObj,
                     bool initContents = true, bool convertDoubleElements = false);
    void initTypedArraySlots(Register obj, Register temp, Register lengthReg,
                             LiveRegisterSet liveRegs, Label* fail,
                             TypedArrayObject* templateObj, TypedArrayLength lengthKind);

    void initUnboxedObjectContents(Register object, UnboxedPlainObject* templateObject);

    void newGCString(Register result, Register temp, Label* fail, bool attemptNursery);
    void newGCFatInlineString(Register result, Register temp, Label* fail, bool attemptNursery);

    // Compares two strings for equality based on the JSOP.
    // This checks for identical pointers, atoms and length and fails for everything else.
    void compareStrings(JSOp op, Register left, Register right, Register result,
                        Label* fail);

    // Result of the typeof operation. Falls back to slow-path for proxies.
    void typeOfObject(Register objReg, Register scratch, Label* slow,
                      Label* isObject, Label* isCallable, Label* isUndefined);

  public:
    // Generates code used to complete a bailout.
    void generateBailoutTail(Register scratch, Register bailoutInfo);

    void assertRectifierFrameParentType(Register frameType);

  public:
#ifndef JS_CODEGEN_ARM64
    // StackPointer manipulation functions.
    // On ARM64, the StackPointer is implemented as two synchronized registers.
    // Code shared across platforms must use these functions to be valid.
    template <typename T> inline void addToStackPtr(T t);
    template <typename T> inline void addStackPtrTo(T t);

    void subFromStackPtr(Imm32 imm32) DEFINED_ON(mips32, mips64, arm, x86, x64);
    void subFromStackPtr(Register reg);

    template <typename T>
    void subStackPtrFrom(T t) { subPtr(getStackPointer(), t); }

    template <typename T>
    void andToStackPtr(T t) { andPtr(t, getStackPointer()); }
    template <typename T>
    void andStackPtrTo(T t) { andPtr(getStackPointer(), t); }

    template <typename T>
    void moveToStackPtr(T t) { movePtr(t, getStackPointer()); }
    template <typename T>
    void moveStackPtrTo(T t) { movePtr(getStackPointer(), t); }

    template <typename T>
    void loadStackPtr(T t) { loadPtr(t, getStackPointer()); }
    template <typename T>
    void storeStackPtr(T t) { storePtr(getStackPointer(), t); }

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
#else // !JS_CODEGEN_ARM64
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
    class AutoProfilerCallInstrumentation {
        MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER;

      public:
        explicit AutoProfilerCallInstrumentation(MacroAssembler& masm
                                                 MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
        ~AutoProfilerCallInstrumentation() {}
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
    void loadJitCodeRaw(Register callee, Register dest);
    void loadJitCodeNoArgCheck(Register callee, Register dest);

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

    Label* failureLabel() {
        return &failureLabel_;
    }

    void finish();
    void link(JitCode* code);

    void assumeUnreachable(const char* output);

    template<typename T>
    void assertTestInt32(Condition cond, const T& value, const char* output);

    void printf(const char* output);
    void printf(const char* output, Register value);

#ifdef JS_TRACE_LOGGING
    void loadTraceLogger(Register logger) {
        loadJSContext(logger);
        loadPtr(Address(logger, offsetof(JSContext, traceLogger)), logger);
    }
    void tracelogStartId(Register logger, uint32_t textId, bool force = false);
    void tracelogStartId(Register logger, Register textId);
    void tracelogStartEvent(Register logger, Register event);
    void tracelogStopId(Register logger, uint32_t textId, bool force = false);
    void tracelogStopId(Register logger, Register textId);
#endif

#define DISPATCH_FLOATING_POINT_OP(method, type, arg1d, arg1f, arg2)    \
    MOZ_ASSERT(IsFloatingPointType(type));                              \
    if (type == MIRType::Double)                                        \
        method##Double(arg1d, arg2);                                    \
    else                                                                \
        method##Float32(arg1f, arg2);                                   \

    void loadConstantFloatingPoint(double d, float f, FloatRegister dest, MIRType destType) {
        DISPATCH_FLOATING_POINT_OP(loadConstant, destType, d, f, dest);
    }
    void boolValueToFloatingPoint(ValueOperand value, FloatRegister dest, MIRType destType) {
        DISPATCH_FLOATING_POINT_OP(boolValueTo, destType, value, value, dest);
    }
    void int32ValueToFloatingPoint(ValueOperand value, FloatRegister dest, MIRType destType) {
        DISPATCH_FLOATING_POINT_OP(int32ValueTo, destType, value, value, dest);
    }
    void convertInt32ToFloatingPoint(Register src, FloatRegister dest, MIRType destType) {
        DISPATCH_FLOATING_POINT_OP(convertInt32To, destType, src, src, dest);
    }

#undef DISPATCH_FLOATING_POINT_OP

    void convertValueToFloatingPoint(ValueOperand value, FloatRegister output, Label* fail,
                                     MIRType outputType);
    MOZ_MUST_USE bool convertValueToFloatingPoint(JSContext* cx, const Value& v,
                                                  FloatRegister output, Label* fail,
                                                  MIRType outputType);
    MOZ_MUST_USE bool convertConstantOrRegisterToFloatingPoint(JSContext* cx,
                                                               const ConstantOrRegister& src,
                                                               FloatRegister output, Label* fail,
                                                               MIRType outputType);
    void convertTypedOrValueToFloatingPoint(TypedOrValueRegister src, FloatRegister output,
                                            Label* fail, MIRType outputType);

    void outOfLineTruncateSlow(FloatRegister src, Register dest, bool widenFloatToDouble,
                               bool compilingWasm, wasm::BytecodeOffset callOffset);

    void convertInt32ValueToDouble(const Address& address, Register scratch, Label* done);
    void convertInt32ValueToDouble(ValueOperand val);

    void convertValueToDouble(ValueOperand value, FloatRegister output, Label* fail) {
        convertValueToFloatingPoint(value, output, fail, MIRType::Double);
    }
    MOZ_MUST_USE bool convertValueToDouble(JSContext* cx, const Value& v, FloatRegister output,
                                           Label* fail) {
        return convertValueToFloatingPoint(cx, v, output, fail, MIRType::Double);
    }
    MOZ_MUST_USE bool convertConstantOrRegisterToDouble(JSContext* cx,
                                                        const ConstantOrRegister& src,
                                                        FloatRegister output, Label* fail)
    {
        return convertConstantOrRegisterToFloatingPoint(cx, src, output, fail, MIRType::Double);
    }
    void convertTypedOrValueToDouble(TypedOrValueRegister src, FloatRegister output, Label* fail) {
        convertTypedOrValueToFloatingPoint(src, output, fail, MIRType::Double);
    }

    void convertValueToFloat(ValueOperand value, FloatRegister output, Label* fail) {
        convertValueToFloatingPoint(value, output, fail, MIRType::Float32);
    }
    MOZ_MUST_USE bool convertValueToFloat(JSContext* cx, const Value& v, FloatRegister output,
                                          Label* fail) {
        return convertValueToFloatingPoint(cx, v, output, fail, MIRType::Float32);
    }
    MOZ_MUST_USE bool convertConstantOrRegisterToFloat(JSContext* cx,
                                                       const ConstantOrRegister& src,
                                                       FloatRegister output, Label* fail)
    {
        return convertConstantOrRegisterToFloatingPoint(cx, src, output, fail, MIRType::Float32);
    }
    void convertTypedOrValueToFloat(TypedOrValueRegister src, FloatRegister output, Label* fail) {
        convertTypedOrValueToFloatingPoint(src, output, fail, MIRType::Float32);
    }
    //
    // Functions for converting values to int.
    //
    void convertDoubleToInt(FloatRegister src, Register output, FloatRegister temp,
                            Label* truncateFail, Label* fail, IntConversionBehavior behavior);

    // Strings may be handled by providing labels to jump to when the behavior
    // is truncation or clamping. The subroutine, usually an OOL call, is
    // passed the unboxed string in |stringReg| and should convert it to a
    // double store into |temp|.
    void convertValueToInt(ValueOperand value, MDefinition* input,
                           Label* handleStringEntry, Label* handleStringRejoin,
                           Label* truncateDoubleSlow,
                           Register stringReg, FloatRegister temp, Register output,
                           Label* fail, IntConversionBehavior behavior,
                           IntConversionInputKind conversion = IntConversionInputKind::Any);
    void convertValueToInt(ValueOperand value, FloatRegister temp, Register output, Label* fail,
                           IntConversionBehavior behavior)
    {
        convertValueToInt(value, nullptr, nullptr, nullptr, nullptr, InvalidReg, temp, output,
                          fail, behavior);
    }
    MOZ_MUST_USE bool convertValueToInt(JSContext* cx, const Value& v, Register output, Label* fail,
                                        IntConversionBehavior behavior);
    MOZ_MUST_USE bool convertConstantOrRegisterToInt(JSContext* cx,
                                                     const ConstantOrRegister& src,
                                                     FloatRegister temp, Register output,
                                                     Label* fail, IntConversionBehavior behavior);
    void convertTypedOrValueToInt(TypedOrValueRegister src, FloatRegister temp, Register output,
                                  Label* fail, IntConversionBehavior behavior);

    // This carries over the MToNumberInt32 operation on the ValueOperand
    // input; see comment at the top of this class.
    void convertValueToInt32(ValueOperand value, MDefinition* input,
                             FloatRegister temp, Register output, Label* fail,
                             bool negativeZeroCheck,
                             IntConversionInputKind conversion = IntConversionInputKind::Any)
    {
        convertValueToInt(value, input, nullptr, nullptr, nullptr, InvalidReg, temp, output, fail,
                          negativeZeroCheck
                          ? IntConversionBehavior::NegativeZeroCheck
                          : IntConversionBehavior::Normal,
                          conversion);
    }

    // This carries over the MTruncateToInt32 operation on the ValueOperand
    // input; see the comment at the top of this class.
    void truncateValueToInt32(ValueOperand value, MDefinition* input,
                              Label* handleStringEntry, Label* handleStringRejoin,
                              Label* truncateDoubleSlow,
                              Register stringReg, FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, handleStringEntry, handleStringRejoin, truncateDoubleSlow,
                          stringReg, temp, output, fail, IntConversionBehavior::Truncate);
    }

    void truncateValueToInt32(ValueOperand value, FloatRegister temp, Register output, Label* fail)
    {
        truncateValueToInt32(value, nullptr, nullptr, nullptr, nullptr, InvalidReg, temp, output,
                             fail);
    }

    MOZ_MUST_USE bool truncateConstantOrRegisterToInt32(JSContext* cx,
                                                        const ConstantOrRegister& src,
                                                        FloatRegister temp, Register output,
                                                        Label* fail)
    {
        return convertConstantOrRegisterToInt(cx, src, temp, output, fail, IntConversionBehavior::Truncate);
    }

    // Convenience functions for clamping values to uint8.
    void clampValueToUint8(ValueOperand value, MDefinition* input,
                           Label* handleStringEntry, Label* handleStringRejoin,
                           Register stringReg, FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, handleStringEntry, handleStringRejoin, nullptr,
                          stringReg, temp, output, fail, IntConversionBehavior::ClampToUint8);
    }

    MOZ_MUST_USE bool clampConstantOrRegisterToUint8(JSContext* cx,
                                                     const ConstantOrRegister& src,
                                                     FloatRegister temp, Register output,
                                                     Label* fail)
    {
        return convertConstantOrRegisterToInt(cx, src, temp, output, fail,
                                              IntConversionBehavior::ClampToUint8);
    }

    MOZ_MUST_USE bool icBuildOOLFakeExitFrame(void* fakeReturnAddr, AutoSaveLiveRegisters& save);

    // Align the stack pointer based on the number of arguments which are pushed
    // on the stack, such that the JitFrameLayout would be correctly aligned on
    // the JitStackAlignment.
    void alignJitStackBasedOnNArgs(Register nargs);
    void alignJitStackBasedOnNArgs(uint32_t nargs);

    inline void assertStackAlignment(uint32_t alignment, int32_t offset = 0);

    void performPendingReadBarriers();

  private:
    // Methods to get a singleton object or object group from a type set without
    // a read barrier, and record the result so that we can perform the barrier
    // later.
    JSObject* getSingletonAndDelayBarrier(const TypeSet* types, size_t i);
    ObjectGroup* getGroupAndDelayBarrier(const TypeSet* types, size_t i);

    Vector<JSObject*, 0, SystemAllocPolicy> pendingObjectReadBarriers_;
    Vector<ObjectGroup*, 0, SystemAllocPolicy> pendingObjectGroupReadBarriers_;
};

//{{{ check_macroassembler_style
inline uint32_t
MacroAssembler::framePushed() const
{
    return framePushed_;
}

inline void
MacroAssembler::setFramePushed(uint32_t framePushed)
{
    framePushed_ = framePushed;
}

inline void
MacroAssembler::adjustFrame(int32_t value)
{
    MOZ_ASSERT_IF(value < 0, framePushed_ >= uint32_t(-value));
    setFramePushed(framePushed_ + value);
}

inline void
MacroAssembler::implicitPop(uint32_t bytes)
{
    MOZ_ASSERT(bytes % sizeof(intptr_t) == 0);
    MOZ_ASSERT(bytes <= INT32_MAX);
    adjustFrame(-int32_t(bytes));
}
//}}} check_macroassembler_style

static inline Assembler::DoubleCondition
JSOpToDoubleCondition(JSOp op)
{
    switch (op) {
      case JSOP_EQ:
      case JSOP_STRICTEQ:
        return Assembler::DoubleEqual;
      case JSOP_NE:
      case JSOP_STRICTNE:
        return Assembler::DoubleNotEqualOrUnordered;
      case JSOP_LT:
        return Assembler::DoubleLessThan;
      case JSOP_LE:
        return Assembler::DoubleLessThanOrEqual;
      case JSOP_GT:
        return Assembler::DoubleGreaterThan;
      case JSOP_GE:
        return Assembler::DoubleGreaterThanOrEqual;
      default:
        MOZ_CRASH("Unexpected comparison operation");
    }
}

// Note: the op may have been inverted during lowering (to put constants in a
// position where they can be immediates), so it is important to use the
// lir->jsop() instead of the mir->jsop() when it is present.
static inline Assembler::Condition
JSOpToCondition(JSOp op, bool isSigned)
{
    if (isSigned) {
        switch (op) {
          case JSOP_EQ:
          case JSOP_STRICTEQ:
            return Assembler::Equal;
          case JSOP_NE:
          case JSOP_STRICTNE:
            return Assembler::NotEqual;
          case JSOP_LT:
            return Assembler::LessThan;
          case JSOP_LE:
            return Assembler::LessThanOrEqual;
          case JSOP_GT:
            return Assembler::GreaterThan;
          case JSOP_GE:
            return Assembler::GreaterThanOrEqual;
          default:
            MOZ_CRASH("Unrecognized comparison operation");
        }
    } else {
        switch (op) {
          case JSOP_EQ:
          case JSOP_STRICTEQ:
            return Assembler::Equal;
          case JSOP_NE:
          case JSOP_STRICTNE:
            return Assembler::NotEqual;
          case JSOP_LT:
            return Assembler::Below;
          case JSOP_LE:
            return Assembler::BelowOrEqual;
          case JSOP_GT:
            return Assembler::Above;
          case JSOP_GE:
            return Assembler::AboveOrEqual;
          default:
            MOZ_CRASH("Unrecognized comparison operation");
        }
    }
}

static inline size_t
StackDecrementForCall(uint32_t alignment, size_t bytesAlreadyPushed, size_t bytesToPush)
{
    return bytesToPush +
           ComputeByteAlignment(bytesAlreadyPushed + bytesToPush, alignment);
}

static inline MIRType
ToMIRType(MIRType t)
{
    return t;
}

static inline MIRType
ToMIRType(ABIArgType argType)
{
    switch (argType) {
      case ArgType_General: return MIRType::Int32;
      case ArgType_Double:  return MIRType::Double;
      case ArgType_Float32: return MIRType::Float32;
      case ArgType_Int64:   return MIRType::Int64;
      default: break;
    }
    MOZ_CRASH("unexpected argType");
}

template <class VecT>
class ABIArgIter
{
    ABIArgGenerator gen_;
    const VecT& types_;
    unsigned i_;

    void settle() { if (!done()) gen_.next(ToMIRType(types_[i_])); }

  public:
    explicit ABIArgIter(const VecT& types) : types_(types), i_(0) { settle(); }
    void operator++(int) { MOZ_ASSERT(!done()); i_++; settle(); }
    bool done() const { return i_ == types_.length(); }

    ABIArg* operator->() { MOZ_ASSERT(!done()); return &gen_.current(); }
    ABIArg& operator*() { MOZ_ASSERT(!done()); return gen_.current(); }

    unsigned index() const { MOZ_ASSERT(!done()); return i_; }
    MIRType mirType() const { MOZ_ASSERT(!done()); return ToMIRType(types_[i_]); }
    uint32_t stackBytesConsumedSoFar() const { return gen_.stackBytesConsumedSoFar(); }
};

} // namespace jit
} // namespace js

#endif /* jit_MacroAssembler_h */
