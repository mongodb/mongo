/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MacroAssembler_h
#define jit_MacroAssembler_h

#include "mozilla/MacroForEach.h"
#include "mozilla/MathAlgorithms.h"

#include "jscompartment.h"

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
#include "jit/JitCompartment.h"
#include "jit/VMFunctions.h"
#include "vm/ProxyObject.h"
#include "vm/Shape.h"
#include "vm/UnboxedObject.h"

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

# define ALL_ARCH mips32, mips64, arm, arm64, x86, x64
# define ALL_SHARED_ARCH arm, arm64, x86_shared, mips_shared

// * How this macro works:
//
// DEFINED_ON is a macro which check if, for the current architecture, the
// method is defined on the macro assembler or not.
//
// For each architecutre, we have a macro named DEFINED_ON_arch.  This macro is
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
// or to nothing, if the current architecture is not lsited in the list of
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


#ifdef IS_LITTLE_ENDIAN
#define IMM32_16ADJ(X) X << 16
#else
#define IMM32_16ADJ(X) X
#endif

namespace js {
namespace jit {

// Defined in JitFrames.h
enum ExitFrameTokenValues;

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

        virtual void emit(MacroAssembler& masm) = 0;
    };

    /*
     * Creates a branch based on a specific TypeSet::Type.
     * Note: emits number test (int/double) for TypeSet::DoubleType()
     */
    class BranchType : public Branch
    {
        TypeSet::Type type_;

      public:
        BranchType()
          : Branch(),
            type_(TypeSet::UnknownType())
        { }

        BranchType(Condition cond, Register reg, TypeSet::Type type, Label* jump)
          : Branch(cond, reg, jump),
            type_(type)
        { }

        void emit(MacroAssembler& masm) {
            MOZ_ASSERT(isInitialized());
            MIRType mirType = MIRType_None;

            if (type_.isPrimitive()) {
                if (type_.isMagicArguments())
                    mirType = MIRType_MagicOptimizedArguments;
                else
                    mirType = MIRTypeFromValueType(type_.primitive());
            } else if (type_.isAnyObject()) {
                mirType = MIRType_Object;
            } else {
                MOZ_CRASH("Unknown conversion to mirtype");
            }

            if (mirType == MIRType_Double)
                masm.branchTestNumber(cond(), reg(), jump());
            else
                masm.branchTestMIRType(cond(), reg(), mirType, jump());
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

        void emit(MacroAssembler& masm) {
            MOZ_ASSERT(isInitialized());
            masm.branchPtr(cond(), reg(), ptr_, jump());
        }
    };

    mozilla::Maybe<AutoRooter> autoRooter_;
    mozilla::Maybe<JitContext> jitContext_;
    mozilla::Maybe<AutoJitContextAlloc> alloc_;

  private:
    // Labels for handling exceptions and failures.
    NonAssertingLabel failureLabel_;

    // Asm failure labels
    NonAssertingLabel asmStackOverflowLabel_;
    NonAssertingLabel asmSyncInterruptLabel_;
    NonAssertingLabel asmOnConversionErrorLabel_;
    NonAssertingLabel asmOnOutOfBoundsLabel_;

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

    // asm.js compilation handles its own JitContext-pushing
    struct AsmJSToken {};
    explicit MacroAssembler(AsmJSToken, TempAllocator *alloc)
      : framePushed_(0),
#ifdef DEBUG
        inCall_(false),
#endif
        emitProfilingInstrumentation_(false)
    {
        if (alloc)
            moveResolver_.setAllocator(*alloc);

#if defined(JS_CODEGEN_ARM)
        initWithAllocator();
        m_buffer.id = 0;
#elif defined(JS_CODEGEN_ARM64)
        initWithAllocator();
        armbuffer_.id = 0;
#endif
    }

    void constructRoot(JSContext* cx) {
        autoRooter_.emplace(cx, this);
    }

    MoveResolver& moveResolver() {
        return moveResolver_;
    }

    size_t instructionsSize() const {
        return size();
    }

    //{{{ check_macroassembler_style
  public:
    // ===============================================================
    // Frame manipulation functions.

    inline uint32_t framePushed() const;
    inline void setFramePushed(uint32_t framePushed);
    inline void adjustFrame(int32_t value);

    // Adjust the frame, to account for implicit modification of the stack
    // pointer, such that callee can remove arguments on the behalf of the
    // caller.
    inline void implicitPop(uint32_t bytes);

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
    void Push(jsid id, Register scratchReg);
    void Push(TypedOrValueRegister v);
    void Push(ConstantOrRegister v);
    void Push(const ValueOperand& val);
    void Push(const Value& val);
    void Push(JSValueType type, Register reg);
    void PushValue(const Address& addr);
    void PushEmptyRooted(VMFunction::RootType rootType);
    inline CodeOffset PushWithPatch(ImmWord word);
    inline CodeOffset PushWithPatch(ImmPtr imm);

    void Pop(const Operand op) DEFINED_ON(x86_shared);
    void Pop(Register reg) PER_SHARED_ARCH;
    void Pop(FloatRegister t) DEFINED_ON(x86_shared);
    void Pop(const ValueOperand& val) PER_SHARED_ARCH;
    void popRooted(VMFunction::RootType rootType, Register cellReg, const ValueOperand& valueReg);

    // Move the stack pointer based on the requested amount.
    void adjustStack(int amount);
    void reserveStack(uint32_t amount) PER_ARCH;
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
    void call(const Address& addr) DEFINED_ON(x86_shared);
    void call(ImmWord imm) PER_SHARED_ARCH;
    // Call a target native function, which is neither traceable nor movable.
    void call(ImmPtr imm) PER_SHARED_ARCH;
    void call(wasm::SymbolicAddress imm) PER_SHARED_ARCH;
    // Call a target JitCode, which must be traceable, and may be movable.
    void call(JitCode* c) PER_SHARED_ARCH;

    inline void call(const wasm::CallSiteDesc& desc, const Register reg);
    inline void call(const wasm::CallSiteDesc& desc, Label* label);
    inline void call(const wasm::CallSiteDesc& desc, AsmJSInternalCallee callee);

    CodeOffset callWithPatch() PER_SHARED_ARCH;
    void patchCall(uint32_t callerOffset, uint32_t calleeOffset) PER_SHARED_ARCH;

    // Push the return address and make a call. On platforms where this function
    // is not defined, push the link register (pushReturnAddress) at the entry
    // point of the callee.
    void callAndPushReturnAddress(Register reg) DEFINED_ON(mips_shared, x86_shared);
    void callAndPushReturnAddress(Label* label) DEFINED_ON(mips_shared, x86_shared);

    void pushReturnAddress() DEFINED_ON(arm, arm64);

  public:
    // ===============================================================
    // ABI function calls.

    // Setup a call to C/C++ code, given the assumption that the framePushed
    // accruately define the state of the stack, and that the top of the stack
    // was properly aligned. Note that this only supports cdecl.
    void setupAlignedABICall(); // CRASH_ON(arm64)

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

    template <typename T>
    inline void callWithABI(const T& fun, MoveOp::Type result = MoveOp::GENERAL);

  private:
    // Reinitialize the variables which have to be cleared before making a call
    // with callWithABI.
    void setupABICall();

    // Reserve the stack and resolve the arguments move.
    void callWithABIPre(uint32_t* stackAdjust, bool callFromAsmJS = false) PER_ARCH;

    // Emits a call to a C/C++ function, resolving all argument moves.
    void callWithABINoProfiler(void* fun, MoveOp::Type result);
    void callWithABINoProfiler(wasm::SymbolicAddress imm, MoveOp::Type result);
    void callWithABINoProfiler(Register fun, MoveOp::Type result) PER_ARCH;
    void callWithABINoProfiler(const Address& fun, MoveOp::Type result) PER_ARCH;

    // Restore the stack to its state before the setup function call.
    void callWithABIPost(uint32_t stackAdjust, MoveOp::Type result) PER_ARCH;

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

    // The frame descriptor is the second field of all Jit frames, pushed before
    // calling the Jit function.  It is a composite value defined in JitFrames.h
    inline void makeFrameDescriptor(Register frameSizeReg, FrameType type);

    // Push the frame descriptor, based on the statically known framePushed.
    inline void pushStaticFrameDescriptor(FrameType type);

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

    // If the current piece of code might be garbage collected, then the exit
    // frame footer must contain a pointer to the current JitCode, such that the
    // garbage collector can keep the code alive as long this code is on the
    // stack. This function pushes a placeholder which is replaced when the code
    // is linked.
    inline void PushStubCode();

    // Return true if the code contains a self-reference which needs to be
    // patched when the code is linked.
    inline bool hasSelfReference() const;

    // Push stub code and the VMFunction pointer.
    inline void enterExitFrame(const VMFunction* f = nullptr);

    // Push an exit frame token to identify which fake exit frame this footer
    // corresponds to.
    inline void enterFakeExitFrame(enum ExitFrameTokenValues token);

    // Push an exit frame token for a native call.
    inline void enterFakeExitFrameForNative(bool isConstructing);

    // Pop ExitFrame footer in addition to the extra frame.
    inline void leaveExitFrame(size_t extraFrame = 0);

  private:
    // Save the top of the stack into PerThreadData::jitTop of the main thread,
    // which should be the location of the latest exit frame.
    void linkExitFrame();

    // Patch the value of PushStubCode with the pointer to the finalized code.
    void linkSelfReference(JitCode* code);

    // If the JitCode that created this assembler needs to transition into the VM,
    // we want to store the JitCode on the stack in order to mark it during a GC.
    // This is a reference to a patch location where the JitCode* will be written.
    CodeOffset selfReferencePatch_;

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

    inline void or32(Register src, Register dest) PER_SHARED_ARCH;
    inline void or32(Imm32 imm, Register dest) PER_SHARED_ARCH;
    inline void or32(Imm32 imm, const Address& dest) PER_SHARED_ARCH;

    inline void orPtr(Register src, Register dest) PER_ARCH;
    inline void orPtr(Imm32 imm, Register dest) PER_ARCH;

    inline void or64(Register64 src, Register64 dest) PER_ARCH;
    inline void xor64(Register64 src, Register64 dest) PER_ARCH;

    inline void xor32(Register src, Register dest) DEFINED_ON(x86_shared);
    inline void xor32(Imm32 imm, Register dest) PER_SHARED_ARCH;

    inline void xorPtr(Register src, Register dest) PER_ARCH;
    inline void xorPtr(Imm32 imm, Register dest) PER_ARCH;

    // ===============================================================
    // Arithmetic functions

    inline void sub32(const Address& src, Register dest) PER_SHARED_ARCH;
    inline void sub32(Register src, Register dest) PER_SHARED_ARCH;
    inline void sub32(Imm32 imm, Register dest) PER_SHARED_ARCH;

    inline void add64(Register64 src, Register64 dest) PER_ARCH;

    // ===============================================================
    // Shift functions

    inline void lshiftPtr(Imm32 imm, Register dest) PER_ARCH;

    inline void lshift64(Imm32 imm, Register64 dest) PER_ARCH;

    inline void rshiftPtr(Imm32 imm, Register dest) PER_ARCH;
    inline void rshiftPtr(Imm32 imm, Register src, Register dest) DEFINED_ON(arm64);

    inline void rshiftPtrArithmetic(Imm32 imm, Register dest) PER_ARCH;

    inline void rshift64(Imm32 imm, Register64 dest) PER_ARCH;

    //}}} check_macroassembler_style
  public:

    // Emits a test of a value against all types in a TypeSet. A scratch
    // register is required.
    template <typename Source>
    void guardTypeSet(const Source& address, const TypeSet* types, BarrierKind kind, Register scratch, Label* miss);

    void guardObjectType(Register obj, const TypeSet* types, Register scratch, Label* miss);

    template <typename TypeSet>
    void guardTypeSetMightBeIncomplete(TypeSet* types, Register obj, Register scratch, Label* label);

    void loadObjShape(Register objReg, Register dest) {
        loadPtr(Address(objReg, JSObject::offsetOfShape()), dest);
    }
    void loadObjGroup(Register objReg, Register dest) {
        loadPtr(Address(objReg, JSObject::offsetOfGroup()), dest);
    }
    void loadBaseShape(Register objReg, Register dest) {
        loadObjShape(objReg, dest);
        loadPtr(Address(dest, Shape::offsetOfBase()), dest);
    }
    void loadObjClass(Register objReg, Register dest) {
        loadObjGroup(objReg, dest);
        loadPtr(Address(dest, ObjectGroup::offsetOfClasp()), dest);
    }
    void branchTestObjClass(Condition cond, Register obj, Register scratch, const js::Class* clasp,
                            Label* label) {
        loadObjGroup(obj, scratch);
        branchPtr(cond, Address(scratch, ObjectGroup::offsetOfClasp()), ImmPtr(clasp), label);
    }
    void branchTestObjShape(Condition cond, Register obj, const Shape* shape, Label* label) {
        branchPtr(cond, Address(obj, JSObject::offsetOfShape()), ImmGCPtr(shape), label);
    }
    void branchTestObjShape(Condition cond, Register obj, Register shape, Label* label) {
        branchPtr(cond, Address(obj, JSObject::offsetOfShape()), shape, label);
    }
    void branchTestObjGroup(Condition cond, Register obj, ObjectGroup* group, Label* label) {
        branchPtr(cond, Address(obj, JSObject::offsetOfGroup()), ImmGCPtr(group), label);
    }
    void branchTestObjGroup(Condition cond, Register obj, Register group, Label* label) {
        branchPtr(cond, Address(obj, JSObject::offsetOfGroup()), group, label);
    }
    void branchTestProxyHandlerFamily(Condition cond, Register proxy, Register scratch,
                                      const void* handlerp, Label* label) {
        Address handlerAddr(proxy, ProxyObject::offsetOfHandler());
        loadPtr(handlerAddr, scratch);
        Address familyAddr(scratch, BaseProxyHandler::offsetOfFamily());
        branchPtr(cond, familyAddr, ImmPtr(handlerp), label);
    }

    template <typename Value>
    void branchTestMIRType(Condition cond, const Value& val, MIRType type, Label* label) {
        switch (type) {
          case MIRType_Null:      return branchTestNull(cond, val, label);
          case MIRType_Undefined: return branchTestUndefined(cond, val, label);
          case MIRType_Boolean:   return branchTestBoolean(cond, val, label);
          case MIRType_Int32:     return branchTestInt32(cond, val, label);
          case MIRType_String:    return branchTestString(cond, val, label);
          case MIRType_Symbol:    return branchTestSymbol(cond, val, label);
          case MIRType_Object:    return branchTestObject(cond, val, label);
          case MIRType_Double:    return branchTestDouble(cond, val, label);
          case MIRType_MagicOptimizedArguments: // Fall through.
          case MIRType_MagicIsConstructing:
          case MIRType_MagicHole: return branchTestMagic(cond, val, label);
          default:
            MOZ_CRASH("Bad MIRType");
        }
    }

    // Branches to |label| if |reg| is false. |reg| should be a C++ bool.
    void branchIfFalseBool(Register reg, Label* label) {
        // Note that C++ bool is only 1 byte, so ignore the higher-order bits.
        branchTest32(Assembler::Zero, reg, Imm32(0xFF), label);
    }

    // Branches to |label| if |reg| is true. |reg| should be a C++ bool.
    void branchIfTrueBool(Register reg, Label* label) {
        // Note that C++ bool is only 1 byte, so ignore the higher-order bits.
        branchTest32(Assembler::NonZero, reg, Imm32(0xFF), label);
    }

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

    void loadStringChars(Register str, Register dest);
    void loadStringChar(Register str, Register index, Register output);

    void branchIfRope(Register str, Label* label) {
        Address flags(str, JSString::offsetOfFlags());
        static_assert(JSString::ROPE_FLAGS == 0, "Rope type flags must be 0");
        branchTest32(Assembler::Zero, flags, Imm32(JSString::TYPE_FLAGS_MASK), label);
    }

    void loadJSContext(Register dest) {
        loadPtr(AbsoluteAddress(GetJitContext()->runtime->addressOfJSContext()), dest);
    }
    void loadJitActivation(Register dest) {
        loadPtr(AbsoluteAddress(GetJitContext()->runtime->addressOfActivation()), dest);
    }

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
            if (src.type() == MIRType_Float32) {
                convertFloat32ToDouble(reg, ScratchDoubleReg);
                reg = ScratchDoubleReg;
            }
            storeDouble(reg, dest);
        } else {
            storeValue(ValueTypeFromMIRType(src.type()), src.typedReg().gpr(), dest);
        }
    }

    template <typename T>
    void storeObjectOrNull(Register src, const T& dest) {
        Label notNull, done;
        branchTestPtr(Assembler::NonZero, src, src, &notNull);
        storeValue(NullValue(), dest);
        jump(&done);
        bind(&notNull);
        storeValue(JSVAL_TYPE_OBJECT, src, dest);
        bind(&done);
    }

    template <typename T>
    void storeConstantOrRegister(ConstantOrRegister src, const T& dest) {
        if (src.constant())
            storeValue(src.value(), dest);
        else
            storeTypedOrValue(src.reg(), dest);
    }

    void storeCallResult(Register reg) {
        if (reg != ReturnReg)
            mov(ReturnReg, reg);
    }

    void storeCallFloatResult(FloatRegister reg) {
        if (reg != ReturnDoubleReg)
            moveDouble(ReturnDoubleReg, reg);
    }

    void storeCallResultValue(AnyRegister dest) {
#if defined(JS_NUNBOX32)
        unboxValue(ValueOperand(JSReturnReg_Type, JSReturnReg_Data), dest);
#elif defined(JS_PUNBOX64)
        unboxValue(ValueOperand(JSReturnReg), dest);
#else
#error "Bad architecture"
#endif
    }

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

    void storeCallResultValue(TypedOrValueRegister dest) {
        if (dest.hasValue())
            storeCallResultValue(dest.valueReg());
        else
            storeCallResultValue(dest.typedReg());
    }

    template <typename T>
    Register extractString(const T& source, Register scratch) {
        return extractObject(source, scratch);
    }

    void branchIfFunctionHasNoScript(Register fun, Label* label) {
        // 16-bit loads are slow and unaligned 32-bit loads may be too so
        // perform an aligned 32-bit load and adjust the bitmask accordingly.
        MOZ_ASSERT(JSFunction::offsetOfNargs() % sizeof(uint32_t) == 0);
        MOZ_ASSERT(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2);
        Address address(fun, JSFunction::offsetOfNargs());
        int32_t bit = IMM32_16ADJ(JSFunction::INTERPRETED);
        branchTest32(Assembler::Zero, address, Imm32(bit), label);
    }
    void branchIfInterpreted(Register fun, Label* label) {
        // 16-bit loads are slow and unaligned 32-bit loads may be too so
        // perform an aligned 32-bit load and adjust the bitmask accordingly.
        MOZ_ASSERT(JSFunction::offsetOfNargs() % sizeof(uint32_t) == 0);
        MOZ_ASSERT(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2);
        Address address(fun, JSFunction::offsetOfNargs());
        int32_t bit = IMM32_16ADJ(JSFunction::INTERPRETED);
        branchTest32(Assembler::NonZero, address, Imm32(bit), label);
    }

    void branchIfNotInterpretedConstructor(Register fun, Register scratch, Label* label);

    void bumpKey(Int32Key* key, int diff) {
        if (key->isRegister())
            add32(Imm32(diff), key->reg());
        else
            key->bumpConstant(diff);
    }

    void storeKey(const Int32Key& key, const Address& dest) {
        if (key.isRegister())
            store32(key.reg(), dest);
        else
            store32(Imm32(key.constant()), dest);
    }

    template<typename T>
    void branchKey(Condition cond, const T& length, const Int32Key& key, Label* label) {
        if (key.isRegister())
            branch32(cond, length, key.reg(), label);
        else
            branch32(cond, length, Imm32(key.constant()), label);
    }

    void branchTestNeedsIncrementalBarrier(Condition cond, Label* label) {
        MOZ_ASSERT(cond == Zero || cond == NonZero);
        CompileZone* zone = GetJitContext()->compartment->zone();
        AbsoluteAddress needsBarrierAddr(zone->addressOfNeedsIncrementalBarrier());
        branchTest32(cond, needsBarrierAddr, Imm32(0x1), label);
    }

    template <typename T>
    void callPreBarrier(const T& address, MIRType type) {
        Label done;

        if (type == MIRType_Value)
            branchTestGCThing(Assembler::NotEqual, address, &done);

        Push(PreBarrierReg);
        computeEffectiveAddress(address, PreBarrierReg);

        const JitRuntime* rt = GetJitContext()->runtime->jitRuntime();
        JitCode* preBarrier = rt->preBarrier(type);

        call(preBarrier);
        Pop(PreBarrierReg);

        bind(&done);
    }

    template <typename T>
    void patchableCallPreBarrier(const T& address, MIRType type) {
        Label done;

        // All barriers are off by default.
        // They are enabled if necessary at the end of CodeGenerator::generate().
        CodeOffset nopJump = toggledJump(&done);
        writePrebarrierOffset(nopJump);

        callPreBarrier(address, type);
        jump(&done);

        haltingAlign(8);
        bind(&done);
    }

    void canonicalizeDouble(FloatRegister reg) {
        Label notNaN;
        branchDouble(DoubleOrdered, reg, reg, &notNaN);
        loadConstantDouble(JS::GenericNaN(), reg);
        bind(&notNaN);
    }

    void canonicalizeFloat(FloatRegister reg) {
        Label notNaN;
        branchFloat(DoubleOrdered, reg, reg, &notNaN);
        loadConstantFloat32(float(JS::GenericNaN()), reg);
        bind(&notNaN);
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

    // Load a property from an UnboxedPlainObject or UnboxedArrayObject.
    template <typename T>
    void loadUnboxedProperty(T address, JSValueType type, TypedOrValueRegister output);

    // Store a property to an UnboxedPlainObject, without triggering barriers.
    // If failure is null, the value definitely has a type suitable for storing
    // in the property.
    template <typename T>
    void storeUnboxedProperty(T address, JSValueType type,
                              ConstantOrRegister value, Label* failure);

    void checkUnboxedArrayCapacity(Register obj, const Int32Key& index, Register temp,
                                   Label* failure);

    Register extractString(const Address& address, Register scratch) {
        return extractObject(address, scratch);
    }
    Register extractString(const ValueOperand& value, Register scratch) {
        return extractObject(value, scratch);
    }

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
        MOZ_ASSERT(reg.type() == MIRType_Object);
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

    // Emit type case branch on tag matching if the type tag in the definition
    // might actually be that type.
    void branchEqualTypeIfNeeded(MIRType type, MDefinition* maybeDef, Register tag, Label* label);

    // Inline allocation.
  private:
    void checkAllocatorState(Label* fail);
    bool shouldNurseryAllocate(gc::AllocKind allocKind, gc::InitialHeap initialHeap);
    void nurseryAllocate(Register result, Register temp, gc::AllocKind allocKind,
                         size_t nDynamicSlots, gc::InitialHeap initialHeap, Label* fail);
    void freeListAllocate(Register result, Register temp, gc::AllocKind allocKind, Label* fail);
    void allocateObject(Register result, Register temp, gc::AllocKind allocKind,
                        uint32_t nDynamicSlots, gc::InitialHeap initialHeap, Label* fail);
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

    void initUnboxedObjectContents(Register object, UnboxedPlainObject* templateObject);

    void newGCString(Register result, Register temp, Label* fail);
    void newGCFatInlineString(Register result, Register temp, Label* fail);

    // Compares two strings for equality based on the JSOP.
    // This checks for identical pointers, atoms and length and fails for everything else.
    void compareStrings(JSOp op, Register left, Register right, Register result,
                        Label* fail);

  public:
    // Generates code used to complete a bailout.
    void generateBailoutTail(Register scratch, Register bailoutInfo);

    void branchTestObjectTruthy(bool truthy, Register objReg, Register scratch,
                                Label* slowCheck, Label* checked)
    {
        // The branches to out-of-line code here implement a conservative version
        // of the JSObject::isWrapper test performed in EmulatesUndefined.  If none
        // of the branches are taken, we can check class flags directly.
        loadObjClass(objReg, scratch);
        Address flags(scratch, Class::offsetOfFlags());

        branchTestClassIsProxy(true, scratch, slowCheck);

        Condition cond = truthy ? Assembler::Zero : Assembler::NonZero;
        branchTest32(cond, flags, Imm32(JSCLASS_EMULATES_UNDEFINED), checked);
    }

    void branchTestClassIsProxy(bool proxy, Register clasp, Label* label)
    {
        branchTest32(proxy ? Assembler::NonZero : Assembler::Zero,
                     Address(clasp, Class::offsetOfFlags()),
                     Imm32(JSCLASS_IS_PROXY), label);
    }

    void branchTestObjectIsProxy(bool proxy, Register object, Register scratch, Label* label)
    {
        loadObjClass(object, scratch);
        branchTestClassIsProxy(proxy, scratch, label);
    }

    inline void branchFunctionKind(Condition cond, JSFunction::FunctionKind kind, Register fun,
                                   Register scratch, Label* label);

  public:
#ifndef JS_CODEGEN_ARM64
    // StackPointer manipulation functions.
    // On ARM64, the StackPointer is implemented as two synchronized registers.
    // Code shared across platforms must use these functions to be valid.
    template <typename T>
    void addToStackPtr(T t) { addPtr(t, getStackPointer()); }
    template <typename T>
    void addStackPtrTo(T t) { addPtr(getStackPointer(), t); }

    template <typename T>
    void subFromStackPtr(T t) { subPtr(t, getStackPointer()); }
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
    void branchTestStackPtr(Condition cond, T t, Label* label) {
        branchTestPtr(cond, getStackPointer(), t, label);
    }
    template <typename T>
    void branchStackPtr(Condition cond, T rhs, Label* label) {
        branchPtr(cond, getStackPointer(), rhs, label);
    }
    template <typename T>
    void branchStackPtrRhs(Condition cond, T lhs, Label* label) {
        branchPtr(cond, lhs, getStackPointer(), label);
    }
#endif // !JS_CODEGEN_ARM64

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
    void loadBaselineOrIonRaw(Register script, Register dest, Label* failure);
    void loadBaselineOrIonNoArgCheck(Register callee, Register dest, Label* failure);

    void loadBaselineFramePtr(Register framePtr, Register dest);

    void pushBaselineFramePtr(Register framePtr, Register scratch) {
        loadBaselineFramePtr(framePtr, scratch);
        push(scratch);
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

    Label* asmSyncInterruptLabel() {
        return &asmSyncInterruptLabel_;
    }
    const Label* asmSyncInterruptLabel() const {
        return &asmSyncInterruptLabel_;
    }
    Label* asmStackOverflowLabel() {
        return &asmStackOverflowLabel_;
    }
    const Label* asmStackOverflowLabel() const {
        return &asmStackOverflowLabel_;
    }
    Label* asmOnOutOfBoundsLabel() {
        return &asmOnOutOfBoundsLabel_;
    }
    const Label* asmOnOutOfBoundsLabel() const {
        return &asmOnOutOfBoundsLabel_;
    }
    Label* asmOnConversionErrorLabel() {
        return &asmOnConversionErrorLabel_;
    }
    const Label* asmOnConversionErrorLabel() const {
        return &asmOnConversionErrorLabel_;
    }

    bool asmMergeWith(const MacroAssembler& masm);
    void finish();
    void link(JitCode* code);

    void assumeUnreachable(const char* output);

    template<typename T>
    void assertTestInt32(Condition cond, const T& value, const char* output);

    void printf(const char* output);
    void printf(const char* output, Register value);

#ifdef JS_TRACE_LOGGING
    void tracelogStartId(Register logger, uint32_t textId, bool force = false);
    void tracelogStartId(Register logger, Register textId);
    void tracelogStartEvent(Register logger, Register event);
    void tracelogStopId(Register logger, uint32_t textId, bool force = false);
    void tracelogStopId(Register logger, Register textId);
#endif

#define DISPATCH_FLOATING_POINT_OP(method, type, arg1d, arg1f, arg2)    \
    MOZ_ASSERT(IsFloatingPointType(type));                              \
    if (type == MIRType_Double)                                         \
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
    bool convertValueToFloatingPoint(JSContext* cx, const Value& v, FloatRegister output,
                                     Label* fail, MIRType outputType);
    bool convertConstantOrRegisterToFloatingPoint(JSContext* cx, ConstantOrRegister src,
                                                  FloatRegister output, Label* fail,
                                                  MIRType outputType);
    void convertTypedOrValueToFloatingPoint(TypedOrValueRegister src, FloatRegister output,
                                            Label* fail, MIRType outputType);

    void convertInt32ValueToDouble(const Address& address, Register scratch, Label* done);
    void convertValueToDouble(ValueOperand value, FloatRegister output, Label* fail) {
        convertValueToFloatingPoint(value, output, fail, MIRType_Double);
    }
    bool convertValueToDouble(JSContext* cx, const Value& v, FloatRegister output, Label* fail) {
        return convertValueToFloatingPoint(cx, v, output, fail, MIRType_Double);
    }
    bool convertConstantOrRegisterToDouble(JSContext* cx, ConstantOrRegister src,
                                           FloatRegister output, Label* fail)
    {
        return convertConstantOrRegisterToFloatingPoint(cx, src, output, fail, MIRType_Double);
    }
    void convertTypedOrValueToDouble(TypedOrValueRegister src, FloatRegister output, Label* fail) {
        convertTypedOrValueToFloatingPoint(src, output, fail, MIRType_Double);
    }

    void convertValueToFloat(ValueOperand value, FloatRegister output, Label* fail) {
        convertValueToFloatingPoint(value, output, fail, MIRType_Float32);
    }
    bool convertValueToFloat(JSContext* cx, const Value& v, FloatRegister output, Label* fail) {
        return convertValueToFloatingPoint(cx, v, output, fail, MIRType_Float32);
    }
    bool convertConstantOrRegisterToFloat(JSContext* cx, ConstantOrRegister src,
                                          FloatRegister output, Label* fail)
    {
        return convertConstantOrRegisterToFloatingPoint(cx, src, output, fail, MIRType_Float32);
    }
    void convertTypedOrValueToFloat(TypedOrValueRegister src, FloatRegister output, Label* fail) {
        convertTypedOrValueToFloatingPoint(src, output, fail, MIRType_Float32);
    }

    enum IntConversionBehavior {
        IntConversion_Normal,
        IntConversion_NegativeZeroCheck,
        IntConversion_Truncate,
        IntConversion_ClampToUint8,
    };

    enum IntConversionInputKind {
        IntConversion_NumbersOnly,
        IntConversion_NumbersOrBoolsOnly,
        IntConversion_Any
    };

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
                           IntConversionInputKind conversion = IntConversion_Any);
    void convertValueToInt(ValueOperand value, FloatRegister temp, Register output, Label* fail,
                           IntConversionBehavior behavior)
    {
        convertValueToInt(value, nullptr, nullptr, nullptr, nullptr, InvalidReg, temp, output,
                          fail, behavior);
    }
    bool convertValueToInt(JSContext* cx, const Value& v, Register output, Label* fail,
                           IntConversionBehavior behavior);
    bool convertConstantOrRegisterToInt(JSContext* cx, ConstantOrRegister src, FloatRegister temp,
                                        Register output, Label* fail, IntConversionBehavior behavior);
    void convertTypedOrValueToInt(TypedOrValueRegister src, FloatRegister temp, Register output,
                                  Label* fail, IntConversionBehavior behavior);

    //
    // Convenience functions for converting values to int32.
    //
    void convertValueToInt32(ValueOperand value, FloatRegister temp, Register output, Label* fail,
                             bool negativeZeroCheck)
    {
        convertValueToInt(value, temp, output, fail, negativeZeroCheck
                          ? IntConversion_NegativeZeroCheck
                          : IntConversion_Normal);
    }
    void convertValueToInt32(ValueOperand value, MDefinition* input,
                             FloatRegister temp, Register output, Label* fail,
                             bool negativeZeroCheck, IntConversionInputKind conversion = IntConversion_Any)
    {
        convertValueToInt(value, input, nullptr, nullptr, nullptr, InvalidReg, temp, output, fail,
                          negativeZeroCheck
                          ? IntConversion_NegativeZeroCheck
                          : IntConversion_Normal,
                          conversion);
    }
    bool convertValueToInt32(JSContext* cx, const Value& v, Register output, Label* fail,
                             bool negativeZeroCheck)
    {
        return convertValueToInt(cx, v, output, fail, negativeZeroCheck
                                 ? IntConversion_NegativeZeroCheck
                                 : IntConversion_Normal);
    }
    bool convertConstantOrRegisterToInt32(JSContext* cx, ConstantOrRegister src, FloatRegister temp,
                                          Register output, Label* fail, bool negativeZeroCheck)
    {
        return convertConstantOrRegisterToInt(cx, src, temp, output, fail, negativeZeroCheck
                                              ? IntConversion_NegativeZeroCheck
                                              : IntConversion_Normal);
    }
    void convertTypedOrValueToInt32(TypedOrValueRegister src, FloatRegister temp, Register output,
                                    Label* fail, bool negativeZeroCheck)
    {
        convertTypedOrValueToInt(src, temp, output, fail, negativeZeroCheck
                                 ? IntConversion_NegativeZeroCheck
                                 : IntConversion_Normal);
    }

    //
    // Convenience functions for truncating values to int32.
    //
    void truncateValueToInt32(ValueOperand value, FloatRegister temp, Register output, Label* fail) {
        convertValueToInt(value, temp, output, fail, IntConversion_Truncate);
    }
    void truncateValueToInt32(ValueOperand value, MDefinition* input,
                              Label* handleStringEntry, Label* handleStringRejoin,
                              Label* truncateDoubleSlow,
                              Register stringReg, FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, handleStringEntry, handleStringRejoin, truncateDoubleSlow,
                          stringReg, temp, output, fail, IntConversion_Truncate);
    }
    void truncateValueToInt32(ValueOperand value, MDefinition* input,
                              FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, nullptr, nullptr, nullptr, InvalidReg, temp, output, fail,
                          IntConversion_Truncate);
    }
    bool truncateValueToInt32(JSContext* cx, const Value& v, Register output, Label* fail) {
        return convertValueToInt(cx, v, output, fail, IntConversion_Truncate);
    }
    bool truncateConstantOrRegisterToInt32(JSContext* cx, ConstantOrRegister src, FloatRegister temp,
                                           Register output, Label* fail)
    {
        return convertConstantOrRegisterToInt(cx, src, temp, output, fail, IntConversion_Truncate);
    }
    void truncateTypedOrValueToInt32(TypedOrValueRegister src, FloatRegister temp, Register output,
                                     Label* fail)
    {
        convertTypedOrValueToInt(src, temp, output, fail, IntConversion_Truncate);
    }

    // Convenience functions for clamping values to uint8.
    void clampValueToUint8(ValueOperand value, FloatRegister temp, Register output, Label* fail) {
        convertValueToInt(value, temp, output, fail, IntConversion_ClampToUint8);
    }
    void clampValueToUint8(ValueOperand value, MDefinition* input,
                           Label* handleStringEntry, Label* handleStringRejoin,
                           Register stringReg, FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, handleStringEntry, handleStringRejoin, nullptr,
                          stringReg, temp, output, fail, IntConversion_ClampToUint8);
    }
    void clampValueToUint8(ValueOperand value, MDefinition* input,
                           FloatRegister temp, Register output, Label* fail)
    {
        convertValueToInt(value, input, nullptr, nullptr, nullptr, InvalidReg, temp, output, fail,
                          IntConversion_ClampToUint8);
    }
    bool clampValueToUint8(JSContext* cx, const Value& v, Register output, Label* fail) {
        return convertValueToInt(cx, v, output, fail, IntConversion_ClampToUint8);
    }
    bool clampConstantOrRegisterToUint8(JSContext* cx, ConstantOrRegister src, FloatRegister temp,
                                        Register output, Label* fail)
    {
        return convertConstantOrRegisterToInt(cx, src, temp, output, fail,
                                              IntConversion_ClampToUint8);
    }
    void clampTypedOrValueToUint8(TypedOrValueRegister src, FloatRegister temp, Register output,
                                  Label* fail)
    {
        convertTypedOrValueToInt(src, temp, output, fail, IntConversion_ClampToUint8);
    }

  public:
    class AfterICSaveLive {
        friend class MacroAssembler;
        explicit AfterICSaveLive(uint32_t initialStack)
#ifdef JS_DEBUG
          : initialStack(initialStack)
#endif
        {}

      public:
#ifdef JS_DEBUG
        uint32_t initialStack;
#endif
        uint32_t alignmentPadding;
    };

    void alignFrameForICArguments(AfterICSaveLive& aic) PER_ARCH;
    void restoreFrameAlignmentForICArguments(AfterICSaveLive& aic) PER_ARCH;

    AfterICSaveLive icSaveLive(LiveRegisterSet& liveRegs);
    bool icBuildOOLFakeExitFrame(void* fakeReturnAddr, AfterICSaveLive& aic);
    void icRestoreLive(LiveRegisterSet& liveRegs, AfterICSaveLive& aic);

    // Align the stack pointer based on the number of arguments which are pushed
    // on the stack, such that the JitFrameLayout would be correctly aligned on
    // the JitStackAlignment.
    void alignJitStackBasedOnNArgs(Register nargs);
    void alignJitStackBasedOnNArgs(uint32_t nargs);

    void assertStackAlignment(uint32_t alignment, int32_t offset = 0) {
#ifdef DEBUG
        Label ok, bad;
        MOZ_ASSERT(IsPowerOfTwo(alignment));

        // Wrap around the offset to be a non-negative number.
        offset %= alignment;
        if (offset < 0)
            offset += alignment;

        // Test if each bit from offset is set.
        uint32_t off = offset;
        while (off) {
            uint32_t lowestBit = 1 << mozilla::CountTrailingZeroes32(off);
            branchTestStackPtr(Assembler::Zero, Imm32(lowestBit), &bad);
            off ^= lowestBit;
        }

        // Check that all remaining bits are zero.
        branchTestStackPtr(Assembler::Zero, Imm32((alignment - 1) ^ offset), &ok);

        bind(&bad);
        breakpoint();
        bind(&ok);
#endif
    }
};

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
