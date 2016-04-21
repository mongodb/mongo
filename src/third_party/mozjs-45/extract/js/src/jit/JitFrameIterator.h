/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitFrameIterator_h
#define jit_JitFrameIterator_h

#include "jsfun.h"
#include "jsscript.h"
#include "jstypes.h"

#include "jit/IonCode.h"
#include "jit/Snapshots.h"

#include "js/ProfilingFrameIterator.h"

namespace js {
    class ActivationIterator;
} // namespace js

namespace js {
namespace jit {

typedef void * CalleeToken;

enum FrameType
{
    // A JS frame is analagous to a js::InterpreterFrame, representing one scripted
    // functon activation. IonJS frames are used by the optimizing compiler.
    JitFrame_IonJS,

    // JS frame used by the baseline JIT.
    JitFrame_BaselineJS,

    // Frame pushed for JIT stubs that make non-tail calls, so that the
    // return address -> ICEntry mapping works.
    JitFrame_BaselineStub,
    JitFrame_IonStub,

    // The entry frame is the initial prologue block transitioning from the VM
    // into the Ion world.
    JitFrame_Entry,

    // A rectifier frame sits in between two JS frames, adapting argc != nargs
    // mismatches in calls.
    JitFrame_Rectifier,

    // Ion IC calling a scripted getter/setter.
    JitFrame_IonAccessorIC,

    // An unwound JS frame is a JS frame signalling that its callee frame has been
    // turned into an exit frame (see EnsureExitFrame). Used by Ion bailouts and
    // Baseline exception unwinding.
    JitFrame_Unwound_BaselineJS,
    JitFrame_Unwound_IonJS,
    JitFrame_Unwound_BaselineStub,
    JitFrame_Unwound_IonStub,
    JitFrame_Unwound_Rectifier,
    JitFrame_Unwound_IonAccessorIC,

    // An exit frame is necessary for transitioning from a JS frame into C++.
    // From within C++, an exit frame is always the last frame in any
    // JitActivation.
    JitFrame_Exit,

    // A bailout frame is a special IonJS jit frame after a bailout, and before
    // the reconstruction of the BaselineJS frame. From within C++, a bailout
    // frame is always the last frame in a JitActivation iff the bailout frame
    // information is recorded on the JitActivation.
    JitFrame_Bailout,

    // A lazy link frame is a special exit frame where a IonJS frame is reused
    // for linking the newly compiled code.  A special frame is needed to
    // work-around the fact that we can make stack patterns which are similar to
    // unwound frames. As opposed to unwound frames, we still have to mark all
    // the arguments of the original IonJS frame.
    JitFrame_LazyLink
};

enum ReadFrameArgsBehavior {
    // Only read formals (i.e. [0 ... callee()->nargs]
    ReadFrame_Formals,

    // Only read overflown args (i.e. [callee()->nargs ... numActuals()]
    ReadFrame_Overflown,

    // Read all args (i.e. [0 ... numActuals()])
    ReadFrame_Actuals
};

class CommonFrameLayout;
class JitFrameLayout;
class ExitFrameLayout;

class BaselineFrame;

class JitActivation;

// Iterate over the JIT stack to assert that all invariants are respected.
//  - Check that all entry frames are aligned on JitStackAlignment.
//  - Check that all rectifier frames keep the JitStackAlignment.
void AssertJitStackInvariants(JSContext* cx);

class JitFrameIterator
{
  protected:
    uint8_t* current_;
    FrameType type_;
    uint8_t* returnAddressToFp_;
    size_t frameSize_;

  private:
    mutable const SafepointIndex* cachedSafepointIndex_;
    const JitActivation* activation_;

    void dumpBaseline() const;

  public:
    explicit JitFrameIterator();
    explicit JitFrameIterator(JSContext* cx);
    explicit JitFrameIterator(const ActivationIterator& activations);

    // Current frame information.
    FrameType type() const {
        return type_;
    }
    uint8_t* fp() const {
        return current_;
    }
    const JitActivation* activation() const {
        return activation_;
    }

    CommonFrameLayout* current() const {
        return (CommonFrameLayout*)current_;
    }

    inline uint8_t* returnAddress() const;

    // Return the pointer of the JitFrame, the iterator is assumed to be settled
    // on a scripted frame.
    JitFrameLayout* jsFrame() const;

    // Returns true iff this exit frame was created using EnsureExitFrame.
    inline bool isFakeExitFrame() const;

    inline ExitFrameLayout* exitFrame() const;

    // Returns whether the JS frame has been invalidated and, if so,
    // places the invalidated Ion script in |ionScript|.
    bool checkInvalidation(IonScript** ionScript) const;
    bool checkInvalidation() const;

    bool isExitFrame() const {
        return type_ == JitFrame_Exit || type_ == JitFrame_LazyLink;
    }
    bool isScripted() const {
        return type_ == JitFrame_BaselineJS || type_ == JitFrame_IonJS || type_ == JitFrame_Bailout;
    }
    bool isBaselineJS() const {
        return type_ == JitFrame_BaselineJS;
    }
    bool isIonScripted() const {
        return type_ == JitFrame_IonJS || type_ == JitFrame_Bailout;
    }
    bool isIonJS() const {
        return type_ == JitFrame_IonJS;
    }
    bool isIonStub() const {
        return type_ == JitFrame_IonStub;
    }
    bool isIonStubMaybeUnwound() const {
        return type_ == JitFrame_IonStub || type_ == JitFrame_Unwound_IonStub;
    }
    bool isIonAccessorICMaybeUnwound() const {
        return type_ == JitFrame_IonAccessorIC || type_ == JitFrame_Unwound_IonAccessorIC;
    }
    bool isBailoutJS() const {
        return type_ == JitFrame_Bailout;
    }
    bool isBaselineStub() const {
        return type_ == JitFrame_BaselineStub;
    }
    bool isBaselineStubMaybeUnwound() const {
        return type_ == JitFrame_BaselineStub || type_ == JitFrame_Unwound_BaselineStub;
    }
    bool isRectifierMaybeUnwound() const {
        return type_ == JitFrame_Rectifier || type_ == JitFrame_Unwound_Rectifier;
    }
    bool isBareExit() const;
    template <typename T> bool isExitFrameLayout() const;

    bool isEntry() const {
        return type_ == JitFrame_Entry;
    }
    bool isFunctionFrame() const;

    bool isConstructing() const;

    void* calleeToken() const;
    JSFunction* callee() const;
    JSFunction* maybeCallee() const;
    unsigned numActualArgs() const;
    JSScript* script() const;
    void baselineScriptAndPc(JSScript** scriptRes, jsbytecode** pcRes) const;
    Value* actualArgs() const;

    // Returns the return address of the frame above this one (that is, the
    // return address that returns back to the current frame).
    uint8_t* returnAddressToFp() const {
        return returnAddressToFp_;
    }

    // Previous frame information extracted from the current frame.
    inline size_t prevFrameLocalSize() const;
    inline FrameType prevType() const;
    uint8_t* prevFp() const;

    // Returns the stack space used by the current frame, in bytes. This does
    // not include the size of its fixed header.
    size_t frameSize() const {
        MOZ_ASSERT(!isExitFrame());
        return frameSize_;
    }

    // Functions used to iterate on frames. When prevType is JitFrame_Entry,
    // the current frame is the last frame.
    inline bool done() const {
        return type_ == JitFrame_Entry;
    }
    JitFrameIterator& operator++();

    // Returns the IonScript associated with this JS frame.
    IonScript* ionScript() const;

    // Returns the IonScript associated with this JS frame; the frame must
    // not be invalidated.
    IonScript* ionScriptFromCalleeToken() const;

    // Returns the Safepoint associated with this JS frame. Incurs a lookup
    // overhead.
    const SafepointIndex* safepoint() const;

    // Returns the OSI index associated with this JS frame. Incurs a lookup
    // overhead.
    const OsiIndex* osiIndex() const;

    // Returns the Snapshot offset associated with this JS frame. Incurs a
    // lookup overhead.
    SnapshotOffset snapshotOffset() const;

    uintptr_t* spillBase() const;
    MachineState machineState() const;

    template <class Op>
    void unaliasedForEachActual(Op op, ReadFrameArgsBehavior behavior) const {
        MOZ_ASSERT(isBaselineJS());

        unsigned nactual = numActualArgs();
        unsigned start, end;
        switch (behavior) {
          case ReadFrame_Formals:
            start = 0;
            end = callee()->nargs();
            break;
          case ReadFrame_Overflown:
            start = callee()->nargs();
            end = nactual;
            break;
          case ReadFrame_Actuals:
            start = 0;
            end = nactual;
        }

        Value* argv = actualArgs();
        for (unsigned i = start; i < end; i++)
            op(argv[i]);
    }

    void dump() const;

    inline BaselineFrame* baselineFrame() const;

#ifdef DEBUG
    bool verifyReturnAddressUsingNativeToBytecodeMap();
#else
    inline bool verifyReturnAddressUsingNativeToBytecodeMap() { return true; }
#endif
};

class JitcodeGlobalTable;

class JitProfilingFrameIterator
{
    uint8_t* fp_;
    FrameType type_;
    void* returnAddressToFp_;

    inline JitFrameLayout* framePtr();
    inline JSScript* frameScript();
    bool tryInitWithPC(void* pc);
    bool tryInitWithTable(JitcodeGlobalTable* table, void* pc, JSRuntime* rt,
                          bool forLastCallSite);
    void fixBaselineDebugModeOSRReturnAddress();

  public:
    JitProfilingFrameIterator(JSRuntime* rt,
                              const JS::ProfilingFrameIterator::RegisterState& state);
    explicit JitProfilingFrameIterator(void* exitFrame);

    void operator++();
    bool done() const { return fp_ == nullptr; }

    void* fp() const { MOZ_ASSERT(!done()); return fp_; }
    void* stackAddress() const { return fp(); }
    FrameType frameType() const { MOZ_ASSERT(!done()); return type_; }
    void* returnAddressToFp() const { MOZ_ASSERT(!done()); return returnAddressToFp_; }
};

class RInstructionResults
{
    // Vector of results of recover instructions.
    typedef mozilla::Vector<RelocatableValue, 1, SystemAllocPolicy> Values;
    mozilla::UniquePtr<Values, JS::DeletePolicy<Values> > results_;

    // The frame pointer is used as a key to check if the current frame already
    // bailed out.
    JitFrameLayout* fp_;

    // Record if we tried and succeed at allocating and filling the vector of
    // recover instruction results, if needed.  This flag is needed in order to
    // avoid evaluating the recover instruction twice.
    bool initialized_;

  public:
    explicit RInstructionResults(JitFrameLayout* fp);
    RInstructionResults(RInstructionResults&& src);

    RInstructionResults& operator=(RInstructionResults&& rhs);

    ~RInstructionResults();

    bool init(JSContext* cx, uint32_t numResults);
    bool isInitialized() const;
#ifdef DEBUG
    size_t length() const;
#endif

    JitFrameLayout* frame() const;

    RelocatableValue& operator[](size_t index);

    void trace(JSTracer* trc);
};

struct MaybeReadFallback
{
    enum NoGCValue {
        NoGC_UndefinedValue,
        NoGC_MagicOptimizedOut
    };

    enum FallbackConsequence {
        Fallback_Invalidate,
        Fallback_DoNothing
    };

    JSContext* maybeCx;
    JitActivation* activation;
    const JitFrameIterator* frame;
    const NoGCValue unreadablePlaceholder_;
    const FallbackConsequence consequence;

    explicit MaybeReadFallback(const Value& placeholder = UndefinedValue())
      : maybeCx(nullptr),
        activation(nullptr),
        frame(nullptr),
        unreadablePlaceholder_(noGCPlaceholder(placeholder)),
        consequence(Fallback_Invalidate)
    {
    }

    MaybeReadFallback(JSContext* cx, JitActivation* activation, const JitFrameIterator* frame,
                      FallbackConsequence consequence = Fallback_Invalidate)
      : maybeCx(cx),
        activation(activation),
        frame(frame),
        unreadablePlaceholder_(NoGC_UndefinedValue),
        consequence(consequence)
    {
    }

    bool canRecoverResults() { return maybeCx; }

    Value unreadablePlaceholder() const {
        if (unreadablePlaceholder_ == NoGC_MagicOptimizedOut)
            return MagicValue(JS_OPTIMIZED_OUT);
        return UndefinedValue();
    }

    NoGCValue noGCPlaceholder(Value v) const {
        if (v.isMagic(JS_OPTIMIZED_OUT))
            return NoGC_MagicOptimizedOut;
        return NoGC_UndefinedValue;
    }
};


class RResumePoint;
class RSimdBox;

// Reads frame information in snapshot-encoding order (that is, outermost frame
// to innermost frame).
class SnapshotIterator
{
  protected:
    SnapshotReader snapshot_;
    RecoverReader recover_;
    JitFrameLayout* fp_;
    const MachineState* machine_;
    IonScript* ionScript_;
    RInstructionResults* instructionResults_;

    enum ReadMethod {
        // Read the normal value.
        RM_Normal          = 1 << 0,

        // Read the default value, or the normal value if there is no default.
        RM_AlwaysDefault   = 1 << 1,

        // Try to read the normal value if it is readable, otherwise default to
        // the Default value.
        RM_NormalOrDefault = RM_Normal | RM_AlwaysDefault,
    };

  private:
    // Read a spilled register from the machine state.
    bool hasRegister(Register reg) const {
        return machine_->has(reg);
    }
    uintptr_t fromRegister(Register reg) const {
        return machine_->read(reg);
    }

    bool hasRegister(FloatRegister reg) const {
        return machine_->has(reg);
    }
    double fromRegister(FloatRegister reg) const {
        return machine_->read(reg);
    }

    // Read an uintptr_t from the stack.
    bool hasStack(int32_t offset) const {
        return true;
    }
    uintptr_t fromStack(int32_t offset) const;

    bool hasInstructionResult(uint32_t index) const {
        return instructionResults_;
    }
    bool hasInstructionResults() const {
        return instructionResults_;
    }
    Value fromInstructionResult(uint32_t index) const;

    Value allocationValue(const RValueAllocation& a, ReadMethod rm = RM_Normal);
    bool allocationReadable(const RValueAllocation& a, ReadMethod rm = RM_Normal);
    void writeAllocationValuePayload(const RValueAllocation& a, Value v);
    void warnUnreadableAllocation();

  private:
    friend class RSimdBox;
    const FloatRegisters::RegisterContent* floatAllocationPointer(const RValueAllocation& a) const;

  public:
    // Handle iterating over RValueAllocations of the snapshots.
    inline RValueAllocation readAllocation() {
        MOZ_ASSERT(moreAllocations());
        return snapshot_.readAllocation();
    }
    Value skip() {
        snapshot_.skipAllocation();
        return UndefinedValue();
    }

    const RResumePoint* resumePoint() const;
    const RInstruction* instruction() const {
        return recover_.instruction();
    }

    uint32_t numAllocations() const;
    inline bool moreAllocations() const {
        return snapshot_.numAllocationsRead() < numAllocations();
    }

    int32_t readOuterNumActualArgs() const;

    // Used by recover instruction to store the value back into the instruction
    // results array.
    void storeInstructionResult(Value v);

  public:
    // Exhibits frame properties contained in the snapshot.
    uint32_t pcOffset() const;
    inline bool resumeAfter() const {
        // Inline frames are inlined on calls, which are considered as being
        // resumed on the Call as baseline will push the pc once we return from
        // the call.
        if (moreFrames())
            return false;
        return recover_.resumeAfter();
    }
    inline BailoutKind bailoutKind() const {
        return snapshot_.bailoutKind();
    }

  public:
    // Read the next instruction available and get ready to either skip it or
    // evaluate it.
    inline void nextInstruction() {
        MOZ_ASSERT(snapshot_.numAllocationsRead() == numAllocations());
        recover_.nextInstruction();
        snapshot_.resetNumAllocationsRead();
    }

    // Skip an Instruction by walking to the next instruction and by skipping
    // all the allocations corresponding to this instruction.
    void skipInstruction();

    inline bool moreInstructions() const {
        return recover_.moreInstructions();
    }

  protected:
    // Register a vector used for storing the results of the evaluation of
    // recover instructions. This vector should be registered before the
    // beginning of the iteration. This function is in charge of allocating
    // enough space for all instructions results, and return false iff it fails.
    bool initInstructionResults(MaybeReadFallback& fallback);

    // This function is used internally for computing the result of the recover
    // instructions.
    bool computeInstructionResults(JSContext* cx, RInstructionResults* results) const;

  public:
    // Handle iterating over frames of the snapshots.
    void nextFrame();
    void settleOnFrame();

    inline bool moreFrames() const {
        // The last instruction is recovering the innermost frame, so as long as
        // there is more instruction there is necesseray more frames.
        return moreInstructions();
    }

  public:
    // Connect all informations about the current script in order to recover the
    // content of baseline frames.

    SnapshotIterator(const JitFrameIterator& iter, const MachineState* machineState);
    SnapshotIterator();

    Value read() {
        return allocationValue(readAllocation());
    }

    // Read the |Normal| value unless it is not available and that the snapshot
    // provides a |Default| value. This is useful to avoid invalidations of the
    // frame while we are only interested in a few properties which are provided
    // by the |Default| value.
    Value readWithDefault(RValueAllocation* alloc) {
        *alloc = RValueAllocation();
        RValueAllocation a = readAllocation();
        if (allocationReadable(a))
            return allocationValue(a);

        *alloc = a;
        return allocationValue(a, RM_AlwaysDefault);
    }

    Value maybeRead(const RValueAllocation& a, MaybeReadFallback& fallback);
    Value maybeRead(MaybeReadFallback& fallback) {
        RValueAllocation a = readAllocation();
        return maybeRead(a, fallback);
    }

    void traceAllocation(JSTracer* trc);

    template <class Op>
    void readFunctionFrameArgs(Op& op, ArgumentsObject** argsObj, Value* thisv,
                               unsigned start, unsigned end, JSScript* script,
                               MaybeReadFallback& fallback)
    {
        // Assumes that the common frame arguments have already been read.
        if (script->argumentsHasVarBinding()) {
            if (argsObj) {
                Value v = read();
                if (v.isObject())
                    *argsObj = &v.toObject().as<ArgumentsObject>();
            } else {
                skip();
            }
        }

        if (thisv)
            *thisv = maybeRead(fallback);
        else
            skip();

        unsigned i = 0;
        if (end < start)
            i = start;

        for (; i < start; i++)
            skip();
        for (; i < end; i++) {
            // We are not always able to read values from the snapshots, some values
            // such as non-gc things may still be live in registers and cause an
            // error while reading the machine state.
            Value v = maybeRead(fallback);
            op(v);
        }
    }

    // Iterate over all the allocations and return only the value of the
    // allocation located at one index.
    Value maybeReadAllocByIndex(size_t index);

#ifdef TRACK_SNAPSHOTS
    void spewBailingFrom() const {
        snapshot_.spewBailingFrom();
    }
#endif
};

// Reads frame information in callstack order (that is, innermost frame to
// outermost frame).
class InlineFrameIterator
{
    const JitFrameIterator* frame_;
    SnapshotIterator start_;
    SnapshotIterator si_;
    uint32_t framesRead_;

    // When the inline-frame-iterator is created, this variable is defined to
    // UINT32_MAX. Then the first iteration of findNextFrame, which settle on
    // the innermost frame, is used to update this counter to the number of
    // frames contained in the recover buffer.
    uint32_t frameCount_;

    // The |calleeTemplate_| fields contains either the JSFunction or the
    // template from which it is supposed to be cloned. The |calleeRVA_| is an
    // Invalid value allocation, if the |calleeTemplate_| field is the effective
    // JSFunction, and not its template. On the other hand, any other value
    // allocation implies that the |calleeTemplate_| is the template JSFunction
    // from which the effective one would be derived and cached by the Recover
    // instruction result.
    RootedFunction calleeTemplate_;
    RValueAllocation calleeRVA_;

    RootedScript script_;
    jsbytecode* pc_;
    uint32_t numActualArgs_;

    // Register state, used by all snapshot iterators.
    MachineState machine_;

    struct Nop {
        void operator()(const Value& v) { }
    };

  private:
    void findNextFrame();
    JSObject* computeScopeChain(Value scopeChainValue, MaybeReadFallback& fallback,
                                bool* hasCallObj = nullptr) const;

  public:
    InlineFrameIterator(JSContext* cx, const JitFrameIterator* iter);
    InlineFrameIterator(JSRuntime* rt, const JitFrameIterator* iter);
    InlineFrameIterator(JSContext* cx, const InlineFrameIterator* iter);

    bool more() const {
        return frame_ && framesRead_ < frameCount_;
    }

    // Due to optimizations, we are not always capable of reading the callee of
    // inlined frames without invalidating the IonCode. This function might
    // return either the effective callee of the JSFunction which might be used
    // to create it.
    //
    // As such, the |calleeTemplate()| can be used to read most of the metadata
    // which are conserved across clones.
    JSFunction* calleeTemplate() const {
        MOZ_ASSERT(isFunctionFrame());
        return calleeTemplate_;
    }
    JSFunction* maybeCalleeTemplate() const {
        return calleeTemplate_;
    }

    JSFunction* callee(MaybeReadFallback& fallback) const;

    unsigned numActualArgs() const {
        // The number of actual arguments of inline frames is recovered by the
        // iteration process. It is recovered from the bytecode because this
        // property still hold since the for inlined frames. This property does not
        // hold for the parent frame because it can have optimize a call to
        // js_fun_call or js_fun_apply.
        if (more())
            return numActualArgs_;

        return frame_->numActualArgs();
    }

    template <class ArgOp, class LocalOp>
    void readFrameArgsAndLocals(JSContext* cx, ArgOp& argOp, LocalOp& localOp,
                                JSObject** scopeChain, bool* hasCallObj,
                                Value* rval, ArgumentsObject** argsObj, Value* thisv,
                                ReadFrameArgsBehavior behavior,
                                MaybeReadFallback& fallback) const
    {
        SnapshotIterator s(si_);

        // Read the scope chain.
        if (scopeChain) {
            Value scopeChainValue = s.maybeRead(fallback);
            *scopeChain = computeScopeChain(scopeChainValue, fallback, hasCallObj);
        } else {
            s.skip();
        }

        // Read return value.
        if (rval)
            *rval = s.read();
        else
            s.skip();

        // Read arguments, which only function frames have.
        if (isFunctionFrame()) {
            unsigned nactual = numActualArgs();
            unsigned nformal = calleeTemplate()->nargs();

            // Get the non overflown arguments, which are taken from the inlined
            // frame, because it will have the updated value when JSOP_SETARG is
            // done.
            if (behavior != ReadFrame_Overflown)
                s.readFunctionFrameArgs(argOp, argsObj, thisv, 0, nformal, script(), fallback);

            if (behavior != ReadFrame_Formals) {
                if (more()) {
                    // There is still a parent frame of this inlined frame.  All
                    // arguments (also the overflown) are the last pushed values
                    // in the parent frame.  To get the overflown arguments, we
                    // need to take them from there.

                    // The overflown arguments are not available in current frame.
                    // They are the last pushed arguments in the parent frame of
                    // this inlined frame.
                    InlineFrameIterator it(cx, this);
                    ++it;
                    unsigned argsObjAdj = it.script()->argumentsHasVarBinding() ? 1 : 0;
                    bool hasNewTarget = isConstructing();
                    SnapshotIterator parent_s(it.snapshotIterator());

                    // Skip over all slots until we get to the last slots
                    // (= arguments slots of callee) the +3 is for [this], [returnvalue],
                    // [scopechain], and maybe +1 for [argsObj]
                    MOZ_ASSERT(parent_s.numAllocations() >= nactual + 3 + argsObjAdj + hasNewTarget);
                    unsigned skip = parent_s.numAllocations() - nactual - 3 - argsObjAdj - hasNewTarget;
                    for (unsigned j = 0; j < skip; j++)
                        parent_s.skip();

                    // Get the overflown arguments
                    MaybeReadFallback unusedFallback;
                    parent_s.skip(); // scope chain
                    parent_s.skip(); // return value
                    parent_s.readFunctionFrameArgs(argOp, nullptr, nullptr,
                                                   nformal, nactual + isConstructing(), it.script(),
                                                   fallback);
                } else {
                    // There is no parent frame to this inlined frame, we can read
                    // from the frame's Value vector directly.
                    Value* argv = frame_->actualArgs();
                    for (unsigned i = nformal; i < nactual + isConstructing(); i++)
                        argOp(argv[i]);
                }
            }
        }

        // At this point we've read all the formals in s, and can read the
        // locals.
        for (unsigned i = 0; i < script()->nfixed(); i++)
            localOp(s.maybeRead(fallback));
    }

    template <class Op>
    void unaliasedForEachActual(JSContext* cx, Op op,
                                ReadFrameArgsBehavior behavior,
                                MaybeReadFallback& fallback) const
    {
        Nop nop;
        readFrameArgsAndLocals(cx, op, nop, nullptr, nullptr, nullptr,
                               nullptr, nullptr, behavior, fallback);
    }

    JSScript* script() const {
        return script_;
    }
    jsbytecode* pc() const {
        return pc_;
    }
    SnapshotIterator snapshotIterator() const {
        return si_;
    }
    bool isFunctionFrame() const;
    bool isConstructing() const;

    JSObject* scopeChain(MaybeReadFallback& fallback) const {
        SnapshotIterator s(si_);

        // scopeChain
        Value v = s.maybeRead(fallback);
        return computeScopeChain(v, fallback);
    }

    Value thisArgument(MaybeReadFallback& fallback) const {
        SnapshotIterator s(si_);

        // scopeChain
        s.skip();

        // return value
        s.skip();

        // Arguments object.
        if (script()->argumentsHasVarBinding())
            s.skip();

        return s.maybeRead(fallback);
    }

    InlineFrameIterator& operator++() {
        findNextFrame();
        return *this;
    }

    void dump() const;

    void resetOn(const JitFrameIterator* iter);

    const JitFrameIterator& frame() const {
        return *frame_;
    }

    // Inline frame number, 0 for the outermost (non-inlined) frame.
    size_t frameNo() const {
        return frameCount() - framesRead_;
    }
    size_t frameCount() const {
        MOZ_ASSERT(frameCount_ != UINT32_MAX);
        return frameCount_;
    }

  private:
    InlineFrameIterator() = delete;
    InlineFrameIterator(const InlineFrameIterator& iter) = delete;
};

} // namespace jit
} // namespace js

#endif /* jit_JitFrameIterator_h */
