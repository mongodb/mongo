/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Everything needed to build actual MIR instructions: the actual opcodes and
 * instructions, the instruction interface, and use chains.
 */

#ifndef jit_MIR_h
#define jit_MIR_h

#include "mozilla/Array.h"
#include "mozilla/DebugOnly.h"

#include "builtin/SIMD.h"
#include "jit/AtomicOp.h"
#include "jit/BaselineIC.h"
#include "jit/FixedList.h"
#include "jit/InlineList.h"
#include "jit/JitAllocPolicy.h"
#include "jit/MacroAssembler.h"
#include "jit/MOpcodes.h"
#include "jit/TypedObjectPrediction.h"
#include "jit/TypePolicy.h"
#include "vm/ArrayObject.h"
#include "vm/ScopeObject.h"
#include "vm/SharedMem.h"
#include "vm/TypedArrayCommon.h"
#include "vm/UnboxedObject.h"

// Undo windows.h damage on Win64
#undef MemoryBarrier

namespace js {

class StringObject;

namespace jit {

class BaselineInspector;
class Range;

static inline
MIRType MIRTypeFromValue(const js::Value& vp)
{
    if (vp.isDouble())
        return MIRType_Double;
    if (vp.isMagic()) {
        switch (vp.whyMagic()) {
          case JS_OPTIMIZED_ARGUMENTS:
            return MIRType_MagicOptimizedArguments;
          case JS_OPTIMIZED_OUT:
            return MIRType_MagicOptimizedOut;
          case JS_ELEMENTS_HOLE:
            return MIRType_MagicHole;
          case JS_IS_CONSTRUCTING:
            return MIRType_MagicIsConstructing;
          case JS_UNINITIALIZED_LEXICAL:
            return MIRType_MagicUninitializedLexical;
          default:
            MOZ_ASSERT(!"Unexpected magic constant");
        }
    }
    return MIRTypeFromValueType(vp.extractNonDoubleType());
}

#define MIR_FLAG_LIST(_)                                                        \
    _(InWorklist)                                                               \
    _(EmittedAtUses)                                                            \
    _(Commutative)                                                              \
    _(Movable)       /* Allow passes like LICM to move this instruction */      \
    _(Lowered)       /* (Debug only) has a virtual register */                  \
    _(Guard)         /* Not removable if uses == 0 */                           \
                                                                                \
    /* Flag an instruction to be considered as a Guard if the instructions
     * bails out on some inputs.
     *
     * Some optimizations can replace an instruction, and leave its operands
     * unused. When the type information of the operand got used as a
     * predicate of the transformation, then we have to flag the operands as
     * GuardRangeBailouts.
     *
     * This flag prevents further optimization of instructions, which
     * might remove the run-time checks (bailout conditions) used as a
     * predicate of the previous transformation.
     */                                                                         \
    _(GuardRangeBailouts)                                                       \
                                                                                \
    /* Keep the flagged instruction in resume points and do not substitute this
     * instruction by an UndefinedValue. This might be used by call inlining
     * when a function argument is not used by the inlined instructions.
     */                                                                         \
    _(ImplicitlyUsed)                                                           \
                                                                                \
    /* The instruction has been marked dead for lazy removal from resume
     * points.
     */                                                                         \
    _(Unused)                                                                   \
                                                                                \
    /* When a branch is removed, the uses of multiple instructions are removed.
     * The removal of branches is based on hypotheses.  These hypotheses might
     * fail, in which case we need to bailout from the current code.
     *
     * When we implement a destructive optimization, we need to consider the
     * failing cases, and consider the fact that we might resume the execution
     * into a branch which was removed from the compiler.  As such, a
     * destructive optimization need to take into acount removed branches.
     *
     * In order to let destructive optimizations know about removed branches, we
     * have to annotate instructions with the UseRemoved flag.  This flag
     * annotates instruction which were used in removed branches.
     */                                                                         \
    _(UseRemoved)                                                               \
                                                                                \
    /* Marks if the current instruction should go to the bailout paths instead
     * of producing code as part of the control flow.  This flag can only be set
     * on instructions which are only used by ResumePoint or by other flagged
     * instructions.
     */                                                                         \
    _(RecoveredOnBailout)                                                       \
                                                                                \
    /* Some instructions might represent an object, but the memory of these
     * objects might be incomplete if we have not recovered all the stores which
     * were supposed to happen before. This flag is used to annotate
     * instructions which might return a pointer to a memory area which is not
     * yet fully initialized. This flag is used to ensure that stores are
     * executed before returning the value.
     */                                                                         \
    _(IncompleteObject)                                                         \
                                                                                \
    /* The current instruction got discarded from the MIR Graph. This is useful
     * when we want to iterate over resume points and instructions, while
     * handling instructions which are discarded without reporting to the
     * iterator.
     */                                                                         \
    _(Discarded)

class MDefinition;
class MInstruction;
class MBasicBlock;
class MNode;
class MUse;
class MPhi;
class MIRGraph;
class MResumePoint;
class MControlInstruction;

// Represents a use of a node.
class MUse : public TempObject, public InlineListNode<MUse>
{
    // Grant access to setProducerUnchecked.
    friend class MDefinition;
    friend class MPhi;

    MDefinition* producer_; // MDefinition that is being used.
    MNode* consumer_;       // The node that is using this operand.

    // Low-level unchecked edit method for replaceAllUsesWith and
    // MPhi::removeOperand. This doesn't update use lists!
    // replaceAllUsesWith and MPhi::removeOperand do that manually.
    void setProducerUnchecked(MDefinition* producer) {
        MOZ_ASSERT(consumer_);
        MOZ_ASSERT(producer_);
        MOZ_ASSERT(producer);
        producer_ = producer;
    }

  public:
    // Default constructor for use in vectors.
    MUse()
      : producer_(nullptr), consumer_(nullptr)
    { }

    // Move constructor for use in vectors. When an MUse is moved, it stays
    // in its containing use list.
    MUse(MUse&& other)
      : InlineListNode<MUse>(mozilla::Move(other)),
        producer_(other.producer_), consumer_(other.consumer_)
    { }

    // Construct an MUse initialized with |producer| and |consumer|.
    MUse(MDefinition* producer, MNode* consumer)
    {
        initUnchecked(producer, consumer);
    }

    // Set this use, which was previously clear.
    inline void init(MDefinition* producer, MNode* consumer);
    // Like init, but works even when the use contains uninitialized data.
    inline void initUnchecked(MDefinition* producer, MNode* consumer);
    // Like initUnchecked, but set the producer to nullptr.
    inline void initUncheckedWithoutProducer(MNode* consumer);
    // Set this use, which was not previously clear.
    inline void replaceProducer(MDefinition* producer);
    // Clear this use.
    inline void releaseProducer();

    MDefinition* producer() const {
        MOZ_ASSERT(producer_ != nullptr);
        return producer_;
    }
    bool hasProducer() const {
        return producer_ != nullptr;
    }
    MNode* consumer() const {
        MOZ_ASSERT(consumer_ != nullptr);
        return consumer_;
    }

#ifdef DEBUG
    // Return the operand index of this MUse in its consumer. This is DEBUG-only
    // as normal code should instead to call indexOf on the casted consumer
    // directly, to allow it to be devirtualized and inlined.
    size_t index() const;
#endif
};

typedef InlineList<MUse>::iterator MUseIterator;

// A node is an entry in the MIR graph. It has two kinds:
//   MInstruction: an instruction which appears in the IR stream.
//   MResumePoint: a list of instructions that correspond to the state of the
//                 interpreter/Baseline stack.
//
// Nodes can hold references to MDefinitions. Each MDefinition has a list of
// nodes holding such a reference (its use chain).
class MNode : public TempObject
{
  protected:
    MBasicBlock* block_;    // Containing basic block.

  public:
    enum Kind {
        Definition,
        ResumePoint
    };

    MNode()
      : block_(nullptr)
    { }

    explicit MNode(MBasicBlock* block)
      : block_(block)
    { }

    virtual Kind kind() const = 0;

    // Returns the definition at a given operand.
    virtual MDefinition* getOperand(size_t index) const = 0;
    virtual size_t numOperands() const = 0;
    virtual size_t indexOf(const MUse* u) const = 0;

    bool isDefinition() const {
        return kind() == Definition;
    }
    bool isResumePoint() const {
        return kind() == ResumePoint;
    }
    MBasicBlock* block() const {
        return block_;
    }
    MBasicBlock* caller() const;

    // Sets an already set operand, updating use information. If you're looking
    // for setOperand, this is probably what you want.
    virtual void replaceOperand(size_t index, MDefinition* operand) = 0;

    // Resets the operand to an uninitialized state, breaking the link
    // with the previous operand's producer.
    void releaseOperand(size_t index) {
        getUseFor(index)->releaseProducer();
    }
    bool hasOperand(size_t index) const {
        return getUseFor(index)->hasProducer();
    }

    inline MDefinition* toDefinition();
    inline MResumePoint* toResumePoint();

    virtual bool writeRecoverData(CompactBufferWriter& writer) const;

    virtual void dump(GenericPrinter& out) const = 0;
    virtual void dump() const = 0;

  protected:
    // Need visibility on getUseFor to avoid O(n^2) complexity.
    friend void AssertBasicGraphCoherency(MIRGraph& graph);

    // Gets the MUse corresponding to given operand.
    virtual MUse* getUseFor(size_t index) = 0;
    virtual const MUse* getUseFor(size_t index) const = 0;
};

class AliasSet {
  private:
    uint32_t flags_;

  public:
    enum Flag {
        None_             = 0,
        ObjectFields      = 1 << 0, // shape, class, slots, length etc.
        Element           = 1 << 1, // A Value member of obj->elements or
                                    // a typed object.
        UnboxedElement    = 1 << 2, // An unboxed scalar or reference member of
                                    // a typed array, typed object, or unboxed
                                    // object.
        DynamicSlot       = 1 << 3, // A Value member of obj->slots.
        FixedSlot         = 1 << 4, // A Value member of obj->fixedSlots().
        DOMProperty       = 1 << 5, // A DOM property
        FrameArgument     = 1 << 6, // An argument kept on the stack frame
        AsmJSGlobalVar    = 1 << 7, // An asm.js global var
        AsmJSHeap         = 1 << 8, // An asm.js heap load
        TypedArrayLength  = 1 << 9,// A typed array's length
        Last              = TypedArrayLength,
        Any               = Last | (Last - 1),

        NumCategories     = 10,

        // Indicates load or store.
        Store_            = 1 << 31
    };

    static_assert((1 << NumCategories) - 1 == Any,
                  "NumCategories must include all flags present in Any");

    explicit AliasSet(uint32_t flags)
      : flags_(flags)
    {
    }

  public:
    inline bool isNone() const {
        return flags_ == None_;
    }
    uint32_t flags() const {
        return flags_ & Any;
    }
    inline bool isStore() const {
        return !!(flags_ & Store_);
    }
    inline bool isLoad() const {
        return !isStore() && !isNone();
    }
    inline AliasSet operator |(const AliasSet& other) const {
        return AliasSet(flags_ | other.flags_);
    }
    inline AliasSet operator&(const AliasSet& other) const {
        return AliasSet(flags_ & other.flags_);
    }
    static AliasSet None() {
        return AliasSet(None_);
    }
    static AliasSet Load(uint32_t flags) {
        MOZ_ASSERT(flags && !(flags & Store_));
        return AliasSet(flags);
    }
    static AliasSet Store(uint32_t flags) {
        MOZ_ASSERT(flags && !(flags & Store_));
        return AliasSet(flags | Store_);
    }
    static uint32_t BoxedOrUnboxedElements(JSValueType type) {
        return (type == JSVAL_TYPE_MAGIC) ? Element : UnboxedElement;
    }
};

// An MDefinition is an SSA name.
class MDefinition : public MNode
{
    friend class MBasicBlock;

  public:
    enum Opcode {
#   define DEFINE_OPCODES(op) Op_##op,
        MIR_OPCODE_LIST(DEFINE_OPCODES)
#   undef DEFINE_OPCODES
        Op_Invalid
    };

  private:
    InlineList<MUse> uses_;        // Use chain.
    uint32_t id_;                  // Instruction ID, which after block re-ordering
                                   // is sorted within a basic block.
    uint32_t flags_;               // Bit flags.
    Range* range_;                 // Any computed range for this def.
    MIRType resultType_;           // Representation of result type.
    TemporaryTypeSet* resultTypeSet_; // Optional refinement of the result type.
    union {
        MInstruction* dependency_; // Implicit dependency (store, call, etc.) of this instruction.
                                   // Used by alias analysis, GVN and LICM.
        uint32_t virtualRegister_; // Used by lowering to map definitions to virtual registers.
    };

    // Track bailouts by storing the current pc in MIR instruction. Also used
    // for profiling and keeping track of what the last known pc was.
    const BytecodeSite* trackedSite_;

  private:
    enum Flag {
        None = 0,
#   define DEFINE_FLAG(flag) flag,
        MIR_FLAG_LIST(DEFINE_FLAG)
#   undef DEFINE_FLAG
        Total
    };

    bool hasFlags(uint32_t flags) const {
        return (flags_ & flags) == flags;
    }
    void removeFlags(uint32_t flags) {
        flags_ &= ~flags;
    }
    void setFlags(uint32_t flags) {
        flags_ |= flags;
    }

  protected:
    virtual void setBlock(MBasicBlock* block) {
        block_ = block;
    }

    static HashNumber addU32ToHash(HashNumber hash, uint32_t data);

  public:
    MDefinition()
      : id_(0),
        flags_(0),
        range_(nullptr),
        resultType_(MIRType_None),
        resultTypeSet_(nullptr),
        dependency_(nullptr),
        trackedSite_(nullptr)
    { }

    // Copying a definition leaves the list of uses and the block empty.
    explicit MDefinition(const MDefinition& other)
      : id_(0),
        flags_(other.flags_),
        range_(other.range_),
        resultType_(other.resultType_),
        resultTypeSet_(other.resultTypeSet_),
        dependency_(other.dependency_),
        trackedSite_(other.trackedSite_)
    { }

    virtual Opcode op() const = 0;
    virtual const char* opName() const = 0;
    virtual void accept(MDefinitionVisitor* visitor) = 0;

    void printName(GenericPrinter& out) const;
    static void PrintOpcodeName(GenericPrinter& out, Opcode op);
    virtual void printOpcode(GenericPrinter& out) const;
    void dump(GenericPrinter& out) const override;
    void dump() const override;
    void dumpLocation(GenericPrinter& out) const;
    void dumpLocation() const;

    // For LICM.
    virtual bool neverHoist() const { return false; }

    // Also for LICM. Test whether this definition is likely to be a call, which
    // would clobber all or many of the floating-point registers, such that
    // hoisting floating-point constants out of containing loops isn't likely to
    // be worthwhile.
    virtual bool possiblyCalls() const { return false; }

    void setTrackedSite(const BytecodeSite* site) {
        MOZ_ASSERT(site);
        trackedSite_ = site;
    }
    const BytecodeSite* trackedSite() const {
        return trackedSite_;
    }
    jsbytecode* trackedPc() const {
        return trackedSite_ ? trackedSite_->pc() : nullptr;
    }
    InlineScriptTree* trackedTree() const {
        return trackedSite_ ? trackedSite_->tree() : nullptr;
    }
    TrackedOptimizations* trackedOptimizations() const {
        return trackedSite_ && trackedSite_->hasOptimizations()
               ? trackedSite_->optimizations()
               : nullptr;
    }

    JSScript* profilerLeaveScript() const {
        return trackedTree()->outermostCaller()->script();
    }

    jsbytecode* profilerLeavePc() const {
        // If this is in a top-level function, use the pc directly.
        if (trackedTree()->isOutermostCaller())
            return trackedPc();

        // Walk up the InlineScriptTree chain to find the top-most callPC
        InlineScriptTree* curTree = trackedTree();
        InlineScriptTree* callerTree = curTree->caller();
        while (!callerTree->isOutermostCaller()) {
            curTree = callerTree;
            callerTree = curTree->caller();
        }

        // Return the callPc of the topmost inlined script.
        return curTree->callerPc();
    }

    // Return the range of this value, *before* any bailout checks. Contrast
    // this with the type() method, and the Range constructor which takes an
    // MDefinition*, which describe the value *after* any bailout checks.
    //
    // Warning: Range analysis is removing the bit-operations such as '| 0' at
    // the end of the transformations. Using this function to analyse any
    // operands after the truncate phase of the range analysis will lead to
    // errors. Instead, one should define the collectRangeInfoPreTrunc() to set
    // the right set of flags which are dependent on the range of the inputs.
    Range* range() const {
        MOZ_ASSERT(type() != MIRType_None);
        return range_;
    }
    void setRange(Range* range) {
        MOZ_ASSERT(type() != MIRType_None);
        range_ = range;
    }

    virtual HashNumber valueHash() const;
    virtual bool congruentTo(const MDefinition* ins) const {
        return false;
    }
    bool congruentIfOperandsEqual(const MDefinition* ins) const;
    virtual MDefinition* foldsTo(TempAllocator& alloc);
    virtual void analyzeEdgeCasesForward();
    virtual void analyzeEdgeCasesBackward();

    // When a floating-point value is used by nodes which would prefer to
    // recieve integer inputs, we may be able to help by computing our result
    // into an integer directly.
    //
    // A value can be truncated in 4 differents ways:
    //   1. Ignore Infinities (x / 0 --> 0).
    //   2. Ignore overflow (INT_MIN / -1 == (INT_MAX + 1) --> INT_MIN)
    //   3. Ignore negative zeros. (-0 --> 0)
    //   4. Ignore remainder. (3 / 4 --> 0)
    //
    // Indirect truncation is used to represent that we are interested in the
    // truncated result, but only if it can safely flow into operations which
    // are computed modulo 2^32, such as (2) and (3). Infinities are not safe,
    // as they would have absorbed other math operations. Remainders are not
    // safe, as fractions can be scaled up by multiplication.
    //
    // Division is a particularly interesting node here because it covers all 4
    // cases even when its own operands are integers.
    //
    // Note that these enum values are ordered from least value-modifying to
    // most value-modifying, and code relies on this ordering.
    enum TruncateKind {
        // No correction.
        NoTruncate = 0,
        // An integer is desired, but we can't skip bailout checks.
        TruncateAfterBailouts = 1,
        // The value will be truncated after some arithmetic (see above).
        IndirectTruncate = 2,
        // Direct and infallible truncation to int32.
        Truncate = 3
    };

    // |needTruncation| records the truncation kind of the results, such that it
    // can be used to truncate the operands of this instruction.  If
    // |needTruncation| function returns true, then the |truncate| function is
    // called on the same instruction to mutate the instruction, such as
    // updating the return type, the range and the specialization of the
    // instruction.
    virtual bool needTruncation(TruncateKind kind);
    virtual void truncate();

    // Determine what kind of truncate this node prefers for the operand at the
    // given index.
    virtual TruncateKind operandTruncateKind(size_t index) const;

    // Compute an absolute or symbolic range for the value of this node.
    virtual void computeRange(TempAllocator& alloc) {
    }

    // Collect information from the pre-truncated ranges.
    virtual void collectRangeInfoPreTrunc() {
    }

    MNode::Kind kind() const override {
        return MNode::Definition;
    }

    uint32_t id() const {
        MOZ_ASSERT(block_);
        return id_;
    }
    void setId(uint32_t id) {
        id_ = id;
    }

#define FLAG_ACCESSOR(flag) \
    bool is##flag() const {\
        return hasFlags(1 << flag);\
    }\
    void set##flag() {\
        MOZ_ASSERT(!hasFlags(1 << flag));\
        setFlags(1 << flag);\
    }\
    void setNot##flag() {\
        MOZ_ASSERT(hasFlags(1 << flag));\
        removeFlags(1 << flag);\
    }\
    void set##flag##Unchecked() {\
        setFlags(1 << flag);\
    } \
    void setNot##flag##Unchecked() {\
        removeFlags(1 << flag);\
    }

    MIR_FLAG_LIST(FLAG_ACCESSOR)
#undef FLAG_ACCESSOR

    // Return the type of this value. This may be speculative, and enforced
    // dynamically with the use of bailout checks. If all the bailout checks
    // pass, the value will have this type.
    //
    // Unless this is an MUrsh that has bailouts disabled, which, as a special
    // case, may return a value in (INT32_MAX,UINT32_MAX] even when its type()
    // is MIRType_Int32.
    MIRType type() const {
        return resultType_;
    }

    TemporaryTypeSet* resultTypeSet() const {
        return resultTypeSet_;
    }
    bool emptyResultTypeSet() const;

    bool mightBeType(MIRType type) const {
        MOZ_ASSERT(type != MIRType_Value);
        MOZ_ASSERT(type != MIRType_ObjectOrNull);

        if (type == this->type())
            return true;

        if (this->type() == MIRType_ObjectOrNull)
            return type == MIRType_Object || type == MIRType_Null;

        if (this->type() == MIRType_Value)
            return !resultTypeSet() || resultTypeSet()->mightBeMIRType(type);

        return false;
    }

    bool mightBeMagicType() const;

    bool maybeEmulatesUndefined(CompilerConstraintList* constraints);

    // Float32 specialization operations (see big comment in IonAnalysis before the Float32
    // specialization algorithm).
    virtual bool isFloat32Commutative() const { return false; }
    virtual bool canProduceFloat32() const { return false; }
    virtual bool canConsumeFloat32(MUse* use) const { return false; }
    virtual void trySpecializeFloat32(TempAllocator& alloc) {}
#ifdef DEBUG
    // Used during the pass that checks that Float32 flow into valid MDefinitions
    virtual bool isConsistentFloat32Use(MUse* use) const {
        return type() == MIRType_Float32 || canConsumeFloat32(use);
    }
#endif

    // Returns the beginning of this definition's use chain.
    MUseIterator usesBegin() const {
        return uses_.begin();
    }

    // Returns the end of this definition's use chain.
    MUseIterator usesEnd() const {
        return uses_.end();
    }

    bool canEmitAtUses() const {
        return !isEmittedAtUses();
    }

    // Removes a use at the given position
    void removeUse(MUse* use) {
        uses_.remove(use);
    }

#if defined(DEBUG) || defined(JS_JITSPEW)
    // Number of uses of this instruction. This function is only available
    // in DEBUG mode since it requires traversing the list. Most users should
    // use hasUses() or hasOneUse() instead.
    size_t useCount() const;

    // Number of uses of this instruction (only counting MDefinitions, ignoring
    // MResumePoints). This function is only available in DEBUG mode since it
    // requires traversing the list. Most users should use hasUses() or
    // hasOneUse() instead.
    size_t defUseCount() const;
#endif

    // Test whether this MDefinition has exactly one use.
    bool hasOneUse() const;

    // Test whether this MDefinition has exactly one use.
    // (only counting MDefinitions, ignoring MResumePoints)
    bool hasOneDefUse() const;

    // Test whether this MDefinition has at least one use.
    // (only counting MDefinitions, ignoring MResumePoints)
    bool hasDefUses() const;

    // Test whether this MDefinition has at least one non-recovered use.
    // (only counting MDefinitions, ignoring MResumePoints)
    bool hasLiveDefUses() const;

    bool hasUses() const {
        return !uses_.empty();
    }

    void addUse(MUse* use) {
        MOZ_ASSERT(use->producer() == this);
        uses_.pushFront(use);
    }
    void addUseUnchecked(MUse* use) {
        MOZ_ASSERT(use->producer() == this);
        uses_.pushFrontUnchecked(use);
    }
    void replaceUse(MUse* old, MUse* now) {
        MOZ_ASSERT(now->producer() == this);
        uses_.replace(old, now);
    }

    // Replace the current instruction by a dominating instruction |dom| in all
    // uses of the current instruction.
    void replaceAllUsesWith(MDefinition* dom);

    // Like replaceAllUsesWith, but doesn't set UseRemoved on |this|'s operands.
    void justReplaceAllUsesWith(MDefinition* dom);

    // Like justReplaceAllUsesWith, but doesn't replace its own use to the
    // dominating instruction (which would introduce a circular dependency).
    void justReplaceAllUsesWithExcept(MDefinition* dom);

    // Replace the current instruction by an optimized-out constant in all uses
    // of the current instruction. Note, that optimized-out constant should not
    // be observed, and thus they should not flow in any computation.
    void optimizeOutAllUses(TempAllocator& alloc);

    // Replace the current instruction by a dominating instruction |dom| in all
    // instruction, but keep the current instruction for resume point and
    // instruction which are recovered on bailouts.
    void replaceAllLiveUsesWith(MDefinition* dom);

    // Mark this instruction as having replaced all uses of ins, as during GVN,
    // returning false if the replacement should not be performed. For use when
    // GVN eliminates instructions which are not equivalent to one another.
    virtual bool updateForReplacement(MDefinition* ins) {
        return true;
    }

    void setVirtualRegister(uint32_t vreg) {
        virtualRegister_ = vreg;
        setLoweredUnchecked();
    }
    uint32_t virtualRegister() const {
        MOZ_ASSERT(isLowered());
        return virtualRegister_;
    }

  public:
    // Opcode testing and casts.
    template<typename MIRType> bool is() const {
        return op() == MIRType::classOpcode;
    }
    template<typename MIRType> MIRType* to() {
        MOZ_ASSERT(this->is<MIRType>());
        return static_cast<MIRType*>(this);
    }
    template<typename MIRType> const MIRType* to() const {
        MOZ_ASSERT(this->is<MIRType>());
        return static_cast<const MIRType*>(this);
    }
#   define OPCODE_CASTS(opcode)           \
    bool is##opcode() const {             \
        return this->is<M##opcode>();     \
    }                                     \
    M##opcode* to##opcode() {             \
        return this->to<M##opcode>();     \
    }                                     \
    const M##opcode* to##opcode() const { \
        return this->to<M##opcode>();     \
    }
    MIR_OPCODE_LIST(OPCODE_CASTS)
#   undef OPCODE_CASTS

    bool isConstantValue() const {
        return isConstant() || (isBox() && getOperand(0)->isConstant());
    }
    const Value& constantValue();
    const Value* constantVp();
    bool constantToBoolean();

    inline MInstruction* toInstruction();
    inline const MInstruction* toInstruction() const;
    bool isInstruction() const {
        return !isPhi();
    }

    virtual bool isControlInstruction() const {
        return false;
    }
    inline MControlInstruction* toControlInstruction();

    void setResultType(MIRType type) {
        resultType_ = type;
    }
    void setResultTypeSet(TemporaryTypeSet* types) {
        resultTypeSet_ = types;
    }

    MInstruction* dependency() const {
        return dependency_;
    }
    void setDependency(MInstruction* dependency) {
        dependency_ = dependency;
    }
    virtual AliasSet getAliasSet() const {
        // Instructions are effectful by default.
        return AliasSet::Store(AliasSet::Any);
    }
    bool isEffectful() const {
        return getAliasSet().isStore();
    }

#ifdef DEBUG
    virtual bool needsResumePoint() const {
        // Return whether this instruction should have its own resume point.
        return isEffectful();
    }
#endif
    virtual bool mightAlias(const MDefinition* store) const {
        // Return whether this load may depend on the specified store, given
        // that the alias sets intersect. This may be refined to exclude
        // possible aliasing in cases where alias set flags are too imprecise.
        MOZ_ASSERT(!isEffectful() && store->isEffectful());
        MOZ_ASSERT(getAliasSet().flags() & store->getAliasSet().flags());
        return true;
    }

    virtual bool canRecoverOnBailout() const {
        return false;
    }
};

// An MUseDefIterator walks over uses in a definition, skipping any use that is
// not a definition. Items from the use list must not be deleted during
// iteration.
class MUseDefIterator
{
    const MDefinition* def_;
    MUseIterator current_;

    MUseIterator search(MUseIterator start) {
        MUseIterator i(start);
        for (; i != def_->usesEnd(); i++) {
            if (i->consumer()->isDefinition())
                return i;
        }
        return def_->usesEnd();
    }

  public:
    explicit MUseDefIterator(const MDefinition* def)
      : def_(def),
        current_(search(def->usesBegin()))
    { }

    explicit operator bool() const {
        return current_ != def_->usesEnd();
    }
    MUseDefIterator operator ++() {
        MOZ_ASSERT(current_ != def_->usesEnd());
        ++current_;
        current_ = search(current_);
        return *this;
    }
    MUseDefIterator operator ++(int) {
        MUseDefIterator old(*this);
        operator++();
        return old;
    }
    MUse* use() const {
        return *current_;
    }
    MDefinition* def() const {
        return current_->consumer()->toDefinition();
    }
};

typedef Vector<MDefinition*, 8, JitAllocPolicy> MDefinitionVector;
typedef Vector<MInstruction*, 6, JitAllocPolicy> MInstructionVector;

// An instruction is an SSA name that is inserted into a basic block's IR
// stream.
class MInstruction
  : public MDefinition,
    public InlineListNode<MInstruction>
{
    MResumePoint* resumePoint_;

  public:
    MInstruction()
      : resumePoint_(nullptr)
    { }

    // Copying an instruction leaves the block and resume point as empty.
    explicit MInstruction(const MInstruction& other)
      : MDefinition(other),
        resumePoint_(nullptr)
    { }

    // Convenient function used for replacing a load by the value of the store
    // if the types are match, and boxing the value if they do not match.
    //
    // Note: There is no need for such function in AsmJS functions as they do
    // not use any MIRType_Value.
    MDefinition* foldsToStoredValue(TempAllocator& alloc, MDefinition* loaded);

    void setResumePoint(MResumePoint* resumePoint);

    // Used to transfer the resume point to the rewritten instruction.
    void stealResumePoint(MInstruction* ins);
    void moveResumePointAsEntry();
    void clearResumePoint();
    MResumePoint* resumePoint() const {
        return resumePoint_;
    }

    // For instructions which can be cloned with new inputs, with all other
    // information being the same. clone() implementations do not need to worry
    // about cloning generic MInstruction/MDefinition state like flags and
    // resume points.
    virtual bool canClone() const {
        return false;
    }
    virtual MInstruction* clone(TempAllocator& alloc, const MDefinitionVector& inputs) const {
        MOZ_CRASH();
    }

    // Instructions needing to hook into type analysis should return a
    // TypePolicy.
    virtual TypePolicy* typePolicy() = 0;
    virtual MIRType typePolicySpecialization() = 0;
};

#define INSTRUCTION_HEADER_WITHOUT_TYPEPOLICY(opcode)                       \
    static const Opcode classOpcode = MDefinition::Op_##opcode;             \
    Opcode op() const override {                                            \
        return classOpcode;                                                 \
    }                                                                       \
    const char* opName() const override {                                   \
        return #opcode;                                                     \
    }                                                                       \
    void accept(MDefinitionVisitor* visitor) override {                     \
        visitor->visit##opcode(this);                                       \
    }

#define INSTRUCTION_HEADER(opcode)                                          \
    INSTRUCTION_HEADER_WITHOUT_TYPEPOLICY(opcode)                           \
    virtual TypePolicy* typePolicy() override;                              \
    virtual MIRType typePolicySpecialization() override;

#define ALLOW_CLONE(typename)                                               \
    bool canClone() const override {                                        \
        return true;                                                        \
    }                                                                       \
    MInstruction* clone(TempAllocator& alloc,                               \
                        const MDefinitionVector& inputs) const override {   \
        MInstruction* res = new(alloc) typename(*this);                     \
        for (size_t i = 0; i < numOperands(); i++)                          \
            res->replaceOperand(i, inputs[i]);                              \
        return res;                                                         \
    }

template <size_t Arity>
class MAryInstruction : public MInstruction
{
    mozilla::Array<MUse, Arity> operands_;

  protected:
    MUse* getUseFor(size_t index) final override {
        return &operands_[index];
    }
    const MUse* getUseFor(size_t index) const final override {
        return &operands_[index];
    }
    void initOperand(size_t index, MDefinition* operand) {
        operands_[index].init(operand, this);
    }

  public:
    MDefinition* getOperand(size_t index) const final override {
        return operands_[index].producer();
    }
    size_t numOperands() const final override {
        return Arity;
    }
    size_t indexOf(const MUse* u) const final override {
        MOZ_ASSERT(u >= &operands_[0]);
        MOZ_ASSERT(u <= &operands_[numOperands() - 1]);
        return u - &operands_[0];
    }
    void replaceOperand(size_t index, MDefinition* operand) final override {
        operands_[index].replaceProducer(operand);
    }

    MAryInstruction() { }

    explicit MAryInstruction(const MAryInstruction<Arity>& other)
      : MInstruction(other)
    {
        for (int i = 0; i < (int) Arity; i++) // N.B. use |int| to avoid warnings when Arity == 0
            operands_[i].init(other.operands_[i].producer(), this);
    }
};

class MNullaryInstruction
  : public MAryInstruction<0>,
    public NoTypePolicy::Data
{ };

class MUnaryInstruction : public MAryInstruction<1>
{
  protected:
    explicit MUnaryInstruction(MDefinition* ins)
    {
        initOperand(0, ins);
    }

  public:
    MDefinition* input() const {
        return getOperand(0);
    }
};

class MBinaryInstruction : public MAryInstruction<2>
{
  protected:
    MBinaryInstruction(MDefinition* left, MDefinition* right)
    {
        initOperand(0, left);
        initOperand(1, right);
    }

  public:
    MDefinition* lhs() const {
        return getOperand(0);
    }
    MDefinition* rhs() const {
        return getOperand(1);
    }
    void swapOperands() {
        MDefinition* temp = getOperand(0);
        replaceOperand(0, getOperand(1));
        replaceOperand(1, temp);
    }

  protected:
    HashNumber valueHash() const
    {
        MDefinition* lhs = getOperand(0);
        MDefinition* rhs = getOperand(1);

        return op() + lhs->id() + rhs->id();
    }
    bool binaryCongruentTo(const MDefinition* ins) const
    {
        if (op() != ins->op())
            return false;

        if (type() != ins->type())
            return false;

        if (isEffectful() || ins->isEffectful())
            return false;

        const MDefinition* left = getOperand(0);
        const MDefinition* right = getOperand(1);
        const MDefinition* tmp;

        if (isCommutative() && left->id() > right->id()) {
            tmp = right;
            right = left;
            left = tmp;
        }

        const MBinaryInstruction* bi = static_cast<const MBinaryInstruction*>(ins);
        const MDefinition* insLeft = bi->getOperand(0);
        const MDefinition* insRight = bi->getOperand(1);
        if (isCommutative() && insLeft->id() > insRight->id()) {
            tmp = insRight;
            insRight = insLeft;
            insLeft = tmp;
        }

        return left == insLeft &&
               right == insRight;
    }

  public:
    // Return if the operands to this instruction are both unsigned.
    static bool unsignedOperands(MDefinition* left, MDefinition* right);
    bool unsignedOperands();

    // Replace any wrapping operands with the underlying int32 operands
    // in case of unsigned operands.
    void replaceWithUnsignedOperands();
};

class MTernaryInstruction : public MAryInstruction<3>
{
  protected:
    MTernaryInstruction(MDefinition* first, MDefinition* second, MDefinition* third)
    {
        initOperand(0, first);
        initOperand(1, second);
        initOperand(2, third);
    }

  protected:
    HashNumber valueHash() const
    {
        MDefinition* first = getOperand(0);
        MDefinition* second = getOperand(1);
        MDefinition* third = getOperand(2);

        return op() + first->id() + second->id() + third->id();
    }
};

class MQuaternaryInstruction : public MAryInstruction<4>
{
  protected:
    MQuaternaryInstruction(MDefinition* first, MDefinition* second,
                           MDefinition* third, MDefinition* fourth)
    {
        initOperand(0, first);
        initOperand(1, second);
        initOperand(2, third);
        initOperand(3, fourth);
    }

  protected:
    HashNumber valueHash() const
    {
        MDefinition* first = getOperand(0);
        MDefinition* second = getOperand(1);
        MDefinition* third = getOperand(2);
        MDefinition* fourth = getOperand(3);

        return op() + first->id() + second->id() +
                      third->id() + fourth->id();
    }
};

template <class T>
class MVariadicT : public T
{
    FixedList<MUse> operands_;

  protected:
    bool init(TempAllocator& alloc, size_t length) {
        return operands_.init(alloc, length);
    }
    void initOperand(size_t index, MDefinition* operand) {
        // FixedList doesn't initialize its elements, so do an unchecked init.
        operands_[index].initUnchecked(operand, this);
    }
    MUse* getUseFor(size_t index) final override {
        return &operands_[index];
    }
    const MUse* getUseFor(size_t index) const final override {
        return &operands_[index];
    }

  public:
    // Will assert if called before initialization.
    MDefinition* getOperand(size_t index) const final override {
        return operands_[index].producer();
    }
    size_t numOperands() const final override {
        return operands_.length();
    }
    size_t indexOf(const MUse* u) const final override {
        MOZ_ASSERT(u >= &operands_[0]);
        MOZ_ASSERT(u <= &operands_[numOperands() - 1]);
        return u - &operands_[0];
    }
    void replaceOperand(size_t index, MDefinition* operand) final override {
        operands_[index].replaceProducer(operand);
    }
};

typedef MVariadicT<MInstruction> MVariadicInstruction;

// Generates an LSnapshot without further effect.
class MStart : public MNullaryInstruction
{
  public:
    enum StartType {
        StartType_Default,
        StartType_Osr
    };

  private:
    StartType startType_;

  private:
    explicit MStart(StartType startType)
      : startType_(startType)
    { }

  public:
    INSTRUCTION_HEADER(Start)
    static MStart* New(TempAllocator& alloc, StartType startType) {
        return new(alloc) MStart(startType);
    }

    StartType startType() {
        return startType_;
    }
};

// Instruction marking on entrypoint for on-stack replacement.
// OSR may occur at loop headers (at JSOP_TRACE).
// There is at most one MOsrEntry per MIRGraph.
class MOsrEntry : public MNullaryInstruction
{
  protected:
    MOsrEntry() {
        setResultType(MIRType_Pointer);
    }

  public:
    INSTRUCTION_HEADER(OsrEntry)
    static MOsrEntry* New(TempAllocator& alloc) {
        return new(alloc) MOsrEntry;
    }
};

// No-op instruction. This cannot be moved or eliminated, and is intended for
// anchoring resume points at arbitrary points in a block.
class MNop : public MNullaryInstruction
{
  protected:
    MNop() {
    }

  public:
    INSTRUCTION_HEADER(Nop)
    static MNop* New(TempAllocator& alloc) {
        return new(alloc) MNop();
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    ALLOW_CLONE(MNop)
};

// Truncation barrier. This is intended for protecting its input against
// follow-up truncation optimizations.
class MLimitedTruncate
  : public MUnaryInstruction,
    public ConvertToInt32Policy<0>::Data
{
  public:
    TruncateKind truncate_;
    TruncateKind truncateLimit_;

  protected:
    MLimitedTruncate(MDefinition* input, TruncateKind limit)
      : MUnaryInstruction(input),
        truncate_(NoTruncate),
        truncateLimit_(limit)
    {
        setResultType(MIRType_Int32);
        setResultTypeSet(input->resultTypeSet());
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(LimitedTruncate)
    static MLimitedTruncate* New(TempAllocator& alloc, MDefinition* input, TruncateKind kind) {
        return new(alloc) MLimitedTruncate(input, kind);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    void computeRange(TempAllocator& alloc) override;
    bool needTruncation(TruncateKind kind) override;
    TruncateKind operandTruncateKind(size_t index) const override;
    TruncateKind truncateKind() const {
        return truncate_;
    }
    void setTruncateKind(TruncateKind kind) {
        truncate_ = kind;
    }
};

// A constant js::Value.
class MConstant : public MNullaryInstruction
{
    Value value_;

  protected:
    MConstant(const Value& v, CompilerConstraintList* constraints);
    explicit MConstant(JSObject* obj);

  public:
    INSTRUCTION_HEADER(Constant)
    static MConstant* New(TempAllocator& alloc, const Value& v,
                          CompilerConstraintList* constraints = nullptr);
    static MConstant* NewTypedValue(TempAllocator& alloc, const Value& v, MIRType type,
                                    CompilerConstraintList* constraints = nullptr);
    static MConstant* NewAsmJS(TempAllocator& alloc, const Value& v, MIRType type);
    static MConstant* NewConstraintlessObject(TempAllocator& alloc, JSObject* v);

    const js::Value& value() const {
        return value_;
    }
    const js::Value* vp() const {
        return &value_;
    }
    bool valueToBoolean() const {
        // A hack to avoid this wordy pattern everywhere in the JIT.
        return ToBoolean(HandleValue::fromMarkedLocation(&value_));
    }

    void printOpcode(GenericPrinter& out) const override;

    HashNumber valueHash() const override;
    bool congruentTo(const MDefinition* ins) const override;

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool updateForReplacement(MDefinition* def) override {
        MConstant* c = def->toConstant();
        // During constant folding, we don't want to replace a float32
        // value by a double value.
        if (type() == MIRType_Float32)
            return c->type() == MIRType_Float32;
        if (type() == MIRType_Double)
            return c->type() != MIRType_Float32;
        return true;
    }

    void computeRange(TempAllocator& alloc) override;
    bool needTruncation(TruncateKind kind) override;
    void truncate() override;

    bool canProduceFloat32() const override;

    ALLOW_CLONE(MConstant)
};

// Generic constructor of SIMD valuesX4.
class MSimdValueX4
  : public MQuaternaryInstruction,
    public Mix4Policy<SimdScalarPolicy<0>, SimdScalarPolicy<1>,
                      SimdScalarPolicy<2>, SimdScalarPolicy<3> >::Data
{
  protected:
    MSimdValueX4(MIRType type, MDefinition* x, MDefinition* y, MDefinition* z, MDefinition* w)
      : MQuaternaryInstruction(x, y, z, w)
    {
        MOZ_ASSERT(IsSimdType(type));
        MOZ_ASSERT(SimdTypeToLength(type) == 4);

        setMovable();
        setResultType(type);
    }

  public:
    INSTRUCTION_HEADER(SimdValueX4)

    static MSimdValueX4* New(TempAllocator& alloc, MIRType type, MDefinition* x,
                             MDefinition* y, MDefinition* z, MDefinition* w)
    {
        return new(alloc) MSimdValueX4(type, x, y, z, w);
    }

    static MSimdValueX4* NewAsmJS(TempAllocator& alloc, MIRType type, MDefinition* x,
                                  MDefinition* y, MDefinition* z, MDefinition* w)
    {
        mozilla::DebugOnly<MIRType> laneType = SimdTypeToLaneType(type);
        MOZ_ASSERT(laneType == x->type());
        MOZ_ASSERT(laneType == y->type());
        MOZ_ASSERT(laneType == z->type());
        MOZ_ASSERT(laneType == w->type());
        return MSimdValueX4::New(alloc, type, x, y, z, w);
    }

    bool canConsumeFloat32(MUse* use) const override {
        return SimdTypeToLaneType(type()) == MIRType_Float32;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    ALLOW_CLONE(MSimdValueX4)
};

// Generic constructor of SIMD valuesX4.
class MSimdSplatX4
  : public MUnaryInstruction,
    public SimdScalarPolicy<0>::Data
{
  protected:
    MSimdSplatX4(MIRType type, MDefinition* v)
      : MUnaryInstruction(v)
    {
        MOZ_ASSERT(IsSimdType(type));
        setMovable();
        setResultType(type);
    }

  public:
    INSTRUCTION_HEADER(SimdSplatX4)

    static MSimdSplatX4* NewAsmJS(TempAllocator& alloc, MDefinition* v, MIRType type)
    {
        MOZ_ASSERT(SimdTypeToLaneType(type) == v->type());
        return new(alloc) MSimdSplatX4(type, v);
    }

    static MSimdSplatX4* New(TempAllocator& alloc, MDefinition* v, MIRType type)
    {
        return new(alloc) MSimdSplatX4(type, v);
    }

    bool canConsumeFloat32(MUse* use) const override {
        return SimdTypeToLaneType(type()) == MIRType_Float32;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    ALLOW_CLONE(MSimdSplatX4)
};

// A constant SIMD value.
class MSimdConstant
  : public MNullaryInstruction
{
    SimdConstant value_;

  protected:
    MSimdConstant(const SimdConstant& v, MIRType type) : value_(v) {
        MOZ_ASSERT(IsSimdType(type));
        setMovable();
        setResultType(type);
    }

  public:
    INSTRUCTION_HEADER(SimdConstant)
    static MSimdConstant* New(TempAllocator& alloc, const SimdConstant& v, MIRType type) {
        return new(alloc) MSimdConstant(v, type);
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isSimdConstant())
            return false;
        return value() == ins->toSimdConstant()->value();
    }

    const SimdConstant& value() const {
        return value_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    ALLOW_CLONE(MSimdConstant)
};

// Converts all lanes of a given vector into the type of another vector
class MSimdConvert
  : public MUnaryInstruction,
    public SimdPolicy<0>::Data
{
    MSimdConvert(MDefinition* obj, MIRType fromType, MIRType toType)
      : MUnaryInstruction(obj)
    {
        MOZ_ASSERT(IsSimdType(toType));
        setResultType(toType);
        specialization_ = fromType; // expects fromType as input

        setMovable();
        if (IsFloatingPointSimdType(fromType) && IsIntegerSimdType(toType)) {
            // Does the extra range check => do not remove
            setGuard();
        }
    }

  public:
    INSTRUCTION_HEADER(SimdConvert)
    static MSimdConvert* NewAsmJS(TempAllocator& alloc, MDefinition* obj, MIRType fromType,
                                  MIRType toType)
    {
        MOZ_ASSERT(IsSimdType(obj->type()) && fromType == obj->type());
        return new(alloc) MSimdConvert(obj, fromType, toType);
    }

    static MSimdConvert* New(TempAllocator& alloc, MDefinition* obj, MIRType fromType,
                             MIRType toType)
    {
        return new(alloc) MSimdConvert(obj, fromType, toType);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    ALLOW_CLONE(MSimdConvert)
};

// Casts bits of a vector input to another SIMD type (doesn't generate code).
class MSimdReinterpretCast
  : public MUnaryInstruction,
    public SimdPolicy<0>::Data
{
    MSimdReinterpretCast(MDefinition* obj, MIRType fromType, MIRType toType)
      : MUnaryInstruction(obj)
    {
        MOZ_ASSERT(IsSimdType(toType));
        setMovable();
        setResultType(toType);
        specialization_ = fromType; // expects fromType as input
    }

  public:
    INSTRUCTION_HEADER(SimdReinterpretCast)
    static MSimdReinterpretCast* NewAsmJS(TempAllocator& alloc, MDefinition* obj, MIRType fromType,
                                          MIRType toType)
    {
        MOZ_ASSERT(IsSimdType(obj->type()) && fromType == obj->type());
        return new(alloc) MSimdReinterpretCast(obj, fromType, toType);
    }

    static MSimdReinterpretCast* New(TempAllocator& alloc, MDefinition* obj, MIRType fromType,
                                     MIRType toType)
    {
        return new(alloc) MSimdReinterpretCast(obj, fromType, toType);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    ALLOW_CLONE(MSimdReinterpretCast)
};

// Extracts a lane element from a given vector type, given by its lane symbol.
class MSimdExtractElement
  : public MUnaryInstruction,
    public SimdPolicy<0>::Data
{
  protected:
    SimdLane lane_;

    MSimdExtractElement(MDefinition* obj, MIRType vecType, MIRType laneType, SimdLane lane)
      : MUnaryInstruction(obj), lane_(lane)
    {
        MOZ_ASSERT(IsSimdType(vecType));
        MOZ_ASSERT(uint32_t(lane) < SimdTypeToLength(vecType));
        MOZ_ASSERT(!IsSimdType(laneType));
        MOZ_ASSERT(SimdTypeToLaneType(vecType) == laneType);

        setMovable();
        specialization_ = vecType;
        setResultType(laneType);
    }

  public:
    INSTRUCTION_HEADER(SimdExtractElement)

    static MSimdExtractElement* NewAsmJS(TempAllocator& alloc, MDefinition* obj, MIRType type,
                                         SimdLane lane)
    {
        return new(alloc) MSimdExtractElement(obj, obj->type(), type, lane);
    }

    static MSimdExtractElement* New(TempAllocator& alloc, MDefinition* obj, MIRType vecType,
                                    MIRType scalarType, SimdLane lane)
    {
        return new(alloc) MSimdExtractElement(obj, vecType, scalarType, lane);
    }

    SimdLane lane() const {
        return lane_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isSimdExtractElement())
            return false;
        const MSimdExtractElement* other = ins->toSimdExtractElement();
        if (other->lane_ != lane_)
            return false;
        return congruentIfOperandsEqual(other);
    }
    ALLOW_CLONE(MSimdExtractElement)
};

// Replaces the datum in the given lane by a scalar value of the same type.
class MSimdInsertElement
  : public MBinaryInstruction,
    public MixPolicy< SimdSameAsReturnedTypePolicy<0>, SimdScalarPolicy<1> >::Data
{
  private:
    SimdLane lane_;

    MSimdInsertElement(MDefinition* vec, MDefinition* val, MIRType type, SimdLane lane)
      : MBinaryInstruction(vec, val), lane_(lane)
    {
        MOZ_ASSERT(IsSimdType(type));
        setMovable();
        setResultType(type);
    }

  public:
    INSTRUCTION_HEADER(SimdInsertElement)

    static MSimdInsertElement* NewAsmJS(TempAllocator& alloc, MDefinition* vec, MDefinition* val,
                                         MIRType type, SimdLane lane)
    {
        MOZ_ASSERT(vec->type() == type);
        MOZ_ASSERT(SimdTypeToLaneType(type) == val->type());
        return new(alloc) MSimdInsertElement(vec, val, type, lane);
    }

    static MSimdInsertElement* New(TempAllocator& alloc, MDefinition* vec, MDefinition* val,
                                   MIRType type, SimdLane lane)
    {
        return new(alloc) MSimdInsertElement(vec, val, type, lane);
    }

    MDefinition* vector() {
        return getOperand(0);
    }
    MDefinition* value() {
        return getOperand(1);
    }
    SimdLane lane() const {
        return lane_;
    }

    static const char* LaneName(SimdLane lane) {
        switch (lane) {
          case LaneX: return "lane x";
          case LaneY: return "lane y";
          case LaneZ: return "lane z";
          case LaneW: return "lane w";
        }
        MOZ_CRASH("unknown lane");
    }

    bool canConsumeFloat32(MUse* use) const override {
        return use == getUseFor(1) && SimdTypeToLaneType(type()) == MIRType_Float32;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool congruentTo(const MDefinition* ins) const override {
        return binaryCongruentTo(ins) && lane_ == ins->toSimdInsertElement()->lane();
    }

    void printOpcode(GenericPrinter& out) const override;

    ALLOW_CLONE(MSimdInsertElement)
};

// Extracts the sign bits from a given vector, returning an MIRType_Int32.
class MSimdSignMask
  : public MUnaryInstruction,
    public SimdPolicy<0>::Data
{
  protected:
    explicit MSimdSignMask(MDefinition* obj, MIRType type)
      : MUnaryInstruction(obj)
    {
        setResultType(MIRType_Int32);
        specialization_ = type;
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(SimdSignMask)

    static MSimdSignMask* NewAsmJS(TempAllocator& alloc, MDefinition* obj)
    {
        MOZ_ASSERT(IsSimdType(obj->type()));
        return new(alloc) MSimdSignMask(obj, obj->type());
    }

    static MSimdSignMask* New(TempAllocator& alloc, MDefinition* obj, MIRType type)
    {
        return new(alloc) MSimdSignMask(obj, type);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isSimdSignMask())
            return false;
        return congruentIfOperandsEqual(ins);
    }

    ALLOW_CLONE(MSimdSignMask)
};

// Base for the MSimdSwizzle and MSimdShuffle classes.
class MSimdShuffleBase
{
  protected:
    // As of now, there are at most 4 lanes. For each lane, we need to know
    // which input we choose and which of the 4 lanes we choose; that can be
    // packed in 3 bits for each lane, so 12 bits in total.
    uint32_t laneMask_;
    uint32_t arity_;

    MSimdShuffleBase(uint32_t laneX, uint32_t laneY, uint32_t laneZ, uint32_t laneW, MIRType type)
    {
        MOZ_ASSERT(SimdTypeToLength(type) == 4);
        MOZ_ASSERT(IsSimdType(type));
        laneMask_ = (laneX << 0) | (laneY << 3) | (laneZ << 6) | (laneW << 9);
        arity_ = 4;
    }

    bool sameLanes(const MSimdShuffleBase* other) const {
        return laneMask_ == other->laneMask_;
    }

  public:
    // For now, these formulas are fine for x4 types. They'll need to be
    // generalized for other SIMD type lengths.
    uint32_t laneX() const { MOZ_ASSERT(arity_ == 4); return laneMask_ & 7; }
    uint32_t laneY() const { MOZ_ASSERT(arity_ == 4); return (laneMask_ >> 3) & 7; }
    uint32_t laneZ() const { MOZ_ASSERT(arity_ == 4); return (laneMask_ >> 6) & 7; }
    uint32_t laneW() const { MOZ_ASSERT(arity_ == 4); return (laneMask_ >> 9) & 7; }

    bool lanesMatch(uint32_t x, uint32_t y, uint32_t z, uint32_t w) const {
        return ((x << 0) | (y << 3) | (z << 6) | (w << 9)) == laneMask_;
    }
};

// Applies a shuffle operation to the input, putting the input lanes as
// indicated in the output register's lanes. This implements the SIMD.js
// "shuffle" function, that takes one vector and one mask.
class MSimdSwizzle
  : public MUnaryInstruction,
    public MSimdShuffleBase,
    public NoTypePolicy::Data
{
  protected:
    MSimdSwizzle(MDefinition* obj, MIRType type,
                 uint32_t laneX, uint32_t laneY, uint32_t laneZ, uint32_t laneW)
      : MUnaryInstruction(obj), MSimdShuffleBase(laneX, laneY, laneZ, laneW, type)
    {
        MOZ_ASSERT(laneX < 4 && laneY < 4 && laneZ < 4 && laneW < 4);
        MOZ_ASSERT(IsSimdType(obj->type()));
        MOZ_ASSERT(IsSimdType(type));
        MOZ_ASSERT(obj->type() == type);
        setResultType(type);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(SimdSwizzle)

    static MSimdSwizzle* New(TempAllocator& alloc, MDefinition* obj, MIRType type,
                             uint32_t laneX, uint32_t laneY, uint32_t laneZ, uint32_t laneW)
    {
        return new(alloc) MSimdSwizzle(obj, type, laneX, laneY, laneZ, laneW);
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isSimdSwizzle())
            return false;
        const MSimdSwizzle* other = ins->toSimdSwizzle();
        return sameLanes(other) && congruentIfOperandsEqual(other);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    ALLOW_CLONE(MSimdSwizzle)
};

// A "general swizzle" is a swizzle or a shuffle with non-constant lane
// indices.  This is the one that Ion inlines and it can be folded into a
// MSimdSwizzle/MSimdShuffle if lane indices are constant.  Performance of
// general swizzle/shuffle does not really matter, as we expect to get
// constant indices most of the time.
class MSimdGeneralShuffle :
    public MVariadicInstruction,
    public SimdShufflePolicy::Data
{
    unsigned numVectors_;
    unsigned numLanes_;

  protected:
    MSimdGeneralShuffle(unsigned numVectors, unsigned numLanes, MIRType type)
      : numVectors_(numVectors), numLanes_(numLanes)
    {
        MOZ_ASSERT(IsSimdType(type));
        MOZ_ASSERT(SimdTypeToLength(type) == numLanes_);

        setResultType(type);
        specialization_ = type;
        setGuard(); // throws if lane index is out of bounds
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(SimdGeneralShuffle);

    static MSimdGeneralShuffle* New(TempAllocator& alloc, unsigned numVectors, unsigned numLanes,
                                    MIRType type)
    {
        return new(alloc) MSimdGeneralShuffle(numVectors, numLanes, type);
    }

    bool init(TempAllocator& alloc) {
        return MVariadicInstruction::init(alloc, numVectors_ + numLanes_);
    }
    void setVector(unsigned i, MDefinition* vec) {
        MOZ_ASSERT(i < numVectors_);
        initOperand(i, vec);
    }
    void setLane(unsigned i, MDefinition* laneIndex) {
        MOZ_ASSERT(i < numLanes_);
        initOperand(numVectors_ + i, laneIndex);
    }

    unsigned numVectors() const {
        return numVectors_;
    }
    unsigned numLanes() const {
        return numLanes_;
    }
    MDefinition* vector(unsigned i) const {
        MOZ_ASSERT(i < numVectors_);
        return getOperand(i);
    }
    MDefinition* lane(unsigned i) const {
        MOZ_ASSERT(i < numLanes_);
        return getOperand(numVectors_ + i);
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isSimdGeneralShuffle())
            return false;
        const MSimdGeneralShuffle* other = ins->toSimdGeneralShuffle();
        return numVectors_ == other->numVectors() &&
               numLanes_ == other->numLanes() &&
               congruentIfOperandsEqual(other);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// Applies a shuffle operation to the inputs, selecting the 2 first lanes of the
// output from lanes of the first input, and the 2 last lanes of the output from
// lanes of the second input.
class MSimdShuffle
  : public MBinaryInstruction,
    public MSimdShuffleBase,
    public NoTypePolicy::Data
{
    MSimdShuffle(MDefinition* lhs, MDefinition* rhs, MIRType type,
                 uint32_t laneX, uint32_t laneY, uint32_t laneZ, uint32_t laneW)
      : MBinaryInstruction(lhs, rhs), MSimdShuffleBase(laneX, laneY, laneZ, laneW, lhs->type())
    {
        MOZ_ASSERT(laneX < 8 && laneY < 8 && laneZ < 8 && laneW < 8);
        MOZ_ASSERT(IsSimdType(lhs->type()));
        MOZ_ASSERT(IsSimdType(rhs->type()));
        MOZ_ASSERT(lhs->type() == rhs->type());
        MOZ_ASSERT(IsSimdType(type));
        MOZ_ASSERT(lhs->type() == type);
        setResultType(type);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(SimdShuffle)

    static MInstruction* New(TempAllocator& alloc, MDefinition* lhs, MDefinition* rhs,
                             MIRType type, uint32_t laneX, uint32_t laneY, uint32_t laneZ,
                             uint32_t laneW)
    {
        // Swap operands so that new lanes come from LHS in majority.
        // In the balanced case, swap operands if needs be, in order to be able
        // to do only one vshufps on x86.
        unsigned lanesFromLHS = (laneX < 4) + (laneY < 4) + (laneZ < 4) + (laneW < 4);
        if (lanesFromLHS < 2 || (lanesFromLHS == 2 && laneX >= 4 && laneY >=4)) {
            laneX = (laneX + 4) % 8;
            laneY = (laneY + 4) % 8;
            laneZ = (laneZ + 4) % 8;
            laneW = (laneW + 4) % 8;
            mozilla::Swap(lhs, rhs);
        }

        // If all lanes come from the same vector, just use swizzle instead.
        if (laneX < 4 && laneY < 4 && laneZ < 4 && laneW < 4)
            return MSimdSwizzle::New(alloc, lhs, type, laneX, laneY, laneZ, laneW);

        return new(alloc) MSimdShuffle(lhs, rhs, type, laneX, laneY, laneZ, laneW);
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isSimdShuffle())
            return false;
        const MSimdShuffle* other = ins->toSimdShuffle();
        return sameLanes(other) && binaryCongruentTo(other);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    ALLOW_CLONE(MSimdShuffle)
};

class MSimdUnaryArith
  : public MUnaryInstruction,
    public SimdSameAsReturnedTypePolicy<0>::Data
{
  public:
    enum Operation {
#define OP_LIST_(OP) OP,
        UNARY_ARITH_FLOAT32X4_SIMD_OP(OP_LIST_)
        neg,
        not_
#undef OP_LIST_
    };

    static const char* OperationName(Operation op) {
        switch (op) {
          case abs:                         return "abs";
          case neg:                         return "neg";
          case not_:                        return "not";
          case reciprocalApproximation:     return "reciprocalApproximation";
          case reciprocalSqrtApproximation: return "reciprocalSqrtApproximation";
          case sqrt:                        return "sqrt";
        }
        MOZ_CRASH("unexpected operation");
    }

  private:
    Operation operation_;

    MSimdUnaryArith(MDefinition* def, Operation op, MIRType type)
      : MUnaryInstruction(def), operation_(op)
    {
        MOZ_ASSERT_IF(type == MIRType_Int32x4, op == neg || op == not_);
        setResultType(type);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(SimdUnaryArith)

    static MSimdUnaryArith* New(TempAllocator& alloc, MDefinition* def, Operation op, MIRType t)
    {
        return new(alloc) MSimdUnaryArith(def, op, t);
    }

    static MSimdUnaryArith* NewAsmJS(TempAllocator& alloc, MDefinition* def,
                                     Operation op, MIRType t)
    {
        MOZ_ASSERT(IsSimdType(t));
        MOZ_ASSERT(def->type() == t);
        return new(alloc) MSimdUnaryArith(def, op, t);
    }

    Operation operation() const { return operation_; }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins) && ins->toSimdUnaryArith()->operation() == operation();
    }

    void printOpcode(GenericPrinter& out) const override;

    ALLOW_CLONE(MSimdUnaryArith);
};

// Compares each value of a SIMD vector to each corresponding lane's value of
// another SIMD vector, and returns a int32x4 vector containing the results of
// the comparison: all bits are set to 1 if the comparison is true, 0 otherwise.
class MSimdBinaryComp
  : public MBinaryInstruction,
    public SimdAllPolicy::Data
{
  public:
    enum Operation {
#define NAME_(x) x,
        COMP_COMMONX4_TO_INT32X4_SIMD_OP(NAME_)
#undef NAME_
    };

    static const char* OperationName(Operation op) {
        switch (op) {
#define NAME_(x) case x: return #x;
        COMP_COMMONX4_TO_INT32X4_SIMD_OP(NAME_)
#undef NAME_
        }
        MOZ_CRASH("unexpected operation");
    }

  private:
    Operation operation_;

    MSimdBinaryComp(MDefinition* left, MDefinition* right, Operation op, MIRType opType)
      : MBinaryInstruction(left, right), operation_(op)
    {
        setResultType(MIRType_Int32x4);
        specialization_ = opType;
        setMovable();
        if (op == equal || op == notEqual)
            setCommutative();
    }

  public:
    INSTRUCTION_HEADER(SimdBinaryComp)
    static MSimdBinaryComp* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                                     Operation op)
    {
        MOZ_ASSERT(IsSimdType(left->type()));
        MOZ_ASSERT(left->type() == right->type());
        return new(alloc) MSimdBinaryComp(left, right, op, left->type());
    }

    static MSimdBinaryComp* New(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                                Operation op, MIRType opType)
    {
        return new(alloc) MSimdBinaryComp(left, right, op, opType);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    Operation operation() const { return operation_; }
    MIRType specialization() const { return specialization_; }

    // Swap the operands and reverse the comparison predicate.
    void reverse() {
        switch (operation()) {
          case greaterThan:        operation_ = lessThan; break;
          case greaterThanOrEqual: operation_ = lessThanOrEqual; break;
          case lessThan:           operation_ = greaterThan; break;
          case lessThanOrEqual:    operation_ = greaterThanOrEqual; break;
          case equal:
          case notEqual:
            break;
          default: MOZ_CRASH("Unexpected compare operation");
        }
        swapOperands();
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!binaryCongruentTo(ins))
            return false;
        const MSimdBinaryComp* other = ins->toSimdBinaryComp();
        return specialization_ == other->specialization() &&
               operation_ == other->operation();
    }

    void printOpcode(GenericPrinter& out) const override;

    ALLOW_CLONE(MSimdBinaryComp)
};

class MSimdBinaryArith
  : public MBinaryInstruction,
    public MixPolicy<SimdSameAsReturnedTypePolicy<0>, SimdSameAsReturnedTypePolicy<1> >::Data
{
  public:
    enum Operation {
#define OP_LIST_(OP) Op_##OP,
        ARITH_COMMONX4_SIMD_OP(OP_LIST_)
        BINARY_ARITH_FLOAT32X4_SIMD_OP(OP_LIST_)
#undef OP_LIST_
    };

    static const char* OperationName(Operation op) {
        switch (op) {
#define OP_CASE_LIST_(OP) case Op_##OP: return #OP;
          ARITH_COMMONX4_SIMD_OP(OP_CASE_LIST_)
          BINARY_ARITH_FLOAT32X4_SIMD_OP(OP_CASE_LIST_)
#undef OP_CASE_LIST_
        }
        MOZ_CRASH("unexpected operation");
    }

  private:
    Operation operation_;

    MSimdBinaryArith(MDefinition* left, MDefinition* right, Operation op, MIRType type)
      : MBinaryInstruction(left, right), operation_(op)
    {
        MOZ_ASSERT_IF(type == MIRType_Int32x4, op == Op_add || op == Op_sub || op == Op_mul);
        MOZ_ASSERT(IsSimdType(type));
        setResultType(type);
        setMovable();
        if (op == Op_add || op == Op_mul || op == Op_min || op == Op_max)
            setCommutative();
    }

  public:
    INSTRUCTION_HEADER(SimdBinaryArith)
    static MSimdBinaryArith* New(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                                 Operation op, MIRType t)
    {
        return new(alloc) MSimdBinaryArith(left, right, op, t);
    }

    static MSimdBinaryArith* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                                      Operation op, MIRType t)
    {
        MOZ_ASSERT(left->type() == right->type());
        MOZ_ASSERT(left->type() == t);
        return New(alloc, left, right, op, t);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    Operation operation() const { return operation_; }

    bool congruentTo(const MDefinition* ins) const override {
        if (!binaryCongruentTo(ins))
            return false;
        return operation_ == ins->toSimdBinaryArith()->operation();
    }

    void printOpcode(GenericPrinter& out) const override;

    ALLOW_CLONE(MSimdBinaryArith)
};

class MSimdBinaryBitwise
  : public MBinaryInstruction,
    public MixPolicy<SimdSameAsReturnedTypePolicy<0>, SimdSameAsReturnedTypePolicy<1> >::Data
{
  public:
    enum Operation {
        and_,
        or_,
        xor_
    };

    static const char* OperationName(Operation op) {
        switch (op) {
          case and_: return "and";
          case or_:  return "or";
          case xor_: return "xor";
        }
        MOZ_CRASH("unexpected operation");
    }

  private:
    Operation operation_;

    MSimdBinaryBitwise(MDefinition* left, MDefinition* right, Operation op, MIRType type)
      : MBinaryInstruction(left, right), operation_(op)
    {
        MOZ_ASSERT(IsSimdType(type));
        setResultType(type);
        setMovable();
        setCommutative();
    }

  public:
    INSTRUCTION_HEADER(SimdBinaryBitwise)
    static MSimdBinaryBitwise* New(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                                   Operation op, MIRType t)
    {
        return new(alloc) MSimdBinaryBitwise(left, right, op, t);
    }

    static MSimdBinaryBitwise* NewAsmJS(TempAllocator& alloc, MDefinition* left,
                                        MDefinition* right, Operation op, MIRType t)
    {
        MOZ_ASSERT(left->type() == right->type());
        MOZ_ASSERT(left->type() == t);
        return new(alloc) MSimdBinaryBitwise(left, right, op, t);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    Operation operation() const { return operation_; }

    bool congruentTo(const MDefinition* ins) const override {
        if (!binaryCongruentTo(ins))
            return false;
        return operation_ == ins->toSimdBinaryBitwise()->operation();
    }

    void printOpcode(GenericPrinter& out) const override;

    ALLOW_CLONE(MSimdBinaryBitwise)
};

class MSimdShift
  : public MBinaryInstruction,
    public MixPolicy<SimdSameAsReturnedTypePolicy<0>, SimdScalarPolicy<1> >::Data
{
  public:
    enum Operation {
        lsh,
        rsh,
        ursh
    };

  private:
    Operation operation_;

    MSimdShift(MDefinition* left, MDefinition* right, Operation op)
      : MBinaryInstruction(left, right), operation_(op)
    {
        setResultType(MIRType_Int32x4);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(SimdShift)
    static MSimdShift* NewAsmJS(TempAllocator& alloc, MDefinition* left,
                                MDefinition* right, Operation op)
    {
        MOZ_ASSERT(left->type() == MIRType_Int32x4 && right->type() == MIRType_Int32);
        return new(alloc) MSimdShift(left, right, op);
    }

    static MSimdShift* New(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                           Operation op, MIRType type)
    {
        MOZ_ASSERT(type == MIRType_Int32x4);
        return new(alloc) MSimdShift(left, right, op);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    Operation operation() const { return operation_; }

    static const char* OperationName(Operation op) {
        switch (op) {
          case lsh:  return "lsh";
          case rsh:  return "rsh-arithmetic";
          case ursh: return "rhs-logical";
        }
        MOZ_CRASH("unexpected operation");
    }

    void printOpcode(GenericPrinter& out) const override;

    bool congruentTo(const MDefinition* ins) const override {
        if (!binaryCongruentTo(ins))
            return false;
        return operation_ == ins->toSimdShift()->operation();
    }

    ALLOW_CLONE(MSimdShift)
};

class MSimdSelect
  : public MTernaryInstruction,
    public SimdSelectPolicy::Data
{
    bool isElementWise_;

    MSimdSelect(MDefinition* mask, MDefinition* lhs, MDefinition* rhs, MIRType type,
                bool isElementWise)
      : MTernaryInstruction(mask, lhs, rhs), isElementWise_(isElementWise)
    {
        MOZ_ASSERT(IsSimdType(type));
        setResultType(type);
        specialization_ = type;
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(SimdSelect)
    static MSimdSelect* NewAsmJS(TempAllocator& alloc, MDefinition* mask, MDefinition* lhs,
                                 MDefinition* rhs, MIRType t, bool isElementWise)
    {
        MOZ_ASSERT(mask->type() == MIRType_Int32x4);
        MOZ_ASSERT(lhs->type() == rhs->type());
        MOZ_ASSERT(lhs->type() == t);
        return new(alloc) MSimdSelect(mask, lhs, rhs, t, isElementWise);
    }

    static MSimdSelect* New(TempAllocator& alloc, MDefinition* mask, MDefinition* lhs,
                            MDefinition* rhs, MIRType t, bool isElementWise)
    {
        return new(alloc) MSimdSelect(mask, lhs, rhs, t, isElementWise);
    }

    MDefinition* mask() const {
        return getOperand(0);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool isElementWise() const {
        return isElementWise_;
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!congruentIfOperandsEqual(ins))
            return false;
        return isElementWise_ == ins->toSimdSelect()->isElementWise();
    }

    ALLOW_CLONE(MSimdSelect)
};

// Deep clone a constant JSObject.
class MCloneLiteral
  : public MUnaryInstruction,
    public ObjectPolicy<0>::Data
{
  protected:
    explicit MCloneLiteral(MDefinition* obj)
      : MUnaryInstruction(obj)
    {
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(CloneLiteral)
    static MCloneLiteral* New(TempAllocator& alloc, MDefinition* obj);
};

class MParameter : public MNullaryInstruction
{
    int32_t index_;

  public:
    static const int32_t THIS_SLOT = -1;

    MParameter(int32_t index, TemporaryTypeSet* types)
      : index_(index)
    {
        setResultType(MIRType_Value);
        setResultTypeSet(types);
    }

  public:
    INSTRUCTION_HEADER(Parameter)
    static MParameter* New(TempAllocator& alloc, int32_t index, TemporaryTypeSet* types);

    int32_t index() const {
        return index_;
    }
    void printOpcode(GenericPrinter& out) const override;

    HashNumber valueHash() const override;
    bool congruentTo(const MDefinition* ins) const override;
};

class MCallee : public MNullaryInstruction
{
  public:
    MCallee()
    {
        setResultType(MIRType_Object);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Callee)

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    static MCallee* New(TempAllocator& alloc) {
        return new(alloc) MCallee();
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MIsConstructing : public MNullaryInstruction
{
  public:
    MIsConstructing() {
        setResultType(MIRType_Boolean);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(IsConstructing)

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    static MIsConstructing* New(TempAllocator& alloc) {
        return new(alloc) MIsConstructing();
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MControlInstruction : public MInstruction
{
  public:
    MControlInstruction()
    { }

    virtual size_t numSuccessors() const = 0;
    virtual MBasicBlock* getSuccessor(size_t i) const = 0;
    virtual void replaceSuccessor(size_t i, MBasicBlock* successor) = 0;

    bool isControlInstruction() const override {
        return true;
    }

    void printOpcode(GenericPrinter& out) const override;
};

class MTableSwitch final
  : public MControlInstruction,
    public NoFloatPolicy<0>::Data
{
    // The successors of the tableswitch
    // - First successor = the default case
    // - Successor 2 and higher = the cases sorted on case index.
    Vector<MBasicBlock*, 0, JitAllocPolicy> successors_;
    Vector<size_t, 0, JitAllocPolicy> cases_;

    // Contains the blocks/cases that still need to get build
    Vector<MBasicBlock*, 0, JitAllocPolicy> blocks_;

    MUse operand_;
    int32_t low_;
    int32_t high_;

    void initOperand(size_t index, MDefinition* operand) {
        MOZ_ASSERT(index == 0);
        operand_.init(operand, this);
    }

    MTableSwitch(TempAllocator& alloc, MDefinition* ins,
                 int32_t low, int32_t high)
      : successors_(alloc),
        cases_(alloc),
        blocks_(alloc),
        low_(low),
        high_(high)
    {
        initOperand(0, ins);
    }

  protected:
    MUse* getUseFor(size_t index) override {
        MOZ_ASSERT(index == 0);
        return &operand_;
    }

    const MUse* getUseFor(size_t index) const override {
        MOZ_ASSERT(index == 0);
        return &operand_;
    }

  public:
    INSTRUCTION_HEADER(TableSwitch)
    static MTableSwitch* New(TempAllocator& alloc, MDefinition* ins, int32_t low, int32_t high);

    size_t numSuccessors() const override {
        return successors_.length();
    }

    bool addSuccessor(MBasicBlock* successor, size_t* index) {
        MOZ_ASSERT(successors_.length() < (size_t)(high_ - low_ + 2));
        MOZ_ASSERT(!successors_.empty());
        *index = successors_.length();
        return successors_.append(successor);
    }

    MBasicBlock* getSuccessor(size_t i) const override {
        MOZ_ASSERT(i < numSuccessors());
        return successors_[i];
    }

    void replaceSuccessor(size_t i, MBasicBlock* successor) override {
        MOZ_ASSERT(i < numSuccessors());
        successors_[i] = successor;
    }

    MBasicBlock** blocks() {
        return &blocks_[0];
    }

    size_t numBlocks() const {
        return blocks_.length();
    }

    int32_t low() const {
        return low_;
    }

    int32_t high() const {
        return high_;
    }

    MBasicBlock* getDefault() const {
        return getSuccessor(0);
    }

    MBasicBlock* getCase(size_t i) const {
        return getSuccessor(cases_[i]);
    }

    size_t numCases() const {
        return high() - low() + 1;
    }

    bool addDefault(MBasicBlock* block, size_t* index = nullptr) {
        MOZ_ASSERT(successors_.empty());
        if (index)
            *index = 0;
        return successors_.append(block);
    }

    bool addCase(size_t successorIndex) {
        return cases_.append(successorIndex);
    }

    MBasicBlock* getBlock(size_t i) const {
        MOZ_ASSERT(i < numBlocks());
        return blocks_[i];
    }

    bool addBlock(MBasicBlock* block) {
        return blocks_.append(block);
    }

    MDefinition* getOperand(size_t index) const override {
        MOZ_ASSERT(index == 0);
        return operand_.producer();
    }

    size_t numOperands() const override {
        return 1;
    }

    size_t indexOf(const MUse* u) const final override {
        MOZ_ASSERT(u == getUseFor(0));
        return 0;
    }

    void replaceOperand(size_t index, MDefinition* operand) final override {
        MOZ_ASSERT(index == 0);
        operand_.replaceProducer(operand);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
};

template <size_t Arity, size_t Successors>
class MAryControlInstruction : public MControlInstruction
{
    mozilla::Array<MUse, Arity> operands_;
    mozilla::Array<MBasicBlock*, Successors> successors_;

  protected:
    void setSuccessor(size_t index, MBasicBlock* successor) {
        successors_[index] = successor;
    }

    MUse* getUseFor(size_t index) final override {
        return &operands_[index];
    }
    const MUse* getUseFor(size_t index) const final override {
        return &operands_[index];
    }
    void initOperand(size_t index, MDefinition* operand) {
        operands_[index].init(operand, this);
    }

  public:
    MDefinition* getOperand(size_t index) const final override {
        return operands_[index].producer();
    }
    size_t numOperands() const final override {
        return Arity;
    }
    size_t indexOf(const MUse* u) const final override {
        MOZ_ASSERT(u >= &operands_[0]);
        MOZ_ASSERT(u <= &operands_[numOperands() - 1]);
        return u - &operands_[0];
    }
    void replaceOperand(size_t index, MDefinition* operand) final override {
        operands_[index].replaceProducer(operand);
    }
    size_t numSuccessors() const final override {
        return Successors;
    }
    MBasicBlock* getSuccessor(size_t i) const final override {
        return successors_[i];
    }
    void replaceSuccessor(size_t i, MBasicBlock* succ) final override {
        successors_[i] = succ;
    }
};

// Jump to the start of another basic block.
class MGoto
  : public MAryControlInstruction<0, 1>,
    public NoTypePolicy::Data
{
    explicit MGoto(MBasicBlock* target) {
        setSuccessor(0, target);
    }

  public:
    INSTRUCTION_HEADER(Goto)
    static MGoto* New(TempAllocator& alloc, MBasicBlock* target);

    MBasicBlock* target() {
        return getSuccessor(0);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

enum BranchDirection {
    FALSE_BRANCH,
    TRUE_BRANCH
};

static inline BranchDirection
NegateBranchDirection(BranchDirection dir)
{
    return (dir == FALSE_BRANCH) ? TRUE_BRANCH : FALSE_BRANCH;
}

// Tests if the input instruction evaluates to true or false, and jumps to the
// start of a corresponding basic block.
class MTest
  : public MAryControlInstruction<1, 2>,
    public TestPolicy::Data
{
    bool operandMightEmulateUndefined_;

    MTest(MDefinition* ins, MBasicBlock* if_true, MBasicBlock* if_false)
      : operandMightEmulateUndefined_(true)
    {
        initOperand(0, ins);
        setSuccessor(0, if_true);
        setSuccessor(1, if_false);
    }

  public:
    INSTRUCTION_HEADER(Test)
    static MTest* New(TempAllocator& alloc, MDefinition* ins,
                      MBasicBlock* ifTrue, MBasicBlock* ifFalse);

    MDefinition* input() const {
        return getOperand(0);
    }
    MBasicBlock* ifTrue() const {
        return getSuccessor(0);
    }
    MBasicBlock* ifFalse() const {
        return getSuccessor(1);
    }
    MBasicBlock* branchSuccessor(BranchDirection dir) const {
        return (dir == TRUE_BRANCH) ? ifTrue() : ifFalse();
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    // We cache whether our operand might emulate undefined, but we don't want
    // to do that from New() or the constructor, since those can be called on
    // background threads.  So make callers explicitly call it if they want us
    // to check whether the operand might do this.  If this method is never
    // called, we'll assume our operand can emulate undefined.
    void cacheOperandMightEmulateUndefined(CompilerConstraintList* constraints);
    MDefinition* foldsTo(TempAllocator& alloc) override;
    void filtersUndefinedOrNull(bool trueBranch, MDefinition** subject, bool* filtersUndefined,
                                bool* filtersNull);

    void markNoOperandEmulatesUndefined() {
        operandMightEmulateUndefined_ = false;
    }
    bool operandMightEmulateUndefined() const {
        return operandMightEmulateUndefined_;
    }
#ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override {
        return true;
    }
#endif
};

// Equivalent to MTest(true, successor, fake), except without the foldsTo
// method. This allows IonBuilder to insert fake CFG edges to magically protect
// control flow for try-catch blocks.
class MGotoWithFake
  : public MAryControlInstruction<0, 2>,
    public NoTypePolicy::Data
{
    MGotoWithFake(MBasicBlock* successor, MBasicBlock* fake)
    {
        setSuccessor(0, successor);
        setSuccessor(1, fake);
    }

  public:
    INSTRUCTION_HEADER(GotoWithFake)
    static MGotoWithFake* New(TempAllocator& alloc, MBasicBlock* successor, MBasicBlock* fake) {
        return new(alloc) MGotoWithFake(successor, fake);
    }

    MBasicBlock* target() const {
        return getSuccessor(0);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// Returns from this function to the previous caller.
class MReturn
  : public MAryControlInstruction<1, 0>,
    public BoxInputsPolicy::Data
{
    explicit MReturn(MDefinition* ins) {
        initOperand(0, ins);
    }

  public:
    INSTRUCTION_HEADER(Return)
    static MReturn* New(TempAllocator& alloc, MDefinition* ins) {
        return new(alloc) MReturn(ins);
    }

    MDefinition* input() const {
        return getOperand(0);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MThrow
  : public MAryControlInstruction<1, 0>,
    public BoxInputsPolicy::Data
{
    explicit MThrow(MDefinition* ins) {
        initOperand(0, ins);
    }

  public:
    INSTRUCTION_HEADER(Throw)
    static MThrow* New(TempAllocator& alloc, MDefinition* ins) {
        return new(alloc) MThrow(ins);
    }

    virtual AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool possiblyCalls() const override {
        return true;
    }
};

// Fabricate a type set containing only the type of the specified object.
TemporaryTypeSet*
MakeSingletonTypeSet(CompilerConstraintList* constraints, JSObject* obj);

TemporaryTypeSet*
MakeSingletonTypeSet(CompilerConstraintList* constraints, ObjectGroup* obj);

bool
MergeTypes(MIRType* ptype, TemporaryTypeSet** ptypeSet,
           MIRType newType, TemporaryTypeSet* newTypeSet);

bool
TypeSetIncludes(TypeSet* types, MIRType input, TypeSet* inputTypes);

bool
EqualTypes(MIRType type1, TemporaryTypeSet* typeset1,
           MIRType type2, TemporaryTypeSet* typeset2);

bool
CanStoreUnboxedType(TempAllocator& alloc,
                    JSValueType unboxedType, MIRType input, TypeSet* inputTypes);

#ifdef DEBUG
bool
IonCompilationCanUseNurseryPointers();
#endif

// Helper class to check that GC pointers embedded in MIR instructions are in
// in the nursery only when the store buffer has been marked as needing to
// cancel all ion compilations. Otherwise, off-thread Ion compilation and
// nursery GCs can happen in parallel, so it's invalid to store pointers to
// nursery things. There's no need to root these pointers, as GC is suppressed
// during compilation and off-thread compilations are canceled on major GCs.
template <typename T>
class CompilerGCPointer
{
    js::gc::Cell* ptr_;

  public:
    explicit CompilerGCPointer(T ptr)
      : ptr_(ptr)
    {
        MOZ_ASSERT_IF(IsInsideNursery(ptr), IonCompilationCanUseNurseryPointers());
#ifdef DEBUG
        PerThreadData* pt = TlsPerThreadData.get();
        MOZ_ASSERT_IF(pt->runtimeIfOnOwnerThread(), pt->suppressGC);
#endif
    }

    operator T() const { return static_cast<T>(ptr_); }
    T operator->() const { return static_cast<T>(ptr_); }

  private:
    CompilerGCPointer() = delete;
    CompilerGCPointer(const CompilerGCPointer<T>&) = delete;
    CompilerGCPointer<T>& operator=(const CompilerGCPointer<T>&) = delete;
};

typedef CompilerGCPointer<JSObject*> CompilerObject;
typedef CompilerGCPointer<NativeObject*> CompilerNativeObject;
typedef CompilerGCPointer<JSFunction*> CompilerFunction;
typedef CompilerGCPointer<JSScript*> CompilerScript;
typedef CompilerGCPointer<PropertyName*> CompilerPropertyName;
typedef CompilerGCPointer<Shape*> CompilerShape;
typedef CompilerGCPointer<ObjectGroup*> CompilerObjectGroup;

class MNewArray
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  private:
    // Number of elements to allocate for the array.
    uint32_t length_;

    // Heap where the array should be allocated.
    gc::InitialHeap initialHeap_;

    // Whether values written to this array should be converted to double first.
    bool convertDoubleElements_;

    jsbytecode* pc_;

    MNewArray(CompilerConstraintList* constraints, uint32_t length, MConstant* templateConst,
              gc::InitialHeap initialHeap, jsbytecode* pc);

  public:
    INSTRUCTION_HEADER(NewArray)

    static MNewArray* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                          uint32_t length, MConstant* templateConst,
                          gc::InitialHeap initialHeap, jsbytecode* pc)
    {
        return new(alloc) MNewArray(constraints, length, templateConst, initialHeap, pc);
    }

    uint32_t length() const {
        return length_;
    }

    JSObject* templateObject() const {
        return getOperand(0)->toConstant()->value().toObjectOrNull();
    }

    gc::InitialHeap initialHeap() const {
        return initialHeap_;
    }

    jsbytecode* pc() const {
        return pc_;
    }

    bool convertDoubleElements() const {
        return convertDoubleElements_;
    }

    // Returns true if the code generator should call through to the
    // VM rather than the fast path.
    bool shouldUseVM() const;

    // NewArray is marked as non-effectful because all our allocations are
    // either lazy when we are using "new Array(length)" or bounded by the
    // script or the stack size when we are using "new Array(...)" or "[...]"
    // notations.  So we might have to allocate the array twice if we bail
    // during the computation of the first element of the square braket
    // notation.
    virtual AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        // The template object can safely be used in the recover instruction
        // because it can never be mutated by any other function execution.
        return templateObject() != nullptr;
    }
};

class MNewArrayCopyOnWrite : public MNullaryInstruction
{
    CompilerGCPointer<ArrayObject*> templateObject_;
    gc::InitialHeap initialHeap_;

    MNewArrayCopyOnWrite(CompilerConstraintList* constraints, ArrayObject* templateObject,
              gc::InitialHeap initialHeap)
      : templateObject_(templateObject),
        initialHeap_(initialHeap)
    {
        MOZ_ASSERT(!templateObject->isSingleton());
        setResultType(MIRType_Object);
        setResultTypeSet(MakeSingletonTypeSet(constraints, templateObject));
    }

  public:
    INSTRUCTION_HEADER(NewArrayCopyOnWrite)

    static MNewArrayCopyOnWrite* New(TempAllocator& alloc,
                                     CompilerConstraintList* constraints,
                                     ArrayObject* templateObject,
                                     gc::InitialHeap initialHeap)
    {
        return new(alloc) MNewArrayCopyOnWrite(constraints, templateObject, initialHeap);
    }

    ArrayObject* templateObject() const {
        return templateObject_;
    }

    gc::InitialHeap initialHeap() const {
        return initialHeap_;
    }

    virtual AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MNewArrayDynamicLength
  : public MUnaryInstruction,
    public IntPolicy<0>::Data
{
    CompilerObject templateObject_;
    gc::InitialHeap initialHeap_;

    MNewArrayDynamicLength(CompilerConstraintList* constraints, JSObject* templateObject,
                           gc::InitialHeap initialHeap, MDefinition* length)
      : MUnaryInstruction(length),
        templateObject_(templateObject),
        initialHeap_(initialHeap)
    {
        setGuard(); // Need to throw if length is negative.
        setResultType(MIRType_Object);
        if (!templateObject->isSingleton())
            setResultTypeSet(MakeSingletonTypeSet(constraints, templateObject));
    }

  public:
    INSTRUCTION_HEADER(NewArrayDynamicLength)

    static MNewArrayDynamicLength* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                                       JSObject* templateObject, gc::InitialHeap initialHeap,
                                       MDefinition* length)
    {
        return new(alloc) MNewArrayDynamicLength(constraints, templateObject, initialHeap, length);
    }

    MDefinition* length() const {
        return getOperand(0);
    }
    JSObject* templateObject() const {
        return templateObject_;
    }
    gc::InitialHeap initialHeap() const {
        return initialHeap_;
    }

    virtual AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MNewObject
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  public:
    enum Mode { ObjectLiteral, ObjectCreate };

  private:
    gc::InitialHeap initialHeap_;
    Mode mode_;

    MNewObject(CompilerConstraintList* constraints, MConstant* templateConst,
               gc::InitialHeap initialHeap, Mode mode)
      : MUnaryInstruction(templateConst),
        initialHeap_(initialHeap),
        mode_(mode)
    {
        MOZ_ASSERT_IF(mode != ObjectLiteral, !shouldUseVM());
        setResultType(MIRType_Object);

        if (JSObject* obj = templateObject())
            setResultTypeSet(MakeSingletonTypeSet(constraints, obj));

        // The constant is kept separated in a MConstant, this way we can safely
        // mark it during GC if we recover the object allocation.  Otherwise, by
        // making it emittedAtUses, we do not produce register allocations for
        // it and inline its content inside the code produced by the
        // CodeGenerator.
        if (templateConst->toConstant()->value().isObject())
            templateConst->setEmittedAtUses();
    }

  public:
    INSTRUCTION_HEADER(NewObject)

    static MNewObject* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                           MConstant* templateConst, gc::InitialHeap initialHeap,
                           Mode mode)
    {
        return new(alloc) MNewObject(constraints, templateConst, initialHeap, mode);
    }

    // Returns true if the code generator should call through to the
    // VM rather than the fast path.
    bool shouldUseVM() const;

    Mode mode() const {
        return mode_;
    }

    JSObject* templateObject() const {
        return getOperand(0)->toConstant()->value().toObjectOrNull();
    }

    gc::InitialHeap initialHeap() const {
        return initialHeap_;
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        // The template object can safely be used in the recover instruction
        // because it can never be mutated by any other function execution.
        return templateObject() != nullptr;
    }
};

class MNewTypedObject : public MNullaryInstruction
{
    CompilerGCPointer<InlineTypedObject*> templateObject_;
    gc::InitialHeap initialHeap_;

    MNewTypedObject(CompilerConstraintList* constraints,
                    InlineTypedObject* templateObject,
                    gc::InitialHeap initialHeap)
      : templateObject_(templateObject),
        initialHeap_(initialHeap)
    {
        setResultType(MIRType_Object);
        setResultTypeSet(MakeSingletonTypeSet(constraints, templateObject));
    }

  public:
    INSTRUCTION_HEADER(NewTypedObject)

    static MNewTypedObject* New(TempAllocator& alloc,
                                CompilerConstraintList* constraints,
                                InlineTypedObject* templateObject,
                                gc::InitialHeap initialHeap)
    {
        return new(alloc) MNewTypedObject(constraints, templateObject, initialHeap);
    }

    InlineTypedObject* templateObject() const {
        return templateObject_;
    }

    gc::InitialHeap initialHeap() const {
        return initialHeap_;
    }

    virtual AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MTypedObjectDescr
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
  private:
    explicit MTypedObjectDescr(MDefinition* object)
      : MUnaryInstruction(object)
    {
        setResultType(MIRType_Object);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(TypedObjectDescr)

    static MTypedObjectDescr* New(TempAllocator& alloc, MDefinition* object) {
        return new(alloc) MTypedObjectDescr(object);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
};

// Generic way for constructing a SIMD object in IonMonkey, this instruction
// takes as argument a SIMD instruction and returns a new SIMD object which
// corresponds to the MIRType of its operand.
class MSimdBox
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  protected:
    CompilerGCPointer<InlineTypedObject*> templateObject_;
    gc::InitialHeap initialHeap_;

    MSimdBox(CompilerConstraintList* constraints,
             MDefinition* op,
             InlineTypedObject* templateObject,
             gc::InitialHeap initialHeap)
      : MUnaryInstruction(op),
        templateObject_(templateObject),
        initialHeap_(initialHeap)
    {
        MOZ_ASSERT(IsSimdType(op->type()));
        setMovable();
        setResultType(MIRType_Object);
        if (constraints)
            setResultTypeSet(MakeSingletonTypeSet(constraints, templateObject));
    }

  public:
    INSTRUCTION_HEADER(SimdBox)

    static MSimdBox* New(TempAllocator& alloc,
                         CompilerConstraintList* constraints,
                         MDefinition* op,
                         InlineTypedObject* templateObject,
                         gc::InitialHeap initialHeap)
    {
        return new(alloc) MSimdBox(constraints, op, templateObject, initialHeap);
    }

    InlineTypedObject* templateObject() const {
        return templateObject_;
    }

    gc::InitialHeap initialHeap() const {
        return initialHeap_;
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (congruentIfOperandsEqual(ins)) {
            MOZ_ASSERT(ins->toSimdBox()->initialHeap() == initialHeap());
            return true;
        }

        return false;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }
};

class MSimdUnbox
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
  protected:
    MSimdUnbox(MDefinition* op, MIRType type)
      : MUnaryInstruction(op)
    {
        MOZ_ASSERT(IsSimdType(type));
        setGuard();
        setMovable();
        setResultType(type);
    }

  public:
    INSTRUCTION_HEADER(SimdUnbox)
    ALLOW_CLONE(MSimdUnbox)

    static MSimdUnbox* New(TempAllocator& alloc, MDefinition* op, MIRType type)
    {
        return new(alloc) MSimdUnbox(op, type);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// Creates a new derived type object. At runtime, this is just a call
// to `BinaryBlock::createDerived()`. That is, the MIR itself does not
// compile to particularly optimized code. However, using a distinct
// MIR for creating derived type objects allows the compiler to
// optimize ephemeral typed objects as would be created for a
// reference like `a.b.c` -- here, the `a.b` will create an ephemeral
// derived type object that aliases the memory of `a` itself. The
// specific nature of `a.b` is revealed by using
// `MNewDerivedTypedObject` rather than `MGetProperty` or what have
// you. Moreover, the compiler knows that there are no side-effects,
// so `MNewDerivedTypedObject` instructions can be reordered or pruned
// as dead code.
class MNewDerivedTypedObject
  : public MTernaryInstruction,
    public Mix3Policy<ObjectPolicy<0>,
                      ObjectPolicy<1>,
                      IntPolicy<2> >::Data
{
  private:
    TypedObjectPrediction prediction_;

    MNewDerivedTypedObject(TypedObjectPrediction prediction,
                           MDefinition* type,
                           MDefinition* owner,
                           MDefinition* offset)
      : MTernaryInstruction(type, owner, offset),
        prediction_(prediction)
    {
        setMovable();
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(NewDerivedTypedObject)

    static MNewDerivedTypedObject* New(TempAllocator& alloc, TypedObjectPrediction prediction,
                                       MDefinition* type, MDefinition* owner, MDefinition* offset)
    {
        return new(alloc) MNewDerivedTypedObject(prediction, type, owner, offset);
    }

    TypedObjectPrediction prediction() const {
        return prediction_;
    }

    MDefinition* type() const {
        return getOperand(0);
    }

    MDefinition* owner() const {
        return getOperand(1);
    }

    MDefinition* offset() const {
        return getOperand(2);
    }

    virtual AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }
};

// This vector is used when the recovered object is kept unboxed. We map the
// offset of each property to the index of the corresponding operands in the
// object state.
struct OperandIndexMap : public TempObject
{
    // The number of properties is limited by scalar replacement. Thus we cannot
    // have any large number of properties.
    FixedList<uint8_t> map;

    bool init(TempAllocator& alloc, JSObject* templateObject);
};

// Represent the content of all slots of an object.  This instruction is not
// lowered and is not used to generate code.
class MObjectState
  : public MVariadicInstruction,
    public NoFloatPolicyAfter<1>::Data
{
  private:
    uint32_t numSlots_;
    uint32_t numFixedSlots_;        // valid if isUnboxed() == false.
    OperandIndexMap* operandIndex_; // valid if isUnboxed() == true.

    bool isUnboxed() const {
        return operandIndex_ != nullptr;
    }

    MObjectState(JSObject *templateObject, OperandIndexMap* operandIndex);
    explicit MObjectState(MObjectState* state);

    bool init(TempAllocator& alloc, MDefinition* obj);

    void initSlot(uint32_t slot, MDefinition* def) {
        initOperand(slot + 1, def);
    }

  public:
    INSTRUCTION_HEADER(ObjectState)

    // Return the template object of any object creation which can be recovered
    // on bailout.
    static JSObject* templateObjectOf(MDefinition* obj);

    static MObjectState* New(TempAllocator& alloc, MDefinition* obj, MDefinition* undefinedVal);
    static MObjectState* Copy(TempAllocator& alloc, MObjectState* state);

    MDefinition* object() const {
        return getOperand(0);
    }

    size_t numFixedSlots() const {
        MOZ_ASSERT(!isUnboxed());
        return numFixedSlots_;
    }
    size_t numSlots() const {
        return numSlots_;
    }

    MDefinition* getSlot(uint32_t slot) const {
        return getOperand(slot + 1);
    }
    void setSlot(uint32_t slot, MDefinition* def) {
        replaceOperand(slot + 1, def);
    }

    bool hasFixedSlot(uint32_t slot) const {
        return slot < numSlots() && slot < numFixedSlots();
    }
    MDefinition* getFixedSlot(uint32_t slot) const {
        MOZ_ASSERT(slot < numFixedSlots());
        return getSlot(slot);
    }
    void setFixedSlot(uint32_t slot, MDefinition* def) {
        MOZ_ASSERT(slot < numFixedSlots());
        setSlot(slot, def);
    }

    bool hasDynamicSlot(uint32_t slot) const {
        return numFixedSlots() < numSlots() && slot < numSlots() - numFixedSlots();
    }
    MDefinition* getDynamicSlot(uint32_t slot) const {
        return getSlot(slot + numFixedSlots());
    }
    void setDynamicSlot(uint32_t slot, MDefinition* def) {
        setSlot(slot + numFixedSlots(), def);
    }

    // Interface reserved for unboxed objects.
    bool hasOffset(uint32_t offset) const {
        MOZ_ASSERT(isUnboxed());
        return offset < operandIndex_->map.length() && operandIndex_->map[offset] != 0;
    }
    MDefinition* getOffset(uint32_t offset) const {
        return getOperand(operandIndex_->map[offset]);
    }
    void setOffset(uint32_t offset, MDefinition* def) {
        replaceOperand(operandIndex_->map[offset], def);
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }
};

// Represent the contents of all elements of an array.  This instruction is not
// lowered and is not used to generate code.
class MArrayState
  : public MVariadicInstruction,
    public NoFloatPolicyAfter<2>::Data
{
  private:
    uint32_t numElements_;

    explicit MArrayState(MDefinition* arr);

    bool init(TempAllocator& alloc, MDefinition* obj, MDefinition* len);

    void initElement(uint32_t index, MDefinition* def) {
        initOperand(index + 2, def);
    }

  public:
    INSTRUCTION_HEADER(ArrayState)

    static MArrayState* New(TempAllocator& alloc, MDefinition* arr, MDefinition* undefinedVal,
                            MDefinition* initLength);
    static MArrayState* Copy(TempAllocator& alloc, MArrayState* state);

    MDefinition* array() const {
        return getOperand(0);
    }

    MDefinition* initializedLength() const {
        return getOperand(1);
    }
    void setInitializedLength(MDefinition* def) {
        replaceOperand(1, def);
    }


    size_t numElements() const {
        return numElements_;
    }

    MDefinition* getElement(uint32_t index) const {
        return getOperand(index + 2);
    }
    void setElement(uint32_t index, MDefinition* def) {
        replaceOperand(index + 2, def);
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }
};

// Setting __proto__ in an object literal.
class MMutateProto
  : public MAryInstruction<2>,
    public MixPolicy<ObjectPolicy<0>, BoxPolicy<1> >::Data
{
  protected:
    MMutateProto(MDefinition* obj, MDefinition* value)
    {
        initOperand(0, obj);
        initOperand(1, value);
        setResultType(MIRType_None);
    }

  public:
    INSTRUCTION_HEADER(MutateProto)

    static MMutateProto* New(TempAllocator& alloc, MDefinition* obj, MDefinition* value)
    {
        return new(alloc) MMutateProto(obj, value);
    }

    MDefinition* getObject() const {
        return getOperand(0);
    }
    MDefinition* getValue() const {
        return getOperand(1);
    }

    bool possiblyCalls() const override {
        return true;
    }
};

// Slow path for adding a property to an object without a known base.
class MInitProp
  : public MAryInstruction<2>,
    public MixPolicy<ObjectPolicy<0>, BoxPolicy<1> >::Data
{
    CompilerPropertyName name_;

  protected:
    MInitProp(MDefinition* obj, PropertyName* name, MDefinition* value)
      : name_(name)
    {
        initOperand(0, obj);
        initOperand(1, value);
        setResultType(MIRType_None);
    }

  public:
    INSTRUCTION_HEADER(InitProp)

    static MInitProp* New(TempAllocator& alloc, MDefinition* obj, PropertyName* name,
                          MDefinition* value)
    {
        return new(alloc) MInitProp(obj, name, value);
    }

    MDefinition* getObject() const {
        return getOperand(0);
    }
    MDefinition* getValue() const {
        return getOperand(1);
    }

    PropertyName* propertyName() const {
        return name_;
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MInitPropGetterSetter
  : public MBinaryInstruction,
    public MixPolicy<ObjectPolicy<0>, ObjectPolicy<1> >::Data
{
    CompilerPropertyName name_;

    MInitPropGetterSetter(MDefinition* obj, PropertyName* name, MDefinition* value)
      : MBinaryInstruction(obj, value),
        name_(name)
    { }

  public:
    INSTRUCTION_HEADER(InitPropGetterSetter)

    static MInitPropGetterSetter* New(TempAllocator& alloc, MDefinition* obj, PropertyName* name,
                                      MDefinition* value)
    {
        return new(alloc) MInitPropGetterSetter(obj, name, value);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* value() const {
        return getOperand(1);
    }
    PropertyName* name() const {
        return name_;
    }
};

class MInitElem
  : public MAryInstruction<3>,
    public Mix3Policy<ObjectPolicy<0>, BoxPolicy<1>, BoxPolicy<2> >::Data
{
    MInitElem(MDefinition* obj, MDefinition* id, MDefinition* value)
    {
        initOperand(0, obj);
        initOperand(1, id);
        initOperand(2, value);
        setResultType(MIRType_None);
    }

  public:
    INSTRUCTION_HEADER(InitElem)

    static MInitElem* New(TempAllocator& alloc, MDefinition* obj, MDefinition* id,
                          MDefinition* value)
    {
        return new(alloc) MInitElem(obj, id, value);
    }

    MDefinition* getObject() const {
        return getOperand(0);
    }
    MDefinition* getId() const {
        return getOperand(1);
    }
    MDefinition* getValue() const {
        return getOperand(2);
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MInitElemGetterSetter
  : public MTernaryInstruction,
    public Mix3Policy<ObjectPolicy<0>, BoxPolicy<1>, ObjectPolicy<2> >::Data
{
    MInitElemGetterSetter(MDefinition* obj, MDefinition* id, MDefinition* value)
      : MTernaryInstruction(obj, id, value)
    { }

  public:
    INSTRUCTION_HEADER(InitElemGetterSetter)

    static MInitElemGetterSetter* New(TempAllocator& alloc, MDefinition* obj, MDefinition* id,
                                      MDefinition* value)
    {
        return new(alloc) MInitElemGetterSetter(obj, id, value);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* idValue() const {
        return getOperand(1);
    }
    MDefinition* value() const {
        return getOperand(2);
    }
};

class MCall
  : public MVariadicInstruction,
    public CallPolicy::Data
{
  private:
    // An MCall uses the MPrepareCall, MDefinition for the function, and
    // MPassArg instructions. They are stored in the same list.
    static const size_t FunctionOperandIndex   = 0;
    static const size_t NumNonArgumentOperands = 1;

  protected:
    // Monomorphic cache of single target from TI, or nullptr.
    CompilerFunction target_;

    // Original value of argc from the bytecode.
    uint32_t numActualArgs_;

    // True if the call is for JSOP_NEW.
    bool construct_;

    bool needsArgCheck_;

    MCall(JSFunction* target, uint32_t numActualArgs, bool construct)
      : target_(target),
        numActualArgs_(numActualArgs),
        construct_(construct),
        needsArgCheck_(true)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(Call)
    static MCall* New(TempAllocator& alloc, JSFunction* target, size_t maxArgc, size_t numActualArgs,
                      bool construct, bool isDOMCall);

    void initFunction(MDefinition* func) {
        initOperand(FunctionOperandIndex, func);
    }

    bool needsArgCheck() const {
        return needsArgCheck_;
    }

    void disableArgCheck() {
        needsArgCheck_ = false;
    }
    MDefinition* getFunction() const {
        return getOperand(FunctionOperandIndex);
    }
    void replaceFunction(MInstruction* newfunc) {
        replaceOperand(FunctionOperandIndex, newfunc);
    }

    void addArg(size_t argnum, MDefinition* arg);

    MDefinition* getArg(uint32_t index) const {
        return getOperand(NumNonArgumentOperands + index);
    }

    static size_t IndexOfThis() {
        return NumNonArgumentOperands;
    }
    static size_t IndexOfArgument(size_t index) {
        return NumNonArgumentOperands + index + 1; // +1 to skip |this|.
    }
    static size_t IndexOfStackArg(size_t index) {
        return NumNonArgumentOperands + index;
    }

    // For TI-informed monomorphic callsites.
    JSFunction* getSingleTarget() const {
        return target_;
    }

    bool isConstructing() const {
        return construct_;
    }

    // The number of stack arguments is the max between the number of formal
    // arguments and the number of actual arguments. The number of stack
    // argument includes the |undefined| padding added in case of underflow.
    // Includes |this|.
    uint32_t numStackArgs() const {
        return numOperands() - NumNonArgumentOperands;
    }

    // Does not include |this|.
    uint32_t numActualArgs() const {
        return numActualArgs_;
    }

    bool possiblyCalls() const override {
        return true;
    }

    virtual bool isCallDOMNative() const {
        return false;
    }

    // A method that can be called to tell the MCall to figure out whether it's
    // movable or not.  This can't be done in the constructor, because it
    // depends on the arguments to the call, and those aren't passed to the
    // constructor but are set up later via addArg.
    virtual void computeMovable() {
    }
};

class MCallDOMNative : public MCall
{
    // A helper class for MCalls for DOM natives.  Note that this is NOT
    // actually a separate MIR op from MCall, because all sorts of places use
    // isCall() to check for calls and all we really want is to overload a few
    // virtual things from MCall.
  protected:
    MCallDOMNative(JSFunction* target, uint32_t numActualArgs)
        : MCall(target, numActualArgs, false)
    {
        MOZ_ASSERT(getJitInfo()->type() != JSJitInfo::InlinableNative);

        // If our jitinfo is not marked eliminatable, that means that our C++
        // implementation is fallible or that it never wants to be eliminated or
        // that we have no hope of ever doing the sort of argument analysis that
        // would allow us to detemine that we're side-effect-free.  In the
        // latter case we wouldn't get DCEd no matter what, but for the former
        // two cases we have to explicitly say that we can't be DCEd.
        if (!getJitInfo()->isEliminatable)
            setGuard();
    }

    friend MCall* MCall::New(TempAllocator& alloc, JSFunction* target, size_t maxArgc,
                             size_t numActualArgs, bool construct, bool isDOMCall);

    const JSJitInfo* getJitInfo() const;
  public:
    virtual AliasSet getAliasSet() const override;

    virtual bool congruentTo(const MDefinition* ins) const override;

    virtual bool isCallDOMNative() const override {
        return true;
    }

    virtual void computeMovable() override;
};

// arr.splice(start, deleteCount) with unused return value.
class MArraySplice
  : public MTernaryInstruction,
    public Mix3Policy<ObjectPolicy<0>, IntPolicy<1>, IntPolicy<2> >::Data
{
  private:

    MArraySplice(MDefinition* object, MDefinition* start, MDefinition* deleteCount)
      : MTernaryInstruction(object, start, deleteCount)
    { }

  public:
    INSTRUCTION_HEADER(ArraySplice)
    static MArraySplice* New(TempAllocator& alloc, MDefinition* object,
                             MDefinition* start, MDefinition* deleteCount)
    {
        return new(alloc) MArraySplice(object, start, deleteCount);
    }

    MDefinition* object() const {
        return getOperand(0);
    }

    MDefinition* start() const {
        return getOperand(1);
    }

    MDefinition* deleteCount() const {
        return getOperand(2);
    }

    bool possiblyCalls() const override {
        return true;
    }
};

// fun.apply(self, arguments)
class MApplyArgs
  : public MAryInstruction<3>,
    public Mix3Policy<ObjectPolicy<0>, IntPolicy<1>, BoxPolicy<2> >::Data
{
  protected:
    // Monomorphic cache of single target from TI, or nullptr.
    CompilerFunction target_;

    MApplyArgs(JSFunction* target, MDefinition* fun, MDefinition* argc, MDefinition* self)
      : target_(target)
    {
        initOperand(0, fun);
        initOperand(1, argc);
        initOperand(2, self);
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(ApplyArgs)
    static MApplyArgs* New(TempAllocator& alloc, JSFunction* target, MDefinition* fun,
                           MDefinition* argc, MDefinition* self);

    MDefinition* getFunction() const {
        return getOperand(0);
    }

    // For TI-informed monomorphic callsites.
    JSFunction* getSingleTarget() const {
        return target_;
    }

    MDefinition* getArgc() const {
        return getOperand(1);
    }
    MDefinition* getThis() const {
        return getOperand(2);
    }
    bool possiblyCalls() const override {
        return true;
    }
};

// fun.apply(fn, array)
class MApplyArray
  : public MAryInstruction<3>,
    public Mix3Policy<ObjectPolicy<0>, ObjectPolicy<1>, BoxPolicy<2> >::Data
{
  protected:
    // Monomorphic cache of single target from TI, or nullptr.
    CompilerFunction target_;

    MApplyArray(JSFunction* target, MDefinition* fun, MDefinition* elements, MDefinition* self)
      : target_(target)
    {
        initOperand(0, fun);
        initOperand(1, elements);
        initOperand(2, self);
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(ApplyArray)
    static MApplyArray* New(TempAllocator& alloc, JSFunction* target, MDefinition* fun,
                            MDefinition* elements, MDefinition* self);

    MDefinition* getFunction() const {
        return getOperand(0);
    }

    // For TI-informed monomorphic callsites.
    JSFunction* getSingleTarget() const {
        return target_;
    }

    MDefinition* getElements() const {
        return getOperand(1);
    }
    MDefinition* getThis() const {
        return getOperand(2);
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MBail : public MNullaryInstruction
{
  protected:
    explicit MBail(BailoutKind kind)
      : MNullaryInstruction()
    {
        bailoutKind_ = kind;
        setGuard();
    }

  private:
    BailoutKind bailoutKind_;

  public:
    INSTRUCTION_HEADER(Bail)

    static MBail*
    New(TempAllocator& alloc, BailoutKind kind) {
        return new(alloc) MBail(kind);
    }
    static MBail*
    New(TempAllocator& alloc) {
        return new(alloc) MBail(Bailout_Inevitable);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    BailoutKind bailoutKind() const {
        return bailoutKind_;
    }
};

class MUnreachable
  : public MAryControlInstruction<0, 0>,
    public NoTypePolicy::Data
{
  public:
    INSTRUCTION_HEADER(Unreachable)

    static MUnreachable* New(TempAllocator& alloc) {
        return new(alloc) MUnreachable();
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// This class serve as a way to force the encoding of a snapshot, even if there
// is no resume point using it.  This is useful to run MAssertRecoveredOnBailout
// assertions.
class MEncodeSnapshot : public MNullaryInstruction
{
  protected:
    MEncodeSnapshot()
      : MNullaryInstruction()
    {
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(EncodeSnapshot)

    static MEncodeSnapshot*
    New(TempAllocator& alloc) {
        return new(alloc) MEncodeSnapshot();
    }
};

class MAssertRecoveredOnBailout
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  protected:
    bool mustBeRecovered_;

    MAssertRecoveredOnBailout(MDefinition* ins, bool mustBeRecovered)
      : MUnaryInstruction(ins), mustBeRecovered_(mustBeRecovered)
    {
        setResultType(MIRType_Value);
        setRecoveredOnBailout();
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(AssertRecoveredOnBailout)

    static MAssertRecoveredOnBailout* New(TempAllocator& alloc, MDefinition* ins,
                                          bool mustBeRecovered)
    {
        return new(alloc) MAssertRecoveredOnBailout(ins, mustBeRecovered);
    }

    // Needed to assert that float32 instructions are correctly recovered.
    bool canConsumeFloat32(MUse* use) const override { return true; }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }
};

class MAssertFloat32
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  protected:
    bool mustBeFloat32_;

    MAssertFloat32(MDefinition* value, bool mustBeFloat32)
      : MUnaryInstruction(value), mustBeFloat32_(mustBeFloat32)
    {
    }

  public:
    INSTRUCTION_HEADER(AssertFloat32)

    static MAssertFloat32* New(TempAllocator& alloc, MDefinition* value, bool mustBeFloat32) {
        return new(alloc) MAssertFloat32(value, mustBeFloat32);
    }

    bool canConsumeFloat32(MUse* use) const override { return true; }

    bool mustBeFloat32() const { return mustBeFloat32_; }
};

class MGetDynamicName
  : public MAryInstruction<2>,
    public MixPolicy<ObjectPolicy<0>, ConvertToStringPolicy<1> >::Data
{
  protected:
    MGetDynamicName(MDefinition* scopeChain, MDefinition* name)
    {
        initOperand(0, scopeChain);
        initOperand(1, name);
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(GetDynamicName)

    static MGetDynamicName*
    New(TempAllocator& alloc, MDefinition* scopeChain, MDefinition* name) {
        return new(alloc) MGetDynamicName(scopeChain, name);
    }

    MDefinition* getScopeChain() const {
        return getOperand(0);
    }
    MDefinition* getName() const {
        return getOperand(1);
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MCallDirectEval
  : public MAryInstruction<3>,
    public Mix3Policy<ObjectPolicy<0>,
                      StringPolicy<1>,
                      BoxPolicy<2> >::Data
{
  protected:
    MCallDirectEval(MDefinition* scopeChain, MDefinition* string,
                    MDefinition* newTargetValue, jsbytecode* pc)
        : pc_(pc)
    {
        initOperand(0, scopeChain);
        initOperand(1, string);
        initOperand(2, newTargetValue);
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(CallDirectEval)

    static MCallDirectEval*
    New(TempAllocator& alloc, MDefinition* scopeChain, MDefinition* string,
        MDefinition* newTargetValue, jsbytecode* pc)
    {
        return new(alloc) MCallDirectEval(scopeChain, string, newTargetValue, pc);
    }

    MDefinition* getScopeChain() const {
        return getOperand(0);
    }
    MDefinition* getString() const {
        return getOperand(1);
    }
    MDefinition* getNewTargetValue() const {
        return getOperand(2);
    }

    jsbytecode* pc() const {
        return pc_;
    }

    bool possiblyCalls() const override {
        return true;
    }

  private:
    jsbytecode* pc_;
};

class MCompare
  : public MBinaryInstruction,
    public ComparePolicy::Data
{
  public:
    enum CompareType {

        // Anything compared to Undefined
        Compare_Undefined,

        // Anything compared to Null
        Compare_Null,

        // Undefined compared to Boolean
        // Null      compared to Boolean
        // Double    compared to Boolean
        // String    compared to Boolean
        // Symbol    compared to Boolean
        // Object    compared to Boolean
        // Value     compared to Boolean
        Compare_Boolean,

        // Int32   compared to Int32
        // Boolean compared to Boolean
        Compare_Int32,
        Compare_Int32MaybeCoerceBoth,
        Compare_Int32MaybeCoerceLHS,
        Compare_Int32MaybeCoerceRHS,

        // Int32 compared as unsigneds
        Compare_UInt32,

        // Double compared to Double
        Compare_Double,

        Compare_DoubleMaybeCoerceLHS,
        Compare_DoubleMaybeCoerceRHS,

        // Float compared to Float
        Compare_Float32,

        // String compared to String
        Compare_String,

        // Undefined compared to String
        // Null      compared to String
        // Boolean   compared to String
        // Int32     compared to String
        // Double    compared to String
        // Object    compared to String
        // Value     compared to String
        Compare_StrictString,

        // Object compared to Object
        Compare_Object,

        // Compare 2 values bitwise
        Compare_Bitwise,

        // All other possible compares
        Compare_Unknown
    };

  private:
    CompareType compareType_;
    JSOp jsop_;
    bool operandMightEmulateUndefined_;
    bool operandsAreNeverNaN_;

    // When a floating-point comparison is converted to an integer comparison
    // (when range analysis proves it safe), we need to convert the operands
    // to integer as well.
    bool truncateOperands_;

    MCompare(MDefinition* left, MDefinition* right, JSOp jsop)
      : MBinaryInstruction(left, right),
        compareType_(Compare_Unknown),
        jsop_(jsop),
        operandMightEmulateUndefined_(true),
        operandsAreNeverNaN_(false),
        truncateOperands_(false)
    {
        setResultType(MIRType_Boolean);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Compare)
    static MCompare* New(TempAllocator& alloc, MDefinition* left, MDefinition* right, JSOp op);
    static MCompare* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right, JSOp op,
                              CompareType compareType);

    bool tryFold(bool* result);
    bool evaluateConstantOperands(TempAllocator& alloc, bool* result);
    MDefinition* foldsTo(TempAllocator& alloc) override;
    void filtersUndefinedOrNull(bool trueBranch, MDefinition** subject, bool* filtersUndefined,
                                bool* filtersNull);

    CompareType compareType() const {
        return compareType_;
    }
    bool isInt32Comparison() const {
        return compareType() == Compare_Int32 ||
               compareType() == Compare_Int32MaybeCoerceBoth ||
               compareType() == Compare_Int32MaybeCoerceLHS ||
               compareType() == Compare_Int32MaybeCoerceRHS;
    }
    bool isDoubleComparison() const {
        return compareType() == Compare_Double ||
               compareType() == Compare_DoubleMaybeCoerceLHS ||
               compareType() == Compare_DoubleMaybeCoerceRHS;
    }
    bool isFloat32Comparison() const {
        return compareType() == Compare_Float32;
    }
    bool isNumericComparison() const {
        return isInt32Comparison() ||
               isDoubleComparison() ||
               isFloat32Comparison();
    }
    void setCompareType(CompareType type) {
        compareType_ = type;
    }
    MIRType inputType();

    JSOp jsop() const {
        return jsop_;
    }
    void markNoOperandEmulatesUndefined() {
        operandMightEmulateUndefined_ = false;
    }
    bool operandMightEmulateUndefined() const {
        return operandMightEmulateUndefined_;
    }
    bool operandsAreNeverNaN() const {
        return operandsAreNeverNaN_;
    }
    AliasSet getAliasSet() const override {
        // Strict equality is never effectful.
        if (jsop_ == JSOP_STRICTEQ || jsop_ == JSOP_STRICTNE)
            return AliasSet::None();
        if (compareType_ == Compare_Unknown)
            return AliasSet::Store(AliasSet::Any);
        MOZ_ASSERT(compareType_ <= Compare_Bitwise);
        return AliasSet::None();
    }

    void printOpcode(GenericPrinter& out) const override;
    void collectRangeInfoPreTrunc() override;

    void trySpecializeFloat32(TempAllocator& alloc) override;
    bool isFloat32Commutative() const override { return true; }
    bool needTruncation(TruncateKind kind) override;
    void truncate() override;
    TruncateKind operandTruncateKind(size_t index) const override;

    static CompareType determineCompareType(JSOp op, MDefinition* left, MDefinition* right);
    void cacheOperandMightEmulateUndefined(CompilerConstraintList* constraints);

# ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override {
        // Both sides of the compare can be Float32
        return compareType_ == Compare_Float32;
    }
# endif

    ALLOW_CLONE(MCompare)

  protected:
    bool tryFoldEqualOperands(bool* result);
    bool tryFoldTypeOf(bool* result);

    bool congruentTo(const MDefinition* ins) const override {
        if (!binaryCongruentTo(ins))
            return false;
        return compareType() == ins->toCompare()->compareType() &&
               jsop() == ins->toCompare()->jsop();
    }
};

// Takes a typed value and returns an untyped value.
class MBox
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    MBox(TempAllocator& alloc, MDefinition* ins)
      : MUnaryInstruction(ins)
    {
        setResultType(MIRType_Value);
        if (ins->resultTypeSet()) {
            setResultTypeSet(ins->resultTypeSet());
        } else if (ins->type() != MIRType_Value) {
            TypeSet::Type ntype = ins->type() == MIRType_Object
                                  ? TypeSet::AnyObjectType()
                                  : TypeSet::PrimitiveType(ValueTypeFromMIRType(ins->type()));
            setResultTypeSet(alloc.lifoAlloc()->new_<TemporaryTypeSet>(alloc.lifoAlloc(), ntype));
        }
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Box)
    static MBox* New(TempAllocator& alloc, MDefinition* ins)
    {
        // Cannot box a box.
        MOZ_ASSERT(ins->type() != MIRType_Value);

        return new(alloc) MBox(alloc, ins);
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    ALLOW_CLONE(MBox)
};

// Note: the op may have been inverted during lowering (to put constants in a
// position where they can be immediates), so it is important to use the
// lir->jsop() instead of the mir->jsop() when it is present.
static inline Assembler::Condition
JSOpToCondition(MCompare::CompareType compareType, JSOp op)
{
    bool isSigned = (compareType != MCompare::Compare_UInt32);
    return JSOpToCondition(op, isSigned);
}

// Takes a typed value and checks if it is a certain type. If so, the payload
// is unpacked and returned as that type. Otherwise, it is considered a
// deoptimization.
class MUnbox final : public MUnaryInstruction, public BoxInputsPolicy::Data
{
  public:
    enum Mode {
        Fallible,       // Check the type, and deoptimize if unexpected.
        Infallible,     // Type guard is not necessary.
        TypeBarrier     // Guard on the type, and act like a TypeBarrier on failure.
    };

  private:
    Mode mode_;
    BailoutKind bailoutKind_;

    MUnbox(MDefinition* ins, MIRType type, Mode mode, BailoutKind kind, TempAllocator& alloc)
      : MUnaryInstruction(ins),
        mode_(mode)
    {
        // Only allow unboxing a non MIRType_Value when input and output types
        // don't match. This is often used to force a bailout. Boxing happens
        // during type analysis.
        MOZ_ASSERT_IF(ins->type() != MIRType_Value, type != ins->type());

        MOZ_ASSERT(type == MIRType_Boolean ||
                   type == MIRType_Int32   ||
                   type == MIRType_Double  ||
                   type == MIRType_String  ||
                   type == MIRType_Symbol  ||
                   type == MIRType_Object);

        TemporaryTypeSet* resultSet = ins->resultTypeSet();
        if (resultSet && type == MIRType_Object)
            resultSet = resultSet->cloneObjectsOnly(alloc.lifoAlloc());

        setResultType(type);
        setResultTypeSet(resultSet);
        setMovable();

        if (mode_ == TypeBarrier || mode_ == Fallible)
            setGuard();

        bailoutKind_ = kind;
    }
  public:
    INSTRUCTION_HEADER(Unbox)
    static MUnbox* New(TempAllocator& alloc, MDefinition* ins, MIRType type, Mode mode)
    {
        // Unless we were given a specific BailoutKind, pick a default based on
        // the type we expect.
        BailoutKind kind;
        switch (type) {
          case MIRType_Boolean:
            kind = Bailout_NonBooleanInput;
            break;
          case MIRType_Int32:
            kind = Bailout_NonInt32Input;
            break;
          case MIRType_Double:
            kind = Bailout_NonNumericInput; // Int32s are fine too
            break;
          case MIRType_String:
            kind = Bailout_NonStringInput;
            break;
          case MIRType_Symbol:
            kind = Bailout_NonSymbolInput;
            break;
          case MIRType_Object:
            kind = Bailout_NonObjectInput;
            break;
          default:
            MOZ_CRASH("Given MIRType cannot be unboxed.");
        }

        return new(alloc) MUnbox(ins, type, mode, kind, alloc);
    }

    static MUnbox* New(TempAllocator& alloc, MDefinition* ins, MIRType type, Mode mode,
                       BailoutKind kind)
    {
        return new(alloc) MUnbox(ins, type, mode, kind, alloc);
    }

    Mode mode() const {
        return mode_;
    }
    BailoutKind bailoutKind() const {
        // If infallible, no bailout should be generated.
        MOZ_ASSERT(fallible());
        return bailoutKind_;
    }
    bool fallible() const {
        return mode() != Infallible;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isUnbox() || ins->toUnbox()->mode() != mode())
            return false;
        return congruentIfOperandsEqual(ins);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    void printOpcode(GenericPrinter& out) const override;
    void makeInfallible() {
        // Should only be called if we're already Infallible or TypeBarrier
        MOZ_ASSERT(mode() != Fallible);
        mode_ = Infallible;
    }

    ALLOW_CLONE(MUnbox)
};

class MGuardObject
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MGuardObject(MDefinition* ins)
      : MUnaryInstruction(ins)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Object);
        setResultTypeSet(ins->resultTypeSet());
    }

  public:
    INSTRUCTION_HEADER(GuardObject)

    static MGuardObject* New(TempAllocator& alloc, MDefinition* ins) {
        return new(alloc) MGuardObject(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MGuardString
  : public MUnaryInstruction,
    public StringPolicy<0>::Data
{
    explicit MGuardString(MDefinition* ins)
      : MUnaryInstruction(ins)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_String);
    }

  public:
    INSTRUCTION_HEADER(GuardString)

    static MGuardString* New(TempAllocator& alloc, MDefinition* ins) {
        return new(alloc) MGuardString(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MPolyInlineGuard
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MPolyInlineGuard(MDefinition* ins)
      : MUnaryInstruction(ins)
    {
        setGuard();
        setResultType(MIRType_Object);
        setResultTypeSet(ins->resultTypeSet());
    }

  public:
    INSTRUCTION_HEADER(PolyInlineGuard)

    static MPolyInlineGuard* New(TempAllocator& alloc, MDefinition* ins) {
        return new(alloc) MPolyInlineGuard(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MAssertRange
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    // This is the range checked by the assertion. Don't confuse this with the
    // range_ member or the range() accessor. Since MAssertRange doesn't return
    // a value, it doesn't use those.
    const Range* assertedRange_;

    MAssertRange(MDefinition* ins, const Range* assertedRange)
      : MUnaryInstruction(ins), assertedRange_(assertedRange)
    {
        setGuard();
        setResultType(MIRType_None);
    }

  public:
    INSTRUCTION_HEADER(AssertRange)

    static MAssertRange* New(TempAllocator& alloc, MDefinition* ins, const Range* assertedRange) {
        return new(alloc) MAssertRange(ins, assertedRange);
    }

    const Range* assertedRange() const {
        return assertedRange_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    void printOpcode(GenericPrinter& out) const override;
};

// Caller-side allocation of |this| for |new|:
// Given a templateobject, construct |this| for JSOP_NEW
class MCreateThisWithTemplate
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    gc::InitialHeap initialHeap_;

    MCreateThisWithTemplate(CompilerConstraintList* constraints, MConstant* templateConst,
                            gc::InitialHeap initialHeap)
      : MUnaryInstruction(templateConst),
        initialHeap_(initialHeap)
    {
        setResultType(MIRType_Object);
        setResultTypeSet(MakeSingletonTypeSet(constraints, templateObject()));
    }

  public:
    INSTRUCTION_HEADER(CreateThisWithTemplate)
    static MCreateThisWithTemplate* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                                        MConstant* templateConst, gc::InitialHeap initialHeap)
    {
        return new(alloc) MCreateThisWithTemplate(constraints, templateConst, initialHeap);
    }

    // Template for |this|, provided by TI.
    JSObject* templateObject() const {
        return &getOperand(0)->toConstant()->value().toObject();
    }

    gc::InitialHeap initialHeap() const {
        return initialHeap_;
    }

    // Although creation of |this| modifies global state, it is safely repeatable.
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override;
};

// Caller-side allocation of |this| for |new|:
// Given a prototype operand, construct |this| for JSOP_NEW.
class MCreateThisWithProto
  : public MTernaryInstruction,
    public Mix3Policy<ObjectPolicy<0>, ObjectPolicy<1>, ObjectPolicy<2> >::Data
{
    MCreateThisWithProto(MDefinition* callee, MDefinition* newTarget, MDefinition* prototype)
      : MTernaryInstruction(callee, newTarget, prototype)
    {
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(CreateThisWithProto)
    static MCreateThisWithProto* New(TempAllocator& alloc, MDefinition* callee,
                                     MDefinition* newTarget, MDefinition* prototype)
    {
        return new(alloc) MCreateThisWithProto(callee, newTarget, prototype);
    }

    MDefinition* getCallee() const {
        return getOperand(0);
    }
    MDefinition* getNewTarget() const {
        return getOperand(1);
    }
    MDefinition* getPrototype() const {
        return getOperand(2);
    }

    // Although creation of |this| modifies global state, it is safely repeatable.
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool possiblyCalls() const override {
        return true;
    }
};

// Caller-side allocation of |this| for |new|:
// Constructs |this| when possible, else MagicValue(JS_IS_CONSTRUCTING).
class MCreateThis
  : public MBinaryInstruction,
    public MixPolicy<ObjectPolicy<0>, ObjectPolicy<1> >::Data
{
    explicit MCreateThis(MDefinition* callee, MDefinition* newTarget)
      : MBinaryInstruction(callee, newTarget)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(CreateThis)
    static MCreateThis* New(TempAllocator& alloc, MDefinition* callee, MDefinition* newTarget)
    {
        return new(alloc) MCreateThis(callee, newTarget);
    }

    MDefinition* getCallee() const {
        return getOperand(0);
    }
    MDefinition* getNewTarget() const {
        return getOperand(0);
    }

    // Although creation of |this| modifies global state, it is safely repeatable.
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool possiblyCalls() const override {
        return true;
    }
};

// Eager initialization of arguments object.
class MCreateArgumentsObject
  : public MUnaryInstruction,
    public ObjectPolicy<0>::Data
{
    explicit MCreateArgumentsObject(MDefinition* callObj)
      : MUnaryInstruction(callObj)
    {
        setResultType(MIRType_Object);
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(CreateArgumentsObject)
    static MCreateArgumentsObject* New(TempAllocator& alloc, MDefinition* callObj) {
        return new(alloc) MCreateArgumentsObject(callObj);
    }

    MDefinition* getCallObject() const {
        return getOperand(0);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MGetArgumentsObjectArg
  : public MUnaryInstruction,
    public ObjectPolicy<0>::Data
{
    size_t argno_;

    MGetArgumentsObjectArg(MDefinition* argsObject, size_t argno)
      : MUnaryInstruction(argsObject),
        argno_(argno)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(GetArgumentsObjectArg)
    static MGetArgumentsObjectArg* New(TempAllocator& alloc, MDefinition* argsObj, size_t argno)
    {
        return new(alloc) MGetArgumentsObjectArg(argsObj, argno);
    }

    MDefinition* getArgsObject() const {
        return getOperand(0);
    }

    size_t argno() const {
        return argno_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::Any);
    }
};

class MSetArgumentsObjectArg
  : public MBinaryInstruction,
    public MixPolicy<ObjectPolicy<0>, BoxPolicy<1> >::Data
{
    size_t argno_;

    MSetArgumentsObjectArg(MDefinition* argsObj, size_t argno, MDefinition* value)
      : MBinaryInstruction(argsObj, value),
        argno_(argno)
    {
    }

  public:
    INSTRUCTION_HEADER(SetArgumentsObjectArg)
    static MSetArgumentsObjectArg* New(TempAllocator& alloc, MDefinition* argsObj, size_t argno,
                                       MDefinition* value)
    {
        return new(alloc) MSetArgumentsObjectArg(argsObj, argno, value);
    }

    MDefinition* getArgsObject() const {
        return getOperand(0);
    }

    size_t argno() const {
        return argno_;
    }

    MDefinition* getValue() const {
        return getOperand(1);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::Any);
    }
};

class MRunOncePrologue
  : public MNullaryInstruction
{
  protected:
    MRunOncePrologue()
    {
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(RunOncePrologue)

    static MRunOncePrologue* New(TempAllocator& alloc) {
        return new(alloc) MRunOncePrologue();
    }
    bool possiblyCalls() const override {
        return true;
    }
};

// Given a MIRType_Value A and a MIRType_Object B:
// If the Value may be safely unboxed to an Object, return Object(A).
// Otherwise, return B.
// Used to implement return behavior for inlined constructors.
class MReturnFromCtor
  : public MAryInstruction<2>,
    public MixPolicy<BoxPolicy<0>, ObjectPolicy<1> >::Data
{
    MReturnFromCtor(MDefinition* value, MDefinition* object) {
        initOperand(0, value);
        initOperand(1, object);
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(ReturnFromCtor)
    static MReturnFromCtor* New(TempAllocator& alloc, MDefinition* value, MDefinition* object)
    {
        return new(alloc) MReturnFromCtor(value, object);
    }

    MDefinition* getValue() const {
        return getOperand(0);
    }
    MDefinition* getObject() const {
        return getOperand(1);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MToFPInstruction
  : public MUnaryInstruction,
    public ToDoublePolicy::Data
{
  public:
    // Types of values which can be converted.
    enum ConversionKind {
        NonStringPrimitives,
        NonNullNonStringPrimitives,
        NumbersOnly
    };

  private:
    ConversionKind conversion_;

  protected:
    explicit MToFPInstruction(MDefinition* def, ConversionKind conversion = NonStringPrimitives)
      : MUnaryInstruction(def), conversion_(conversion)
    { }

  public:
    ConversionKind conversion() const {
        return conversion_;
    }
};

// Converts a primitive (either typed or untyped) to a double. If the input is
// not primitive at runtime, a bailout occurs.
class MToDouble
  : public MToFPInstruction
{
  private:
    TruncateKind implicitTruncate_;

    explicit MToDouble(MDefinition* def, ConversionKind conversion = NonStringPrimitives)
      : MToFPInstruction(def, conversion), implicitTruncate_(NoTruncate)
    {
        setResultType(MIRType_Double);
        setMovable();

        // An object might have "valueOf", which means it is effectful.
        // ToNumber(symbol) throws.
        if (def->mightBeType(MIRType_Object) || def->mightBeType(MIRType_Symbol))
            setGuard();
    }

  public:
    INSTRUCTION_HEADER(ToDouble)
    static MToDouble* New(TempAllocator& alloc, MDefinition* def,
                          ConversionKind conversion = NonStringPrimitives)
    {
        return new(alloc) MToDouble(def, conversion);
    }
    static MToDouble* NewAsmJS(TempAllocator& alloc, MDefinition* def) {
        return new(alloc) MToDouble(def);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isToDouble() || ins->toToDouble()->conversion() != conversion())
            return false;
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    void computeRange(TempAllocator& alloc) override;
    bool needTruncation(TruncateKind kind) override;
    void truncate() override;
    TruncateKind operandTruncateKind(size_t index) const override;

#ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif

    TruncateKind truncateKind() const {
        return implicitTruncate_;
    }
    void setTruncateKind(TruncateKind kind) {
        implicitTruncate_ = Max(implicitTruncate_, kind);
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        if (input()->type() == MIRType_Value)
            return false;
        if (input()->type() == MIRType_Symbol)
            return false;

        return true;
    }

    ALLOW_CLONE(MToDouble)
};

// Converts a primitive (either typed or untyped) to a float32. If the input is
// not primitive at runtime, a bailout occurs.
class MToFloat32
  : public MToFPInstruction
{
  protected:
    MToFloat32(MDefinition* def, ConversionKind conversion)
      : MToFPInstruction(def, conversion)
    {
        setResultType(MIRType_Float32);
        setMovable();

        // An object might have "valueOf", which means it is effectful.
        // ToNumber(symbol) throws.
        if (def->mightBeType(MIRType_Object) || def->mightBeType(MIRType_Symbol))
            setGuard();
    }

  public:
    INSTRUCTION_HEADER(ToFloat32)
    static MToFloat32* New(TempAllocator& alloc, MDefinition* def,
                           ConversionKind conversion = NonStringPrimitives)
    {
        return new(alloc) MToFloat32(def, conversion);
    }
    static MToFloat32* NewAsmJS(TempAllocator& alloc, MDefinition* def) {
        return new(alloc) MToFloat32(def, NonStringPrimitives);
    }

    virtual MDefinition* foldsTo(TempAllocator& alloc) override;
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isToFloat32() || ins->toToFloat32()->conversion() != conversion())
            return false;
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    void computeRange(TempAllocator& alloc) override;

    bool canConsumeFloat32(MUse* use) const override { return true; }
    bool canProduceFloat32() const override { return true; }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MToFloat32)
};

// Converts a uint32 to a double (coming from asm.js).
class MAsmJSUnsignedToDouble
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    explicit MAsmJSUnsignedToDouble(MDefinition* def)
      : MUnaryInstruction(def)
    {
        setResultType(MIRType_Double);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(AsmJSUnsignedToDouble)
    static MAsmJSUnsignedToDouble* NewAsmJS(TempAllocator& alloc, MDefinition* def) {
        return new(alloc) MAsmJSUnsignedToDouble(def);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// Converts a uint32 to a float32 (coming from asm.js).
class MAsmJSUnsignedToFloat32
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    explicit MAsmJSUnsignedToFloat32(MDefinition* def)
      : MUnaryInstruction(def)
    {
        setResultType(MIRType_Float32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(AsmJSUnsignedToFloat32)
    static MAsmJSUnsignedToFloat32* NewAsmJS(TempAllocator& alloc, MDefinition* def) {
        return new(alloc) MAsmJSUnsignedToFloat32(def);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool canProduceFloat32() const override { return true; }
};

// Converts a primitive (either typed or untyped) to an int32. If the input is
// not primitive at runtime, a bailout occurs. If the input cannot be converted
// to an int32 without loss (i.e. "5.5" or undefined) then a bailout occurs.
class MToInt32
  : public MUnaryInstruction,
    public ToInt32Policy::Data
{
    bool canBeNegativeZero_;
    MacroAssembler::IntConversionInputKind conversion_;

    MToInt32(MDefinition* def, MacroAssembler::IntConversionInputKind conversion)
      : MUnaryInstruction(def),
        canBeNegativeZero_(true),
        conversion_(conversion)
    {
        setResultType(MIRType_Int32);
        setMovable();

        // An object might have "valueOf", which means it is effectful.
        // ToNumber(symbol) throws.
        if (def->mightBeType(MIRType_Object) || def->mightBeType(MIRType_Symbol))
            setGuard();
    }

  public:
    INSTRUCTION_HEADER(ToInt32)
    static MToInt32* New(TempAllocator& alloc, MDefinition* def,
                         MacroAssembler::IntConversionInputKind conversion =
                             MacroAssembler::IntConversion_Any)
    {
        return new(alloc) MToInt32(def, conversion);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    // this only has backwards information flow.
    void analyzeEdgeCasesBackward() override;

    bool canBeNegativeZero() const {
        return canBeNegativeZero_;
    }
    void setCanBeNegativeZero(bool negativeZero) {
        canBeNegativeZero_ = negativeZero;
    }

    MacroAssembler::IntConversionInputKind conversion() const {
        return conversion_;
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isToInt32() || ins->toToInt32()->conversion() != conversion())
            return false;
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    void computeRange(TempAllocator& alloc) override;
    void collectRangeInfoPreTrunc() override;

#ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif

    ALLOW_CLONE(MToInt32)
};

// Converts a value or typed input to a truncated int32, for use with bitwise
// operations. This is an infallible ValueToECMAInt32.
class MTruncateToInt32
  : public MUnaryInstruction,
    public ToInt32Policy::Data
{
    explicit MTruncateToInt32(MDefinition* def)
      : MUnaryInstruction(def)
    {
        setResultType(MIRType_Int32);
        setMovable();

        // An object might have "valueOf", which means it is effectful.
        // ToInt32(symbol) throws.
        if (def->mightBeType(MIRType_Object) || def->mightBeType(MIRType_Symbol))
            setGuard();
    }

  public:
    INSTRUCTION_HEADER(TruncateToInt32)
    static MTruncateToInt32* New(TempAllocator& alloc, MDefinition* def) {
        return new(alloc) MTruncateToInt32(def);
    }
    static MTruncateToInt32* NewAsmJS(TempAllocator& alloc, MDefinition* def) {
        return new(alloc) MTruncateToInt32(def);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    void computeRange(TempAllocator& alloc) override;
    TruncateKind operandTruncateKind(size_t index) const override;
# ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override {
        return true;
    }
#endif

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return input()->type() < MIRType_Symbol;
    }

    ALLOW_CLONE(MTruncateToInt32)
};

// Converts any type to a string
class MToString :
  public MUnaryInstruction,
  public ToStringPolicy::Data
{
    explicit MToString(MDefinition* def)
      : MUnaryInstruction(def)
    {
        setResultType(MIRType_String);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(ToString)
    static MToString* New(TempAllocator& alloc, MDefinition* def)
    {
        return new(alloc) MToString(def);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool fallible() const {
        return input()->mightBeType(MIRType_Object);
    }

    ALLOW_CLONE(MToString)
};

// Converts any type to an object or null value, throwing on undefined.
class MToObjectOrNull :
  public MUnaryInstruction,
  public BoxInputsPolicy::Data
{
    explicit MToObjectOrNull(MDefinition* def)
      : MUnaryInstruction(def)
    {
        setResultType(MIRType_ObjectOrNull);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(ToObjectOrNull)
    static MToObjectOrNull* New(TempAllocator& alloc, MDefinition* def)
    {
        return new(alloc) MToObjectOrNull(def);
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    ALLOW_CLONE(MToObjectOrNull)
};

class MBitNot
  : public MUnaryInstruction,
    public BitwisePolicy::Data
{
  protected:
    explicit MBitNot(MDefinition* input)
      : MUnaryInstruction(input)
    {
        specialization_ = MIRType_None;
        setResultType(MIRType_Int32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(BitNot)
    static MBitNot* New(TempAllocator& alloc, MDefinition* input);
    static MBitNot* NewAsmJS(TempAllocator& alloc, MDefinition* input);

    MDefinition* foldsTo(TempAllocator& alloc) override;
    void setSpecialization(MIRType type) {
        specialization_ = type;
        setResultType(type);
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        if (specialization_ == MIRType_None)
            return AliasSet::Store(AliasSet::Any);
        return AliasSet::None();
    }
    void computeRange(TempAllocator& alloc) override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ != MIRType_None;
    }

    ALLOW_CLONE(MBitNot)
};

class MTypeOf
  : public MUnaryInstruction,
    public BoxInputsPolicy::Data
{
    MIRType inputType_;
    bool inputMaybeCallableOrEmulatesUndefined_;

    MTypeOf(MDefinition* def, MIRType inputType)
      : MUnaryInstruction(def), inputType_(inputType),
        inputMaybeCallableOrEmulatesUndefined_(true)
    {
        setResultType(MIRType_String);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(TypeOf)

    static MTypeOf* New(TempAllocator& alloc, MDefinition* def, MIRType inputType) {
        return new(alloc) MTypeOf(def, inputType);
    }

    MIRType inputType() const {
        return inputType_;
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
    void cacheInputMaybeCallableOrEmulatesUndefined(CompilerConstraintList* constraints);

    bool inputMaybeCallableOrEmulatesUndefined() const {
        return inputMaybeCallableOrEmulatesUndefined_;
    }
    void markInputNotCallableOrEmulatesUndefined() {
        inputMaybeCallableOrEmulatesUndefined_ = false;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isTypeOf())
            return false;
        if (inputType() != ins->toTypeOf()->inputType())
            return false;
        if (inputMaybeCallableOrEmulatesUndefined() !=
            ins->toTypeOf()->inputMaybeCallableOrEmulatesUndefined())
        {
            return false;
        }
        return congruentIfOperandsEqual(ins);
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }
};

class MToId
  : public MUnaryInstruction,
    public BoxInputsPolicy::Data
{
    explicit MToId(MDefinition* index)
      : MUnaryInstruction(index)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(ToId)

    static MToId* New(TempAllocator& alloc, MDefinition* index) {
        return new(alloc) MToId(index);
    }
};

class MBinaryBitwiseInstruction
  : public MBinaryInstruction,
    public BitwisePolicy::Data
{
  protected:
    MBinaryBitwiseInstruction(MDefinition* left, MDefinition* right)
      : MBinaryInstruction(left, right)
    {
        setResultType(MIRType_Int32);
        setMovable();
    }

    void specializeAsInt32();

  public:
    MDefinition* foldsTo(TempAllocator& alloc) override;
    MDefinition* foldUnnecessaryBitop();
    virtual MDefinition* foldIfZero(size_t operand) = 0;
    virtual MDefinition* foldIfNegOne(size_t operand) = 0;
    virtual MDefinition* foldIfEqual()  = 0;
    virtual void infer(BaselineInspector* inspector, jsbytecode* pc);

    bool congruentTo(const MDefinition* ins) const override {
        return binaryCongruentTo(ins);
    }
    AliasSet getAliasSet() const override {
        if (specialization_ >= MIRType_Object)
            return AliasSet::Store(AliasSet::Any);
        return AliasSet::None();
    }

    TruncateKind operandTruncateKind(size_t index) const override;
};

class MBitAnd : public MBinaryBitwiseInstruction
{
    MBitAnd(MDefinition* left, MDefinition* right)
      : MBinaryBitwiseInstruction(left, right)
    { }

  public:
    INSTRUCTION_HEADER(BitAnd)
    static MBitAnd* New(TempAllocator& alloc, MDefinition* left, MDefinition* right);
    static MBitAnd* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right);

    MDefinition* foldIfZero(size_t operand) override {
        return getOperand(operand); // 0 & x => 0;
    }
    MDefinition* foldIfNegOne(size_t operand) override {
        return getOperand(1 - operand); // x & -1 => x
    }
    MDefinition* foldIfEqual() override {
        return getOperand(0); // x & x => x;
    }
    void computeRange(TempAllocator& alloc) override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ != MIRType_None;
    }

    ALLOW_CLONE(MBitAnd)
};

class MBitOr : public MBinaryBitwiseInstruction
{
    MBitOr(MDefinition* left, MDefinition* right)
      : MBinaryBitwiseInstruction(left, right)
    { }

  public:
    INSTRUCTION_HEADER(BitOr)
    static MBitOr* New(TempAllocator& alloc, MDefinition* left, MDefinition* right);
    static MBitOr* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right);

    MDefinition* foldIfZero(size_t operand) override {
        return getOperand(1 - operand); // 0 | x => x, so if ith is 0, return (1-i)th
    }
    MDefinition* foldIfNegOne(size_t operand) override {
        return getOperand(operand); // x | -1 => -1
    }
    MDefinition* foldIfEqual() override {
        return getOperand(0); // x | x => x
    }
    void computeRange(TempAllocator& alloc) override;
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ != MIRType_None;
    }

    ALLOW_CLONE(MBitOr)
};

class MBitXor : public MBinaryBitwiseInstruction
{
    MBitXor(MDefinition* left, MDefinition* right)
      : MBinaryBitwiseInstruction(left, right)
    { }

  public:
    INSTRUCTION_HEADER(BitXor)
    static MBitXor* New(TempAllocator& alloc, MDefinition* left, MDefinition* right);
    static MBitXor* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right);

    MDefinition* foldIfZero(size_t operand) override {
        return getOperand(1 - operand); // 0 ^ x => x
    }
    MDefinition* foldIfNegOne(size_t operand) override {
        return this;
    }
    MDefinition* foldIfEqual() override {
        return this;
    }
    void computeRange(TempAllocator& alloc) override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ < MIRType_Object;
    }

    ALLOW_CLONE(MBitXor)
};

class MShiftInstruction
  : public MBinaryBitwiseInstruction
{
  protected:
    MShiftInstruction(MDefinition* left, MDefinition* right)
      : MBinaryBitwiseInstruction(left, right)
    { }

  public:
    MDefinition* foldIfNegOne(size_t operand) override {
        return this;
    }
    MDefinition* foldIfEqual() override {
        return this;
    }
    virtual void infer(BaselineInspector* inspector, jsbytecode* pc) override;
};

class MLsh : public MShiftInstruction
{
    MLsh(MDefinition* left, MDefinition* right)
      : MShiftInstruction(left, right)
    { }

  public:
    INSTRUCTION_HEADER(Lsh)
    static MLsh* New(TempAllocator& alloc, MDefinition* left, MDefinition* right);
    static MLsh* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right);

    MDefinition* foldIfZero(size_t operand) override {
        // 0 << x => 0
        // x << 0 => x
        return getOperand(0);
    }

    void computeRange(TempAllocator& alloc) override;
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ != MIRType_None;
    }

    ALLOW_CLONE(MLsh)
};

class MRsh : public MShiftInstruction
{
    MRsh(MDefinition* left, MDefinition* right)
      : MShiftInstruction(left, right)
    { }

  public:
    INSTRUCTION_HEADER(Rsh)
    static MRsh* New(TempAllocator& alloc, MDefinition* left, MDefinition* right);
    static MRsh* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right);

    MDefinition* foldIfZero(size_t operand) override {
        // 0 >> x => 0
        // x >> 0 => x
        return getOperand(0);
    }
    void computeRange(TempAllocator& alloc) override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ < MIRType_Object;
    }

    ALLOW_CLONE(MRsh)
};

class MUrsh : public MShiftInstruction
{
    bool bailoutsDisabled_;

    MUrsh(MDefinition* left, MDefinition* right)
      : MShiftInstruction(left, right),
        bailoutsDisabled_(false)
    { }

  public:
    INSTRUCTION_HEADER(Ursh)
    static MUrsh* New(TempAllocator& alloc, MDefinition* left, MDefinition* right);
    static MUrsh* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right);

    MDefinition* foldIfZero(size_t operand) override {
        // 0 >>> x => 0
        if (operand == 0)
            return getOperand(0);

        return this;
    }

    void infer(BaselineInspector* inspector, jsbytecode* pc) override;

    bool bailoutsDisabled() const {
        return bailoutsDisabled_;
    }

    bool fallible() const;

    void computeRange(TempAllocator& alloc) override;
    void collectRangeInfoPreTrunc() override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ < MIRType_Object;
    }

    ALLOW_CLONE(MUrsh)
};

class MBinaryArithInstruction
  : public MBinaryInstruction,
    public ArithPolicy::Data
{
    // Implicit truncate flag is set by the truncate backward range analysis
    // optimization phase, and by asm.js pre-processing. It is used in
    // NeedNegativeZeroCheck to check if the result of a multiplication needs to
    // produce -0 double value, and for avoiding overflow checks.

    // This optimization happens when the multiplication cannot be truncated
    // even if all uses are truncating its result, such as when the range
    // analysis detect a precision loss in the multiplication.
    TruncateKind implicitTruncate_;

  public:
    MBinaryArithInstruction(MDefinition* left, MDefinition* right)
      : MBinaryInstruction(left, right),
        implicitTruncate_(NoTruncate)
    {
        specialization_ = MIRType_None;
        setMovable();
    }

    static MBinaryArithInstruction* New(TempAllocator& alloc, Opcode op,
                                        MDefinition* left, MDefinition* right);

    bool constantDoubleResult(TempAllocator& alloc);

    MDefinition* foldsTo(TempAllocator& alloc) override;

    virtual double getIdentity() = 0;

    void setSpecialization(MIRType type) {
        specialization_ = type;
        setResultType(type);
    }
    void setInt32Specialization() {
        specialization_ = MIRType_Int32;
        setResultType(MIRType_Int32);
    }
    void setNumberSpecialization(TempAllocator& alloc, BaselineInspector* inspector, jsbytecode* pc);

    virtual void trySpecializeFloat32(TempAllocator& alloc) override;

    bool congruentTo(const MDefinition* ins) const override {
        return binaryCongruentTo(ins);
    }
    AliasSet getAliasSet() const override {
        if (specialization_ >= MIRType_Object)
            return AliasSet::Store(AliasSet::Any);
        return AliasSet::None();
    }

    bool isTruncated() const {
        return implicitTruncate_ == Truncate;
    }
    TruncateKind truncateKind() const {
        return implicitTruncate_;
    }
    void setTruncateKind(TruncateKind kind) {
        implicitTruncate_ = Max(implicitTruncate_, kind);
    }
};

class MMinMax
  : public MBinaryInstruction,
    public ArithPolicy::Data
{
    bool isMax_;

    MMinMax(MDefinition* left, MDefinition* right, MIRType type, bool isMax)
      : MBinaryInstruction(left, right),
        isMax_(isMax)
    {
        MOZ_ASSERT(IsNumberType(type));
        setResultType(type);
        setMovable();
        specialization_ = type;
    }

  public:
    INSTRUCTION_HEADER(MinMax)
    static MMinMax* New(TempAllocator& alloc, MDefinition* left, MDefinition* right, MIRType type,
                        bool isMax)
    {
        return new(alloc) MMinMax(left, right, type, isMax);
    }

    bool isMax() const {
        return isMax_;
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isMinMax())
            return false;
        if (isMax() != ins->toMinMax()->isMax())
            return false;
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    MDefinition* foldsTo(TempAllocator& alloc) override;
    void computeRange(TempAllocator& alloc) override;
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    bool isFloat32Commutative() const override { return true; }
    void trySpecializeFloat32(TempAllocator& alloc) override;

    ALLOW_CLONE(MMinMax)
};

class MAbs
  : public MUnaryInstruction,
    public ArithPolicy::Data
{
    bool implicitTruncate_;

    MAbs(MDefinition* num, MIRType type)
      : MUnaryInstruction(num),
        implicitTruncate_(false)
    {
        MOZ_ASSERT(IsNumberType(type));
        setResultType(type);
        setMovable();
        specialization_ = type;
    }

  public:
    INSTRUCTION_HEADER(Abs)
    static MAbs* New(TempAllocator& alloc, MDefinition* num, MIRType type) {
        return new(alloc) MAbs(num, type);
    }
    static MAbs* NewAsmJS(TempAllocator& alloc, MDefinition* num, MIRType type) {
        MAbs* ins = new(alloc) MAbs(num, type);
        if (type == MIRType_Int32)
            ins->implicitTruncate_ = true;
        return ins;
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    bool fallible() const;

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    void computeRange(TempAllocator& alloc) override;
    bool isFloat32Commutative() const override { return true; }
    void trySpecializeFloat32(TempAllocator& alloc) override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MAbs)
};

class MClz
    : public MUnaryInstruction
    , public BitwisePolicy::Data
{
    bool operandIsNeverZero_;

    explicit MClz(MDefinition* num)
      : MUnaryInstruction(num),
        operandIsNeverZero_(false)
    {
        MOZ_ASSERT(IsNumberType(num->type()));
        specialization_ = MIRType_Int32;
        setResultType(MIRType_Int32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Clz)
    static MClz* New(TempAllocator& alloc, MDefinition* num) {
        return new(alloc) MClz(num);
    }
    static MClz* NewAsmJS(TempAllocator& alloc, MDefinition* num) {
        return new(alloc) MClz(num);
    }
    MDefinition* num() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool operandIsNeverZero() const {
        return operandIsNeverZero_;
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
    void computeRange(TempAllocator& alloc) override;
    void collectRangeInfoPreTrunc() override;
};

// Inline implementation of Math.sqrt().
class MSqrt
  : public MUnaryInstruction,
    public FloatingPointPolicy<0>::Data
{
    MSqrt(MDefinition* num, MIRType type)
      : MUnaryInstruction(num)
    {
        setResultType(type);
        specialization_ = type;
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Sqrt)
    static MSqrt* New(TempAllocator& alloc, MDefinition* num) {
        return new(alloc) MSqrt(num, MIRType_Double);
    }
    static MSqrt* NewAsmJS(TempAllocator& alloc, MDefinition* num, MIRType type) {
        MOZ_ASSERT(IsFloatingPointType(type));
        return new(alloc) MSqrt(num, type);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    void computeRange(TempAllocator& alloc) override;

    bool isFloat32Commutative() const override { return true; }
    void trySpecializeFloat32(TempAllocator& alloc) override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MSqrt)
};

// Inline implementation of atan2 (arctangent of y/x).
class MAtan2
  : public MBinaryInstruction,
    public MixPolicy<DoublePolicy<0>, DoublePolicy<1> >::Data
{
    MAtan2(MDefinition* y, MDefinition* x)
      : MBinaryInstruction(y, x)
    {
        setResultType(MIRType_Double);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Atan2)
    static MAtan2* New(TempAllocator& alloc, MDefinition* y, MDefinition* x) {
        return new(alloc) MAtan2(y, x);
    }

    MDefinition* y() const {
        return getOperand(0);
    }

    MDefinition* x() const {
        return getOperand(1);
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool possiblyCalls() const override {
        return true;
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MAtan2)
};

// Inline implementation of Math.hypot().
class MHypot
  : public MVariadicInstruction,
    public AllDoublePolicy::Data
{
    MHypot()
    {
        setResultType(MIRType_Double);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Hypot)
    static MHypot* New(TempAllocator& alloc, const MDefinitionVector& vector);

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool possiblyCalls() const override {
        return true;
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    bool canClone() const override {
        return true;
    }

    MInstruction* clone(TempAllocator& alloc,
                        const MDefinitionVector& inputs) const override {
       return MHypot::New(alloc, inputs);
    }
};

// Inline implementation of Math.pow().
class MPow
  : public MBinaryInstruction,
    public PowPolicy::Data
{
    MPow(MDefinition* input, MDefinition* power, MIRType powerType)
      : MBinaryInstruction(input, power)
    {
        specialization_ = powerType;
        setResultType(MIRType_Double);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Pow)
    static MPow* New(TempAllocator& alloc, MDefinition* input, MDefinition* power,
                     MIRType powerType)
    {
        MOZ_ASSERT(powerType == MIRType_Double || powerType == MIRType_Int32);
        return new(alloc) MPow(input, power, powerType);
    }

    MDefinition* input() const {
        return lhs();
    }
    MDefinition* power() const {
        return rhs();
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool possiblyCalls() const override {
        return true;
    }
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        // Temporarily disable recovery to relieve fuzzer pressure. See bug 1188586.
        return false;
    }

    ALLOW_CLONE(MPow)
};

// Inline implementation of Math.pow(x, 0.5), which subtly differs from Math.sqrt(x).
class MPowHalf
  : public MUnaryInstruction,
    public DoublePolicy<0>::Data
{
    bool operandIsNeverNegativeInfinity_;
    bool operandIsNeverNegativeZero_;
    bool operandIsNeverNaN_;

    explicit MPowHalf(MDefinition* input)
      : MUnaryInstruction(input),
        operandIsNeverNegativeInfinity_(false),
        operandIsNeverNegativeZero_(false),
        operandIsNeverNaN_(false)
    {
        setResultType(MIRType_Double);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(PowHalf)
    static MPowHalf* New(TempAllocator& alloc, MDefinition* input) {
        return new(alloc) MPowHalf(input);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    bool operandIsNeverNegativeInfinity() const {
        return operandIsNeverNegativeInfinity_;
    }
    bool operandIsNeverNegativeZero() const {
        return operandIsNeverNegativeZero_;
    }
    bool operandIsNeverNaN() const {
        return operandIsNeverNaN_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    void collectRangeInfoPreTrunc() override;
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MPowHalf)
};

// Inline implementation of Math.random().
class MRandom : public MNullaryInstruction
{
    MRandom()
    {
        setResultType(MIRType_Double);
    }

  public:
    INSTRUCTION_HEADER(Random)
    static MRandom* New(TempAllocator& alloc) {
        return new(alloc) MRandom;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool possiblyCalls() const override {
        return true;
    }

    void computeRange(TempAllocator& alloc) override;

    ALLOW_CLONE(MRandom)
};

class MMathFunction
  : public MUnaryInstruction,
    public FloatingPointPolicy<0>::Data
{
  public:
    enum Function {
        Log,
        Sin,
        Cos,
        Exp,
        Tan,
        ACos,
        ASin,
        ATan,
        Log10,
        Log2,
        Log1P,
        ExpM1,
        CosH,
        SinH,
        TanH,
        ACosH,
        ASinH,
        ATanH,
        Sign,
        Trunc,
        Cbrt,
        Floor,
        Ceil,
        Round
    };

  private:
    Function function_;
    const MathCache* cache_;

    MMathFunction(MDefinition* input, Function function, const MathCache* cache)
      : MUnaryInstruction(input), function_(function), cache_(cache)
    {
        setResultType(MIRType_Double);
        specialization_ = MIRType_Double;
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(MathFunction)

    // A nullptr cache means this function will neither access nor update the cache.
    static MMathFunction* New(TempAllocator& alloc, MDefinition* input, Function function,
                              const MathCache* cache)
    {
        return new(alloc) MMathFunction(input, function, cache);
    }
    Function function() const {
        return function_;
    }
    const MathCache* cache() const {
        return cache_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isMathFunction())
            return false;
        if (ins->toMathFunction()->function() != function())
            return false;
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool possiblyCalls() const override {
        return true;
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    void printOpcode(GenericPrinter& out) const override;

    static const char* FunctionName(Function function);

    bool isFloat32Commutative() const override {
        return function_ == Floor || function_ == Ceil || function_ == Round;
    }
    void trySpecializeFloat32(TempAllocator& alloc) override;
    void computeRange(TempAllocator& alloc) override;
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        if (input()->type() == MIRType_SinCosDouble)
            return false;
        switch(function_) {
          case Sin:
          case Log:
          case Round:
            return true;
          default:
            return false;
        }
    }

    ALLOW_CLONE(MMathFunction)
};

class MAdd : public MBinaryArithInstruction
{
    // Is this instruction really an int at heart?
    MAdd(MDefinition* left, MDefinition* right)
      : MBinaryArithInstruction(left, right)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(Add)
    static MAdd* New(TempAllocator& alloc, MDefinition* left, MDefinition* right) {
        return new(alloc) MAdd(left, right);
    }

    static MAdd* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                          MIRType type)
    {
        MAdd* add = new(alloc) MAdd(left, right);
        add->specialization_ = type;
        add->setResultType(type);
        if (type == MIRType_Int32) {
            add->setTruncateKind(Truncate);
            add->setCommutative();
        }
        return add;
    }

    bool isFloat32Commutative() const override { return true; }

    double getIdentity() override {
        return 0;
    }

    bool fallible() const;
    void computeRange(TempAllocator& alloc) override;
    bool needTruncation(TruncateKind kind) override;
    void truncate() override;
    TruncateKind operandTruncateKind(size_t index) const override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ < MIRType_Object;
    }

    ALLOW_CLONE(MAdd)
};

class MSub : public MBinaryArithInstruction
{
    MSub(MDefinition* left, MDefinition* right)
      : MBinaryArithInstruction(left, right)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(Sub)
    static MSub* New(TempAllocator& alloc, MDefinition* left, MDefinition* right) {
        return new(alloc) MSub(left, right);
    }
    static MSub* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                          MIRType type)
    {
        MSub* sub = new(alloc) MSub(left, right);
        sub->specialization_ = type;
        sub->setResultType(type);
        if (type == MIRType_Int32)
            sub->setTruncateKind(Truncate);
        return sub;
    }

    double getIdentity() override {
        return 0;
    }

    bool isFloat32Commutative() const override { return true; }

    bool fallible() const;
    void computeRange(TempAllocator& alloc) override;
    bool needTruncation(TruncateKind kind) override;
    void truncate() override;
    TruncateKind operandTruncateKind(size_t index) const override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ < MIRType_Object;
    }

    ALLOW_CLONE(MSub)
};

class MMul : public MBinaryArithInstruction
{
  public:
    enum Mode {
        Normal,
        Integer
    };

  private:
    // Annotation the result could be a negative zero
    // and we need to guard this during execution.
    bool canBeNegativeZero_;

    Mode mode_;

    MMul(MDefinition* left, MDefinition* right, MIRType type, Mode mode)
      : MBinaryArithInstruction(left, right),
        canBeNegativeZero_(true),
        mode_(mode)
    {
        if (mode == Integer) {
            // This implements the required behavior for Math.imul, which
            // can never fail and always truncates its output to int32.
            canBeNegativeZero_ = false;
            setTruncateKind(Truncate);
            setCommutative();
        }
        MOZ_ASSERT_IF(mode != Integer, mode == Normal);

        if (type != MIRType_Value)
            specialization_ = type;
        setResultType(type);
    }

  public:
    INSTRUCTION_HEADER(Mul)
    static MMul* New(TempAllocator& alloc, MDefinition* left, MDefinition* right) {
        return new(alloc) MMul(left, right, MIRType_Value, MMul::Normal);
    }
    static MMul* New(TempAllocator& alloc, MDefinition* left, MDefinition* right, MIRType type,
                     Mode mode = Normal)
    {
        return new(alloc) MMul(left, right, type, mode);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
    void analyzeEdgeCasesForward() override;
    void analyzeEdgeCasesBackward() override;
    void collectRangeInfoPreTrunc() override;

    double getIdentity() override {
        return 1;
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isMul())
            return false;

        const MMul* mul = ins->toMul();
        if (canBeNegativeZero_ != mul->canBeNegativeZero())
            return false;

        if (mode_ != mul->mode())
            return false;

        return binaryCongruentTo(ins);
    }

    bool canOverflow() const;

    bool canBeNegativeZero() const {
        return canBeNegativeZero_;
    }
    void setCanBeNegativeZero(bool negativeZero) {
        canBeNegativeZero_ = negativeZero;
    }

    bool updateForReplacement(MDefinition* ins) override;

    bool fallible() const {
        return canBeNegativeZero_ || canOverflow();
    }

    void setSpecialization(MIRType type) {
        specialization_ = type;
    }

    bool isFloat32Commutative() const override { return true; }

    void computeRange(TempAllocator& alloc) override;
    bool needTruncation(TruncateKind kind) override;
    void truncate() override;
    TruncateKind operandTruncateKind(size_t index) const override;

    Mode mode() const { return mode_; }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ < MIRType_Object;
    }

    ALLOW_CLONE(MMul)
};

class MDiv : public MBinaryArithInstruction
{
    bool canBeNegativeZero_;
    bool canBeNegativeOverflow_;
    bool canBeDivideByZero_;
    bool canBeNegativeDividend_;
    bool unsigned_;

    MDiv(MDefinition* left, MDefinition* right, MIRType type)
      : MBinaryArithInstruction(left, right),
        canBeNegativeZero_(true),
        canBeNegativeOverflow_(true),
        canBeDivideByZero_(true),
        canBeNegativeDividend_(true),
        unsigned_(false)
    {
        if (type != MIRType_Value)
            specialization_ = type;
        setResultType(type);
    }

  public:
    INSTRUCTION_HEADER(Div)
    static MDiv* New(TempAllocator& alloc, MDefinition* left, MDefinition* right) {
        return new(alloc) MDiv(left, right, MIRType_Value);
    }
    static MDiv* New(TempAllocator& alloc, MDefinition* left, MDefinition* right, MIRType type) {
        return new(alloc) MDiv(left, right, type);
    }
    static MDiv* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                          MIRType type, bool unsignd)
    {
        MDiv* div = new(alloc) MDiv(left, right, type);
        div->unsigned_ = unsignd;
        if (type == MIRType_Int32)
            div->setTruncateKind(Truncate);
        return div;
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
    void analyzeEdgeCasesForward() override;
    void analyzeEdgeCasesBackward() override;

    double getIdentity() override {
        MOZ_CRASH("not used");
    }

    bool canBeNegativeZero() const {
        return canBeNegativeZero_;
    }
    void setCanBeNegativeZero(bool negativeZero) {
        canBeNegativeZero_ = negativeZero;
    }

    bool canBeNegativeOverflow() const {
        return canBeNegativeOverflow_;
    }

    bool canBeDivideByZero() const {
        return canBeDivideByZero_;
    }

    bool canBeNegativeDividend() const {
        // "Dividend" is an ambiguous concept for unsigned truncated
        // division, because of the truncation procedure:
        // ((x>>>0)/2)|0, for example, gets transformed in
        // MDiv::truncate into a node with lhs representing x (not
        // x>>>0) and rhs representing the constant 2; in other words,
        // the MIR node corresponds to "cast operands to unsigned and
        // divide" operation. In this case, is the dividend x or is it
        // x>>>0? In order to resolve such ambiguities, we disallow
        // the usage of this method for unsigned division.
        MOZ_ASSERT(!unsigned_);
        return canBeNegativeDividend_;
    }

    bool isUnsigned() const {
        return unsigned_;
    }

    bool isTruncatedIndirectly() const {
        return truncateKind() >= IndirectTruncate;
    }

    bool canTruncateInfinities() const {
        return isTruncated();
    }
    bool canTruncateRemainder() const {
        return isTruncated();
    }
    bool canTruncateOverflow() const {
        return isTruncated() || isTruncatedIndirectly();
    }
    bool canTruncateNegativeZero() const {
        return isTruncated() || isTruncatedIndirectly();
    }

    bool isFloat32Commutative() const override { return true; }

    void computeRange(TempAllocator& alloc) override;
    bool fallible() const;
    bool needTruncation(TruncateKind kind) override;
    void truncate() override;
    void collectRangeInfoPreTrunc() override;
    TruncateKind operandTruncateKind(size_t index) const override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ < MIRType_Object;
    }

    ALLOW_CLONE(MDiv)
};

class MMod : public MBinaryArithInstruction
{
    bool unsigned_;
    bool canBeNegativeDividend_;
    bool canBePowerOfTwoDivisor_;
    bool canBeDivideByZero_;

    MMod(MDefinition* left, MDefinition* right, MIRType type)
      : MBinaryArithInstruction(left, right),
        unsigned_(false),
        canBeNegativeDividend_(true),
        canBePowerOfTwoDivisor_(true),
        canBeDivideByZero_(true)
    {
        if (type != MIRType_Value)
            specialization_ = type;
        setResultType(type);
    }

  public:
    INSTRUCTION_HEADER(Mod)
    static MMod* New(TempAllocator& alloc, MDefinition* left, MDefinition* right) {
        return new(alloc) MMod(left, right, MIRType_Value);
    }
    static MMod* NewAsmJS(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                          MIRType type, bool unsignd)
    {
        MMod* mod = new(alloc) MMod(left, right, type);
        mod->unsigned_ = unsignd;
        if (type == MIRType_Int32)
            mod->setTruncateKind(Truncate);
        return mod;
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    double getIdentity() override {
        MOZ_CRASH("not used");
    }

    bool canBeNegativeDividend() const {
        MOZ_ASSERT(specialization_ == MIRType_Int32);
        MOZ_ASSERT(!unsigned_);
        return canBeNegativeDividend_;
    }

    bool canBeDivideByZero() const {
        MOZ_ASSERT(specialization_ == MIRType_Int32);
        return canBeDivideByZero_;
    }

    bool canBePowerOfTwoDivisor() const {
        MOZ_ASSERT(specialization_ == MIRType_Int32);
        return canBePowerOfTwoDivisor_;
    }

    void analyzeEdgeCasesForward() override;

    bool isUnsigned() const {
        return unsigned_;
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return specialization_ < MIRType_Object;
    }

    bool fallible() const;

    void computeRange(TempAllocator& alloc) override;
    bool needTruncation(TruncateKind kind) override;
    void truncate() override;
    void collectRangeInfoPreTrunc() override;
    TruncateKind operandTruncateKind(size_t index) const override;

    ALLOW_CLONE(MMod)
};

class MConcat
  : public MBinaryInstruction,
    public MixPolicy<ConvertToStringPolicy<0>, ConvertToStringPolicy<1> >::Data
{
    MConcat(MDefinition* left, MDefinition* right)
      : MBinaryInstruction(left, right)
    {
        // At least one input should be definitely string
        MOZ_ASSERT(left->type() == MIRType_String || right->type() == MIRType_String);

        setMovable();
        setResultType(MIRType_String);
    }

  public:
    INSTRUCTION_HEADER(Concat)
    static MConcat* New(TempAllocator& alloc, MDefinition* left, MDefinition* right) {
        return new(alloc) MConcat(left, right);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MConcat)
};

class MCharCodeAt
  : public MBinaryInstruction,
    public MixPolicy<StringPolicy<0>, IntPolicy<1> >::Data
{
    MCharCodeAt(MDefinition* str, MDefinition* index)
        : MBinaryInstruction(str, index)
    {
        setMovable();
        setResultType(MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(CharCodeAt)

    static MCharCodeAt* New(TempAllocator& alloc, MDefinition* str, MDefinition* index) {
        return new(alloc) MCharCodeAt(str, index);
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    virtual AliasSet getAliasSet() const override {
        // Strings are immutable, so there is no implicit dependency.
        return AliasSet::None();
    }

    void computeRange(TempAllocator& alloc) override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MCharCodeAt)
};

class MFromCharCode
  : public MUnaryInstruction,
    public IntPolicy<0>::Data
{
    explicit MFromCharCode(MDefinition* code)
      : MUnaryInstruction(code)
    {
        setMovable();
        setResultType(MIRType_String);
    }

  public:
    INSTRUCTION_HEADER(FromCharCode)

    static MFromCharCode* New(TempAllocator& alloc, MDefinition* code) {
        return new(alloc) MFromCharCode(code);
    }

    virtual AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MFromCharCode)
};

class MSinCos
  : public MUnaryInstruction,
    public FloatingPointPolicy<0>::Data
{
    const MathCache* cache_;

    MSinCos(MDefinition *input, const MathCache *cache) : MUnaryInstruction(input), cache_(cache)
    {
        setResultType(MIRType_SinCosDouble);
        specialization_ = MIRType_Double;
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(SinCos)

    static MSinCos *New(TempAllocator &alloc, MDefinition *input, const MathCache *cache)
    {
        return new (alloc) MSinCos(input, cache);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool congruentTo(const MDefinition *ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    bool possiblyCalls() const override {
        return true;
    }
    const MathCache* cache() const {
        return cache_;
    }
};

class MStringSplit
  : public MTernaryInstruction,
    public MixPolicy<StringPolicy<0>, StringPolicy<1> >::Data
{
    MStringSplit(CompilerConstraintList* constraints, MDefinition* string, MDefinition* sep,
                 MConstant* templateObject)
      : MTernaryInstruction(string, sep, templateObject)
    {
        setResultType(MIRType_Object);
        setResultTypeSet(templateObject->resultTypeSet());
    }

  public:
    INSTRUCTION_HEADER(StringSplit)

    static MStringSplit* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                             MDefinition* string, MDefinition* sep,
                             MConstant* templateObject)
    {
        return new(alloc) MStringSplit(constraints, string, sep, templateObject);
    }
    MDefinition* string() const {
        return getOperand(0);
    }
    MDefinition* separator() const {
        return getOperand(1);
    }
    JSObject* templateObject() const {
        return &getOperand(2)->toConstant()->value().toObject();
    }
    ObjectGroup* group() const {
        return templateObject()->group();
    }
    bool possiblyCalls() const override {
        return true;
    }
    virtual AliasSet getAliasSet() const override {
        // Although this instruction returns a new array, we don't have to mark
        // it as store instruction, see also MNewArray.
        return AliasSet::None();
    }
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }
};

// Returns the value to use as |this| value. See also ComputeThis and
// BoxNonStrictThis in Interpreter.h.
class MComputeThis
  : public MUnaryInstruction,
    public BoxPolicy<0>::Data
{
    explicit MComputeThis(MDefinition* def)
      : MUnaryInstruction(def)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(ComputeThis)

    static MComputeThis* New(TempAllocator& alloc, MDefinition* def) {
        return new(alloc) MComputeThis(def);
    }

    bool possiblyCalls() const override {
        return true;
    }

    // Note: don't override getAliasSet: the thisValue hook can be effectful.
};

// Load an arrow function's |new.target| value.
class MArrowNewTarget
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MArrowNewTarget(MDefinition* callee)
      : MUnaryInstruction(callee)
    {
        setResultType(MIRType_Value);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(ArrowNewTarget)

    static MArrowNewTarget* New(TempAllocator& alloc, MDefinition* callee) {
        return new(alloc) MArrowNewTarget(callee);
    }
    MDefinition* callee() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        // An arrow function's lexical |this| value is immutable.
        return AliasSet::None();
    }
};

class MPhi final
  : public MDefinition,
    public InlineListNode<MPhi>,
    public NoTypePolicy::Data
{
    js::Vector<MUse, 2, JitAllocPolicy> inputs_;

    TruncateKind truncateKind_;
    bool hasBackedgeType_;
    bool triedToSpecialize_;
    bool isIterator_;
    bool canProduceFloat32_;
    bool canConsumeFloat32_;

#if DEBUG
    bool specialized_;
#endif

  protected:
    MUse* getUseFor(size_t index) override {
        // Note: after the initial IonBuilder pass, it is OK to change phi
        // operands such that they do not include the type sets of their
        // operands. This can arise during e.g. value numbering, where
        // definitions producing the same value may have different type sets.
        MOZ_ASSERT(index < numOperands());
        return &inputs_[index];
    }
    const MUse* getUseFor(size_t index) const override {
        return &inputs_[index];
    }

  public:
    INSTRUCTION_HEADER_WITHOUT_TYPEPOLICY(Phi)
    virtual TypePolicy* typePolicy();
    virtual MIRType typePolicySpecialization();

    MPhi(TempAllocator& alloc, MIRType resultType)
      : inputs_(alloc),
        truncateKind_(NoTruncate),
        hasBackedgeType_(false),
        triedToSpecialize_(false),
        isIterator_(false),
        canProduceFloat32_(false),
        canConsumeFloat32_(false)
#if DEBUG
        , specialized_(false)
#endif
    {
        setResultType(resultType);
    }

    static MPhi* New(TempAllocator& alloc, MIRType resultType = MIRType_Value) {
        return new(alloc) MPhi(alloc, resultType);
    }

    void removeOperand(size_t index);
    void removeAllOperands();

    MDefinition* getOperand(size_t index) const override {
        return inputs_[index].producer();
    }
    size_t numOperands() const override {
        return inputs_.length();
    }
    size_t indexOf(const MUse* u) const final override {
        MOZ_ASSERT(u >= &inputs_[0]);
        MOZ_ASSERT(u <= &inputs_[numOperands() - 1]);
        return u - &inputs_[0];
    }
    void replaceOperand(size_t index, MDefinition* operand) final override {
        inputs_[index].replaceProducer(operand);
    }
    bool hasBackedgeType() const {
        return hasBackedgeType_;
    }
    bool triedToSpecialize() const {
        return triedToSpecialize_;
    }
    void specialize(MIRType type) {
        triedToSpecialize_ = true;
        setResultType(type);
    }
    bool specializeType();

#ifdef DEBUG
    // Assert that this is a phi in a loop header with a unique predecessor and
    // a unique backedge.
    void assertLoopPhi() const;
#else
    void assertLoopPhi() const {}
#endif

    // Assuming this phi is in a loop header with a unique loop entry, return
    // the phi operand along the loop entry.
    MDefinition* getLoopPredecessorOperand() const {
        assertLoopPhi();
        return getOperand(0);
    }

    // Assuming this phi is in a loop header with a unique loop entry, return
    // the phi operand along the loop backedge.
    MDefinition* getLoopBackedgeOperand() const {
        assertLoopPhi();
        return getOperand(1);
    }

    // Whether this phi's type already includes information for def.
    bool typeIncludes(MDefinition* def);

    // Add types for this phi which speculate about new inputs that may come in
    // via a loop backedge.
    bool addBackedgeType(MIRType type, TemporaryTypeSet* typeSet);

    // Initializes the operands vector to the given capacity,
    // permitting use of addInput() instead of addInputSlow().
    bool reserveLength(size_t length) {
        return inputs_.reserve(length);
    }

    // Use only if capacity has been reserved by reserveLength
    void addInput(MDefinition* ins) {
        // Use infallibleGrowByUninitialized and placement-new instead of just
        // infallibleAppend to avoid creating a temporary MUse which will get
        // linked into |ins|'s use list and then unlinked in favor of the
        // MUse in the Vector. We'd ideally like to use an emplace method here,
        // once Vector supports that.
        inputs_.infallibleGrowByUninitialized(1);
        new (&inputs_.back()) MUse(ins, this);
    }

    // Appends a new input to the input vector. May perform reallocation.
    // Prefer reserveLength() and addInput() instead, where possible.
    bool addInputSlow(MDefinition* ins) {
        return inputs_.emplaceBack(ins, this);
    }

    // Update the type of this phi after adding |ins| as an input. Set
    // |*ptypeChange| to true if the type changed.
    bool checkForTypeChange(MDefinition* ins, bool* ptypeChange);

    MDefinition* foldsTo(TempAllocator& alloc) override;
    MDefinition* foldsTernary();
    MDefinition* foldsFilterTypeSet();

    bool congruentTo(const MDefinition* ins) const override;

    bool isIterator() const {
        return isIterator_;
    }
    void setIterator() {
        isIterator_ = true;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    void computeRange(TempAllocator& alloc) override;

    MDefinition* operandIfRedundant();

    bool canProduceFloat32() const override {
        return canProduceFloat32_;
    }

    void setCanProduceFloat32(bool can) {
        canProduceFloat32_ = can;
    }

    bool canConsumeFloat32(MUse* use) const override {
        return canConsumeFloat32_;
    }

    void setCanConsumeFloat32(bool can) {
        canConsumeFloat32_ = can;
    }

    TruncateKind operandTruncateKind(size_t index) const override;
    bool needTruncation(TruncateKind kind) override;
    void truncate() override;
};

// The goal of a Beta node is to split a def at a conditionally taken
// branch, so that uses dominated by it have a different name.
class MBeta
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  private:
    // This is the range induced by a comparison and branch in a preceding
    // block. Note that this does not reflect any range constraints from
    // the input value itself, so this value may differ from the range()
    // range after it is computed.
    const Range* comparison_;

    MBeta(MDefinition* val, const Range* comp)
        : MUnaryInstruction(val),
          comparison_(comp)
    {
        setResultType(val->type());
        setResultTypeSet(val->resultTypeSet());
    }

  public:
    INSTRUCTION_HEADER(Beta)
    void printOpcode(GenericPrinter& out) const override;
    static MBeta* New(TempAllocator& alloc, MDefinition* val, const Range* comp)
    {
        return new(alloc) MBeta(val, comp);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    void computeRange(TempAllocator& alloc) override;
};

// MIR representation of a Value on the OSR BaselineFrame.
// The Value is indexed off of OsrFrameReg.
class MOsrValue
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  private:
    ptrdiff_t frameOffset_;

    MOsrValue(MOsrEntry* entry, ptrdiff_t frameOffset)
      : MUnaryInstruction(entry),
        frameOffset_(frameOffset)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(OsrValue)
    static MOsrValue* New(TempAllocator& alloc, MOsrEntry* entry, ptrdiff_t frameOffset) {
        return new(alloc) MOsrValue(entry, frameOffset);
    }

    ptrdiff_t frameOffset() const {
        return frameOffset_;
    }

    MOsrEntry* entry() {
        return getOperand(0)->toOsrEntry();
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// MIR representation of a JSObject scope chain pointer on the OSR BaselineFrame.
// The pointer is indexed off of OsrFrameReg.
class MOsrScopeChain
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  private:
    explicit MOsrScopeChain(MOsrEntry* entry)
      : MUnaryInstruction(entry)
    {
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(OsrScopeChain)
    static MOsrScopeChain* New(TempAllocator& alloc, MOsrEntry* entry) {
        return new(alloc) MOsrScopeChain(entry);
    }

    MOsrEntry* entry() {
        return getOperand(0)->toOsrEntry();
    }
};

// MIR representation of a JSObject ArgumentsObject pointer on the OSR BaselineFrame.
// The pointer is indexed off of OsrFrameReg.
class MOsrArgumentsObject
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  private:
    explicit MOsrArgumentsObject(MOsrEntry* entry)
      : MUnaryInstruction(entry)
    {
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(OsrArgumentsObject)
    static MOsrArgumentsObject* New(TempAllocator& alloc, MOsrEntry* entry) {
        return new(alloc) MOsrArgumentsObject(entry);
    }

    MOsrEntry* entry() {
        return getOperand(0)->toOsrEntry();
    }
};

// MIR representation of the return value on the OSR BaselineFrame.
// The Value is indexed off of OsrFrameReg.
class MOsrReturnValue
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
  private:
    explicit MOsrReturnValue(MOsrEntry* entry)
      : MUnaryInstruction(entry)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(OsrReturnValue)
    static MOsrReturnValue* New(TempAllocator& alloc, MOsrEntry* entry) {
        return new(alloc) MOsrReturnValue(entry);
    }

    MOsrEntry* entry() {
        return getOperand(0)->toOsrEntry();
    }
};

class MBinarySharedStub
  : public MBinaryInstruction,
    public MixPolicy<BoxPolicy<0>, BoxPolicy<1> >::Data
{
  protected:
    explicit MBinarySharedStub(MDefinition* left, MDefinition* right)
      : MBinaryInstruction(left, right)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(BinarySharedStub)

    static MBinarySharedStub* New(TempAllocator& alloc, MDefinition* left, MDefinition* right)
    {
        return new(alloc) MBinarySharedStub(left, right);
    }

};

class MUnarySharedStub
  : public MUnaryInstruction,
    public BoxPolicy<0>::Data
{
    explicit MUnarySharedStub(MDefinition* input)
      : MUnaryInstruction(input)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(UnarySharedStub)

    static MUnarySharedStub* New(TempAllocator& alloc, MDefinition* input)
    {
        return new(alloc) MUnarySharedStub(input);
    }
};

// Check the current frame for over-recursion past the global stack limit.
class MCheckOverRecursed
  : public MNullaryInstruction
{
  public:
    INSTRUCTION_HEADER(CheckOverRecursed)

    static MCheckOverRecursed* New(TempAllocator& alloc) {
        return new(alloc) MCheckOverRecursed();
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// Check whether we need to fire the interrupt handler.
class MInterruptCheck : public MNullaryInstruction
{
    MInterruptCheck() {
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(InterruptCheck)

    static MInterruptCheck* New(TempAllocator& alloc) {
        return new(alloc) MInterruptCheck();
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// Check whether we need to fire the interrupt handler at loop headers and
// function prologues in asm.js. Generated only if we can't use implicit
// interrupt checks with signal handlers.
class MAsmJSInterruptCheck
  : public MNullaryInstruction
{
    Label* interruptExit_;
    wasm::CallSiteDesc funcDesc_;

    MAsmJSInterruptCheck(Label* interruptExit, const wasm::CallSiteDesc& funcDesc)
      : interruptExit_(interruptExit), funcDesc_(funcDesc)
    {}

  public:
    INSTRUCTION_HEADER(AsmJSInterruptCheck)

    static MAsmJSInterruptCheck* New(TempAllocator& alloc, Label* interruptExit,
                                     const wasm::CallSiteDesc& funcDesc)
    {
        return new(alloc) MAsmJSInterruptCheck(interruptExit, funcDesc);
    }
    Label* interruptExit() const {
        return interruptExit_;
    }
    const wasm::CallSiteDesc& funcDesc() const {
        return funcDesc_;
    }
};

// Checks if a value is JS_UNINITIALIZED_LEXICAL, bailout out if so, leaving
// it to baseline to throw at the correct pc.
class MLexicalCheck
  : public MUnaryInstruction,
    public BoxPolicy<0>::Data
{
    BailoutKind kind_;
    explicit MLexicalCheck(MDefinition* input, BailoutKind kind)
      : MUnaryInstruction(input),
        kind_(kind)
    {
        setResultType(MIRType_Value);
        setResultTypeSet(input->resultTypeSet());
        setMovable();
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(LexicalCheck)

    static MLexicalCheck* New(TempAllocator& alloc, MDefinition* input,
                              BailoutKind kind = Bailout_UninitializedLexical) {
        return new(alloc) MLexicalCheck(input, kind);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    MDefinition* input() const {
        return getOperand(0);
    }

    BailoutKind bailoutKind() const {
        return kind_;
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
};

// Unconditionally throw an uninitialized let error.
class MThrowRuntimeLexicalError : public MNullaryInstruction
{
    unsigned errorNumber_;

    explicit MThrowRuntimeLexicalError(unsigned errorNumber)
      : errorNumber_(errorNumber)
    {
        setGuard();
        setResultType(MIRType_None);
    }

  public:
    INSTRUCTION_HEADER(ThrowRuntimeLexicalError)

    static MThrowRuntimeLexicalError* New(TempAllocator& alloc, unsigned errorNumber) {
        return new(alloc) MThrowRuntimeLexicalError(errorNumber);
    }

    unsigned errorNumber() const {
        return errorNumber_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// In the prologues of global and eval scripts, check for redeclarations.
class MGlobalNameConflictsCheck : public MNullaryInstruction
{
    MGlobalNameConflictsCheck() {
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(GlobalNameConflictsCheck)

    static MGlobalNameConflictsCheck* New(TempAllocator& alloc) {
        return new(alloc) MGlobalNameConflictsCheck();
    }
};

// If not defined, set a global variable to |undefined|.
class MDefVar
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    CompilerPropertyName name_; // Target name to be defined.
    unsigned attrs_; // Attributes to be set.

  private:
    MDefVar(PropertyName* name, unsigned attrs, MDefinition* scopeChain)
      : MUnaryInstruction(scopeChain),
        name_(name),
        attrs_(attrs)
    {
    }

  public:
    INSTRUCTION_HEADER(DefVar)

    static MDefVar* New(TempAllocator& alloc, PropertyName* name, unsigned attrs,
                        MDefinition* scopeChain)
    {
        return new(alloc) MDefVar(name, attrs, scopeChain);
    }

    PropertyName* name() const {
        return name_;
    }
    unsigned attrs() const {
        return attrs_;
    }
    MDefinition* scopeChain() const {
        return getOperand(0);
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MDefLexical
  : public MNullaryInstruction
{
    CompilerPropertyName name_; // Target name to be defined.
    unsigned attrs_; // Attributes to be set.

  private:
    MDefLexical(PropertyName* name, unsigned attrs)
      : name_(name),
        attrs_(attrs)
    { }

  public:
    INSTRUCTION_HEADER(DefLexical)

    static MDefLexical* New(TempAllocator& alloc, PropertyName* name, unsigned attrs) {
        return new(alloc) MDefLexical(name, attrs);
    }

    PropertyName* name() const {
        return name_;
    }
    unsigned attrs() const {
        return attrs_;
    }
};

class MDefFun
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    CompilerFunction fun_;

  private:
    MDefFun(JSFunction* fun, MDefinition* scopeChain)
      : MUnaryInstruction(scopeChain),
        fun_(fun)
    {}

  public:
    INSTRUCTION_HEADER(DefFun)

    static MDefFun* New(TempAllocator& alloc, JSFunction* fun, MDefinition* scopeChain) {
        return new(alloc) MDefFun(fun, scopeChain);
    }

    JSFunction* fun() const {
        return fun_;
    }
    MDefinition* scopeChain() const {
        return getOperand(0);
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MRegExp : public MNullaryInstruction
{
    CompilerGCPointer<RegExpObject*> source_;
    bool mustClone_;

    MRegExp(CompilerConstraintList* constraints, RegExpObject* source, bool mustClone)
      : source_(source),
        mustClone_(mustClone)
    {
        setResultType(MIRType_Object);
        setResultTypeSet(MakeSingletonTypeSet(constraints, source));
    }

  public:
    INSTRUCTION_HEADER(RegExp)

    static MRegExp* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                        RegExpObject* source, bool mustClone)
    {
        return new(alloc) MRegExp(constraints, source, mustClone);
    }

    bool mustClone() const {
        return mustClone_;
    }
    RegExpObject* source() const {
        return source_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MRegExpExec
  : public MBinaryInstruction,
    public MixPolicy<ConvertToStringPolicy<0>, ObjectPolicy<1> >::Data
{
  private:

    MRegExpExec(MDefinition* regexp, MDefinition* string)
      : MBinaryInstruction(string, regexp)
    {
        // May be object or null.
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(RegExpExec)

    static MRegExpExec* New(TempAllocator& alloc, MDefinition* regexp, MDefinition* string) {
        return new(alloc) MRegExpExec(regexp, string);
    }

    MDefinition* string() const {
        return getOperand(0);
    }

    MDefinition* regexp() const {
        return getOperand(1);
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;

    bool canRecoverOnBailout() const override {
        // XXX: always return false for now, to work around bug 1132128.
        if (false && regexp()->isRegExp())
            return !regexp()->toRegExp()->source()->needUpdateLastIndex();
        return false;
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MRegExpTest
  : public MBinaryInstruction,
    public MixPolicy<ObjectPolicy<1>, ConvertToStringPolicy<0> >::Data
{
  private:

    MRegExpTest(MDefinition* regexp, MDefinition* string)
      : MBinaryInstruction(string, regexp)
    {
        setResultType(MIRType_Boolean);
    }

  public:
    INSTRUCTION_HEADER(RegExpTest)

    static MRegExpTest* New(TempAllocator& alloc, MDefinition* regexp, MDefinition* string) {
        return new(alloc) MRegExpTest(regexp, string);
    }

    MDefinition* string() const {
        return getOperand(0);
    }
    MDefinition* regexp() const {
        return getOperand(1);
    }

    bool possiblyCalls() const override {
        return true;
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        // RegExpTest has a side-effect on the regexp object's lastIndex
        // when sticky or global flags are set.
        // Return false unless we are sure it's not the case.
        // XXX: always return false for now, to work around bug 1132128.
        if (false && regexp()->isRegExp())
            return !regexp()->toRegExp()->source()->needUpdateLastIndex();
        return false;
    }
};

template <class Policy1>
class MStrReplace
  : public MTernaryInstruction,
    public Mix3Policy<StringPolicy<0>, Policy1, StringPolicy<2> >::Data
{
  protected:

    MStrReplace(MDefinition* string, MDefinition* pattern, MDefinition* replacement)
      : MTernaryInstruction(string, pattern, replacement)
    {
        setMovable();
        setResultType(MIRType_String);
    }

  public:

    MDefinition* string() const {
        return getOperand(0);
    }
    MDefinition* pattern() const {
        return getOperand(1);
    }
    MDefinition* replacement() const {
        return getOperand(2);
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MRegExpReplace
  : public MStrReplace< ObjectPolicy<1> >
{
  private:

    MRegExpReplace(MDefinition* string, MDefinition* pattern, MDefinition* replacement)
      : MStrReplace< ObjectPolicy<1> >(string, pattern, replacement)
    {
    }

  public:
    INSTRUCTION_HEADER(RegExpReplace)

    static MRegExpReplace* New(TempAllocator& alloc, MDefinition* string, MDefinition* pattern, MDefinition* replacement) {
        return new(alloc) MRegExpReplace(string, pattern, replacement);
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        // RegExpReplace will zero the lastIndex field when global flag is set.
        // So we can only remove this if it's non-global.
        // XXX: always return false for now, to work around bug 1132128.
        if (false && pattern()->isRegExp())
            return !pattern()->toRegExp()->source()->global();
        return false;
    }
};

class MStringReplace
  : public MStrReplace< StringPolicy<1> >
{
  private:

    MStringReplace(MDefinition* string, MDefinition* pattern, MDefinition* replacement)
      : MStrReplace< StringPolicy<1> >(string, pattern, replacement)
    {
    }

  public:
    INSTRUCTION_HEADER(StringReplace)

    static MStringReplace* New(TempAllocator& alloc, MDefinition* string, MDefinition* pattern, MDefinition* replacement) {
        return new(alloc) MStringReplace(string, pattern, replacement);
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        if (pattern()->isRegExp())
            return !pattern()->toRegExp()->source()->global();
        return false;
    }
};

class MSubstr
  : public MTernaryInstruction,
    public Mix3Policy<StringPolicy<0>, IntPolicy<1>, IntPolicy<2>>::Data
{
  private:

    MSubstr(MDefinition* string, MDefinition* begin, MDefinition* length)
      : MTernaryInstruction(string, begin, length)
    {
        setResultType(MIRType_String);
    }

  public:
    INSTRUCTION_HEADER(Substr)

    static MSubstr* New(TempAllocator& alloc, MDefinition* string, MDefinition* begin,
                        MDefinition* length)
    {
        return new(alloc) MSubstr(string, begin, length);
    }

    MDefinition* string() {
        return getOperand(0);
    }

    MDefinition* begin() {
        return getOperand(1);
    }

    MDefinition* length() {
        return getOperand(2);
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

struct LambdaFunctionInfo
{
    // The functions used in lambdas are the canonical original function in
    // the script, and are immutable except for delazification. Record this
    // information while still on the main thread to avoid races.
    CompilerFunction fun;
    uint16_t flags;
    uint16_t nargs;
    gc::Cell* scriptOrLazyScript;
    bool singletonType;
    bool useSingletonForClone;

    explicit LambdaFunctionInfo(JSFunction* fun)
      : fun(fun), flags(fun->flags()), nargs(fun->nargs()),
        scriptOrLazyScript(fun->hasScript()
                           ? (gc::Cell*) fun->nonLazyScript()
                           : (gc::Cell*) fun->lazyScript()),
        singletonType(fun->isSingleton()),
        useSingletonForClone(ObjectGroup::useSingletonForClone(fun))
    {}

  private:
    LambdaFunctionInfo(const LambdaFunctionInfo&) = delete;
    void operator=(const LambdaFunctionInfo&) = delete;
};

class MLambda
  : public MBinaryInstruction,
    public SingleObjectPolicy::Data
{
    const LambdaFunctionInfo info_;

    MLambda(CompilerConstraintList* constraints, MDefinition* scopeChain, MConstant* cst)
      : MBinaryInstruction(scopeChain, cst), info_(&cst->value().toObject().as<JSFunction>())
    {
        setResultType(MIRType_Object);
        if (!info().fun->isSingleton() && !ObjectGroup::useSingletonForClone(info().fun))
            setResultTypeSet(MakeSingletonTypeSet(constraints, info().fun));
    }

  public:
    INSTRUCTION_HEADER(Lambda)

    static MLambda* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                        MDefinition* scopeChain, MConstant* fun)
    {
        return new(alloc) MLambda(constraints, scopeChain, fun);
    }
    MDefinition* scopeChain() const {
        return getOperand(0);
    }
    MConstant* functionOperand() const {
        return getOperand(1)->toConstant();
    }
    const LambdaFunctionInfo& info() const {
        return info_;
    }
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }
};

class MLambdaArrow
  : public MBinaryInstruction,
    public MixPolicy<ObjectPolicy<0>, BoxPolicy<1>>::Data
{
    const LambdaFunctionInfo info_;

    MLambdaArrow(CompilerConstraintList* constraints, MDefinition* scopeChain,
                 MDefinition* newTarget_, JSFunction* fun)
      : MBinaryInstruction(scopeChain, newTarget_), info_(fun)
    {
        setResultType(MIRType_Object);
        MOZ_ASSERT(!ObjectGroup::useSingletonForClone(fun));
        if (!fun->isSingleton())
            setResultTypeSet(MakeSingletonTypeSet(constraints, fun));
    }

  public:
    INSTRUCTION_HEADER(LambdaArrow)

    static MLambdaArrow* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                             MDefinition* scopeChain, MDefinition* newTarget_, JSFunction* fun)
    {
        return new(alloc) MLambdaArrow(constraints, scopeChain, newTarget_, fun);
    }
    MDefinition* scopeChain() const {
        return getOperand(0);
    }
    MDefinition* newTargetDef() const {
        return getOperand(1);
    }
    const LambdaFunctionInfo& info() const {
        return info_;
    }
};

// Returns obj->slots.
class MSlots
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MSlots(MDefinition* object)
      : MUnaryInstruction(object)
    {
        setResultType(MIRType_Slots);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Slots)

    static MSlots* New(TempAllocator& alloc, MDefinition* object) {
        return new(alloc) MSlots(object);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }

    ALLOW_CLONE(MSlots)
};

// Returns obj->elements.
class MElements
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    bool unboxed_;

    explicit MElements(MDefinition* object, bool unboxed)
      : MUnaryInstruction(object), unboxed_(unboxed)
    {
        setResultType(MIRType_Elements);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Elements)

    static MElements* New(TempAllocator& alloc, MDefinition* object, bool unboxed = false) {
        return new(alloc) MElements(object, unboxed);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool unboxed() const {
        return unboxed_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins) &&
               ins->toElements()->unboxed() == unboxed();
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
    bool mightAlias(const MDefinition* store) const override;

    ALLOW_CLONE(MElements)
};

// A constant value for some object's typed array elements.
class MConstantElements : public MNullaryInstruction
{
    SharedMem<void*> value_;

  protected:
    explicit MConstantElements(SharedMem<void*> v)
      : value_(v)
    {
        setResultType(MIRType_Elements);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(ConstantElements)
    static MConstantElements* New(TempAllocator& alloc, SharedMem<void*> v) {
        return new(alloc) MConstantElements(v);
    }

    SharedMem<void*> value() const {
        return value_;
    }

    void printOpcode(GenericPrinter& out) const override;

    HashNumber valueHash() const override {
        return (HashNumber)(size_t) value_.asValue();
    }

    bool congruentTo(const MDefinition* ins) const override {
        return ins->isConstantElements() && ins->toConstantElements()->value() == value();
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    ALLOW_CLONE(MConstantElements)
};

// Passes through an object's elements, after ensuring it is entirely doubles.
class MConvertElementsToDoubles
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    explicit MConvertElementsToDoubles(MDefinition* elements)
      : MUnaryInstruction(elements)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Elements);
    }

  public:
    INSTRUCTION_HEADER(ConvertElementsToDoubles)

    static MConvertElementsToDoubles* New(TempAllocator& alloc, MDefinition* elements) {
        return new(alloc) MConvertElementsToDoubles(elements);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        // This instruction can read and write to the elements' contents.
        // However, it is alright to hoist this from loops which explicitly
        // read or write to the elements: such reads and writes will use double
        // values and can be reordered freely wrt this conversion, except that
        // definite double loads must follow the conversion. The latter
        // property is ensured by chaining this instruction with the elements
        // themselves, in the same manner as MBoundsCheck.
        return AliasSet::None();
    }
};

// If |elements| has the CONVERT_DOUBLE_ELEMENTS flag, convert value to
// double. Else return the original value.
class MMaybeToDoubleElement
  : public MBinaryInstruction,
    public IntPolicy<1>::Data
{
    MMaybeToDoubleElement(MDefinition* elements, MDefinition* value)
      : MBinaryInstruction(elements, value)
    {
        MOZ_ASSERT(elements->type() == MIRType_Elements);
        setMovable();
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(MaybeToDoubleElement)

    static MMaybeToDoubleElement* New(TempAllocator& alloc, MDefinition* elements,
                                      MDefinition* value)
    {
        return new(alloc) MMaybeToDoubleElement(elements, value);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* value() const {
        return getOperand(1);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
};

// Passes through an object, after ensuring its elements are not copy on write.
class MMaybeCopyElementsForWrite
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    bool checkNative_;

    explicit MMaybeCopyElementsForWrite(MDefinition* object, bool checkNative)
      : MUnaryInstruction(object), checkNative_(checkNative)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Object);
        setResultTypeSet(object->resultTypeSet());
    }

  public:
    INSTRUCTION_HEADER(MaybeCopyElementsForWrite)

    static MMaybeCopyElementsForWrite* New(TempAllocator& alloc, MDefinition* object, bool checkNative) {
        return new(alloc) MMaybeCopyElementsForWrite(object, checkNative);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool checkNative() const {
        return checkNative_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins) &&
               checkNative() == ins->toMaybeCopyElementsForWrite()->checkNative();
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::ObjectFields);
    }
#ifdef DEBUG
    bool needsResumePoint() const override {
        // This instruction is idempotent and does not change observable
        // behavior, so does not need its own resume point.
        return false;
    }
#endif

};

// Load the initialized length from an elements header.
class MInitializedLength
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    explicit MInitializedLength(MDefinition* elements)
      : MUnaryInstruction(elements)
    {
        setResultType(MIRType_Int32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(InitializedLength)

    static MInitializedLength* New(TempAllocator& alloc, MDefinition* elements) {
        return new(alloc) MInitializedLength(elements);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
    bool mightAlias(const MDefinition* store) const override;

    void computeRange(TempAllocator& alloc) override;

    ALLOW_CLONE(MInitializedLength)
};

// Store to the initialized length in an elements header. Note the input is an
// *index*, one less than the desired length.
class MSetInitializedLength
  : public MAryInstruction<2>,
    public NoTypePolicy::Data
{
    MSetInitializedLength(MDefinition* elements, MDefinition* index) {
        initOperand(0, elements);
        initOperand(1, index);
    }

  public:
    INSTRUCTION_HEADER(SetInitializedLength)

    static MSetInitializedLength* New(TempAllocator& alloc, MDefinition* elements, MDefinition* index) {
        return new(alloc) MSetInitializedLength(elements, index);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::ObjectFields);
    }

    ALLOW_CLONE(MSetInitializedLength)
};

// Load the length from an unboxed array.
class MUnboxedArrayLength
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MUnboxedArrayLength(MDefinition* object)
      : MUnaryInstruction(object)
    {
        setResultType(MIRType_Int32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(UnboxedArrayLength)

    static MUnboxedArrayLength* New(TempAllocator& alloc, MDefinition* object) {
        return new(alloc) MUnboxedArrayLength(object);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }

    ALLOW_CLONE(MUnboxedArrayLength)
};

// Load the initialized length from an unboxed array.
class MUnboxedArrayInitializedLength
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MUnboxedArrayInitializedLength(MDefinition* object)
      : MUnaryInstruction(object)
    {
        setResultType(MIRType_Int32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(UnboxedArrayInitializedLength)

    static MUnboxedArrayInitializedLength* New(TempAllocator& alloc, MDefinition* object) {
        return new(alloc) MUnboxedArrayInitializedLength(object);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
    bool mightAlias(const MDefinition* store) const override;

    ALLOW_CLONE(MUnboxedArrayInitializedLength)
};

// Increment the initialized length of an unboxed array object.
class MIncrementUnboxedArrayInitializedLength
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MIncrementUnboxedArrayInitializedLength(MDefinition* obj)
      : MUnaryInstruction(obj)
    {}

  public:
    INSTRUCTION_HEADER(IncrementUnboxedArrayInitializedLength)

    static MIncrementUnboxedArrayInitializedLength* New(TempAllocator& alloc, MDefinition* obj) {
        return new(alloc) MIncrementUnboxedArrayInitializedLength(obj);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::ObjectFields);
    }

    ALLOW_CLONE(MIncrementUnboxedArrayInitializedLength)
};

// Set the initialized length of an unboxed array object.
class MSetUnboxedArrayInitializedLength
  : public MBinaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MSetUnboxedArrayInitializedLength(MDefinition* obj, MDefinition* length)
      : MBinaryInstruction(obj, length)
    {}

  public:
    INSTRUCTION_HEADER(SetUnboxedArrayInitializedLength)

    static MSetUnboxedArrayInitializedLength* New(TempAllocator& alloc, MDefinition* obj,
                                                  MDefinition* length) {
        return new(alloc) MSetUnboxedArrayInitializedLength(obj, length);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* length() const {
        return getOperand(1);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::ObjectFields);
    }

    ALLOW_CLONE(MSetUnboxedArrayInitializedLength)
};

// Load the array length from an elements header.
class MArrayLength
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    explicit MArrayLength(MDefinition* elements)
      : MUnaryInstruction(elements)
    {
        setResultType(MIRType_Int32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(ArrayLength)

    static MArrayLength* New(TempAllocator& alloc, MDefinition* elements) {
        return new(alloc) MArrayLength(elements);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }

    void computeRange(TempAllocator& alloc) override;

    ALLOW_CLONE(MArrayLength)
};

// Store to the length in an elements header. Note the input is an *index*, one
// less than the desired length.
class MSetArrayLength
  : public MAryInstruction<2>,
    public NoTypePolicy::Data
{
    MSetArrayLength(MDefinition* elements, MDefinition* index) {
        initOperand(0, elements);
        initOperand(1, index);
    }

  public:
    INSTRUCTION_HEADER(SetArrayLength)

    static MSetArrayLength* New(TempAllocator& alloc, MDefinition* elements, MDefinition* index) {
        return new(alloc) MSetArrayLength(elements, index);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::ObjectFields);
    }
};

// Read the length of a typed array.
class MTypedArrayLength
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MTypedArrayLength(MDefinition* obj)
      : MUnaryInstruction(obj)
    {
        setResultType(MIRType_Int32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(TypedArrayLength)

    static MTypedArrayLength* New(TempAllocator& alloc, MDefinition* obj) {
        return new(alloc) MTypedArrayLength(obj);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::TypedArrayLength);
    }

    void computeRange(TempAllocator& alloc) override;
};

// Load a typed array's elements vector.
class MTypedArrayElements
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MTypedArrayElements(MDefinition* object)
      : MUnaryInstruction(object)
    {
        setResultType(MIRType_Elements);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(TypedArrayElements)

    static MTypedArrayElements* New(TempAllocator& alloc, MDefinition* object) {
        return new(alloc) MTypedArrayElements(object);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }

    ALLOW_CLONE(MTypedArrayElements)
};

class MSetDisjointTypedElements
  : public MTernaryInstruction,
    public NoTypePolicy::Data
{
    explicit MSetDisjointTypedElements(MDefinition* target, MDefinition* targetOffset,
                                       MDefinition* source)
      : MTernaryInstruction(target, targetOffset, source)
    {
        MOZ_ASSERT(target->type() == MIRType_Object);
        MOZ_ASSERT(targetOffset->type() == MIRType_Int32);
        MOZ_ASSERT(source->type() == MIRType_Object);
        setResultType(MIRType_None);
    }

  public:
    INSTRUCTION_HEADER(SetDisjointTypedElements)

    static MSetDisjointTypedElements*
    New(TempAllocator& alloc, MDefinition* target, MDefinition* targetOffset,
        MDefinition* source)
    {
        return new(alloc) MSetDisjointTypedElements(target, targetOffset, source);
    }

    MDefinition* target() const {
        return getOperand(0);
    }

    MDefinition* targetOffset() const {
        return getOperand(1);
    }

    MDefinition* source() const {
        return getOperand(2);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }

    ALLOW_CLONE(MSetDisjointTypedElements)
};

// Load a binary data object's "elements", which is just its opaque
// binary data space. Eventually this should probably be
// unified with `MTypedArrayElements`.
class MTypedObjectElements
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    bool definitelyOutline_;

  private:
    explicit MTypedObjectElements(MDefinition* object, bool definitelyOutline)
      : MUnaryInstruction(object),
        definitelyOutline_(definitelyOutline)
    {
        setResultType(MIRType_Elements);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(TypedObjectElements)

    static MTypedObjectElements* New(TempAllocator& alloc, MDefinition* object,
                                     bool definitelyOutline) {
        return new(alloc) MTypedObjectElements(object, definitelyOutline);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool definitelyOutline() const {
        return definitelyOutline_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isTypedObjectElements())
            return false;
        const MTypedObjectElements* other = ins->toTypedObjectElements();
        if (other->definitelyOutline() != definitelyOutline())
            return false;
        return congruentIfOperandsEqual(other);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
};

// Inlined version of the js::SetTypedObjectOffset() intrinsic.
class MSetTypedObjectOffset
  : public MBinaryInstruction,
    public NoTypePolicy::Data
{
  private:
    MSetTypedObjectOffset(MDefinition* object, MDefinition* offset)
      : MBinaryInstruction(object, offset)
    {
        MOZ_ASSERT(object->type() == MIRType_Object);
        MOZ_ASSERT(offset->type() == MIRType_Int32);
        setResultType(MIRType_None);
    }

  public:
    INSTRUCTION_HEADER(SetTypedObjectOffset)

    static MSetTypedObjectOffset* New(TempAllocator& alloc,
                                      MDefinition* object,
                                      MDefinition* offset)
    {
        return new(alloc) MSetTypedObjectOffset(object, offset);
    }

    MDefinition* object() const {
        return getOperand(0);
    }

    MDefinition* offset() const {
        return getOperand(1);
    }

    AliasSet getAliasSet() const override {
        // This affects the result of MTypedObjectElements,
        // which is described as a load of ObjectFields.
        return AliasSet::Store(AliasSet::ObjectFields);
    }
};

class MKeepAliveObject
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MKeepAliveObject(MDefinition* object)
      : MUnaryInstruction(object)
    {
        setResultType(MIRType_None);
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(KeepAliveObject)

    static MKeepAliveObject* New(TempAllocator& alloc, MDefinition* object) {
        return new(alloc) MKeepAliveObject(object);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
};

// Perform !-operation
class MNot
  : public MUnaryInstruction,
    public TestPolicy::Data
{
    bool operandMightEmulateUndefined_;
    bool operandIsNeverNaN_;

    explicit MNot(MDefinition* input, CompilerConstraintList* constraints = nullptr)
      : MUnaryInstruction(input),
        operandMightEmulateUndefined_(true),
        operandIsNeverNaN_(false)
    {
        setResultType(MIRType_Boolean);
        setMovable();
        if (constraints)
            cacheOperandMightEmulateUndefined(constraints);
    }

    void cacheOperandMightEmulateUndefined(CompilerConstraintList* constraints);

  public:
    static MNot* New(TempAllocator& alloc, MDefinition* elements,
                     CompilerConstraintList* constraints = nullptr)
    {
        return new(alloc) MNot(elements, constraints);
    }
    static MNot* NewAsmJS(TempAllocator& alloc, MDefinition* elements) {
        MNot* ins = new(alloc) MNot(elements);
        ins->setResultType(MIRType_Int32);
        return ins;
    }

    INSTRUCTION_HEADER(Not)

    MDefinition* foldsTo(TempAllocator& alloc) override;

    void markNoOperandEmulatesUndefined() {
        operandMightEmulateUndefined_ = false;
    }
    bool operandMightEmulateUndefined() const {
        return operandMightEmulateUndefined_;
    }
    bool operandIsNeverNaN() const {
        return operandIsNeverNaN_;
    }

    virtual AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    void collectRangeInfoPreTrunc() override;

    void trySpecializeFloat32(TempAllocator& alloc) override;
    bool isFloat32Commutative() const override { return true; }
#ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override {
        return true;
    }
#endif
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }
};

// Bailout if index + minimum < 0 or index + maximum >= length. The length used
// in a bounds check must not be negative, or the wrong result may be computed
// (unsigned comparisons may be used).
class MBoundsCheck
  : public MBinaryInstruction,
    public NoTypePolicy::Data
{
    // Range over which to perform the bounds check, may be modified by GVN.
    int32_t minimum_;
    int32_t maximum_;
    bool fallible_;

    MBoundsCheck(MDefinition* index, MDefinition* length)
      : MBinaryInstruction(index, length), minimum_(0), maximum_(0), fallible_(true)
    {
        setGuard();
        setMovable();
        MOZ_ASSERT(index->type() == MIRType_Int32);
        MOZ_ASSERT(length->type() == MIRType_Int32);

        // Returns the checked index.
        setResultType(MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(BoundsCheck)

    static MBoundsCheck* New(TempAllocator& alloc, MDefinition* index, MDefinition* length) {
        return new(alloc) MBoundsCheck(index, length);
    }
    MDefinition* index() const {
        return getOperand(0);
    }
    MDefinition* length() const {
        return getOperand(1);
    }
    int32_t minimum() const {
        return minimum_;
    }
    void setMinimum(int32_t n) {
        minimum_ = n;
    }
    int32_t maximum() const {
        return maximum_;
    }
    void setMaximum(int32_t n) {
        maximum_ = n;
    }
    MDefinition* foldsTo(TempAllocator& alloc) override;
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isBoundsCheck())
            return false;
        const MBoundsCheck* other = ins->toBoundsCheck();
        if (minimum() != other->minimum() || maximum() != other->maximum())
            return false;
        return congruentIfOperandsEqual(other);
    }
    virtual AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    void computeRange(TempAllocator& alloc) override;
    bool fallible() const {
        return fallible_;
    }
    void collectRangeInfoPreTrunc() override;

    ALLOW_CLONE(MBoundsCheck)
};

// Bailout if index < minimum.
class MBoundsCheckLower
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    int32_t minimum_;
    bool fallible_;

    explicit MBoundsCheckLower(MDefinition* index)
      : MUnaryInstruction(index), minimum_(0), fallible_(true)
    {
        setGuard();
        setMovable();
        MOZ_ASSERT(index->type() == MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(BoundsCheckLower)

    static MBoundsCheckLower* New(TempAllocator& alloc, MDefinition* index) {
        return new(alloc) MBoundsCheckLower(index);
    }

    MDefinition* index() const {
        return getOperand(0);
    }
    int32_t minimum() const {
        return minimum_;
    }
    void setMinimum(int32_t n) {
        minimum_ = n;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool fallible() const {
        return fallible_;
    }
    void collectRangeInfoPreTrunc() override;
};

// Instructions which access an object's elements can either do so on a
// definition accessing that elements pointer, or on the object itself, if its
// elements are inline. In the latter case there must be an offset associated
// with the access.
static inline bool
IsValidElementsType(MDefinition* elements, int32_t offsetAdjustment)
{
    return elements->type() == MIRType_Elements ||
           (elements->type() == MIRType_Object && offsetAdjustment != 0);
}

// Load a value from a dense array's element vector and does a hole check if the
// array is not known to be packed.
class MLoadElement
  : public MBinaryInstruction,
    public SingleObjectPolicy::Data
{
    bool needsHoleCheck_;
    bool loadDoubles_;
    int32_t offsetAdjustment_;

    MLoadElement(MDefinition* elements, MDefinition* index,
                 bool needsHoleCheck, bool loadDoubles, int32_t offsetAdjustment)
      : MBinaryInstruction(elements, index),
        needsHoleCheck_(needsHoleCheck),
        loadDoubles_(loadDoubles),
        offsetAdjustment_(offsetAdjustment)
    {
        if (needsHoleCheck) {
            // Uses may be optimized away based on this instruction's result
            // type. This means it's invalid to DCE this instruction, as we
            // have to invalidate when we read a hole.
            setGuard();
        }
        setResultType(MIRType_Value);
        setMovable();
        MOZ_ASSERT(IsValidElementsType(elements, offsetAdjustment));
        MOZ_ASSERT(index->type() == MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(LoadElement)

    static MLoadElement* New(TempAllocator& alloc, MDefinition* elements, MDefinition* index,
                             bool needsHoleCheck, bool loadDoubles, int32_t offsetAdjustment = 0) {
        return new(alloc) MLoadElement(elements, index, needsHoleCheck, loadDoubles, offsetAdjustment);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    bool needsHoleCheck() const {
        return needsHoleCheck_;
    }
    bool loadDoubles() const {
        return loadDoubles_;
    }
    int32_t offsetAdjustment() const {
        return offsetAdjustment_;
    }
    bool fallible() const {
        return needsHoleCheck();
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isLoadElement())
            return false;
        const MLoadElement* other = ins->toLoadElement();
        if (needsHoleCheck() != other->needsHoleCheck())
            return false;
        if (loadDoubles() != other->loadDoubles())
            return false;
        if (offsetAdjustment() != other->offsetAdjustment())
            return false;
        return congruentIfOperandsEqual(other);
    }
    MDefinition* foldsTo(TempAllocator& alloc) override;
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::Element);
    }
    bool mightAlias(const MDefinition* store) const override;

    ALLOW_CLONE(MLoadElement)
};

// Load a value from the elements vector for a dense native or unboxed array.
// If the index is out-of-bounds, or the indexed slot has a hole, undefined is
// returned instead.
class MLoadElementHole
  : public MTernaryInstruction,
    public SingleObjectPolicy::Data
{
    // Unboxed element type, JSVAL_TYPE_MAGIC for dense native elements.
    JSValueType unboxedType_;

    bool needsNegativeIntCheck_;
    bool needsHoleCheck_;

    MLoadElementHole(MDefinition* elements, MDefinition* index, MDefinition* initLength,
                     JSValueType unboxedType, bool needsHoleCheck)
      : MTernaryInstruction(elements, index, initLength),
        unboxedType_(unboxedType),
        needsNegativeIntCheck_(true),
        needsHoleCheck_(needsHoleCheck)
    {
        setResultType(MIRType_Value);
        setMovable();

        // Set the guard flag to make sure we bail when we see a negative
        // index. We can clear this flag (and needsNegativeIntCheck_) in
        // collectRangeInfoPreTrunc.
        setGuard();

        MOZ_ASSERT(elements->type() == MIRType_Elements);
        MOZ_ASSERT(index->type() == MIRType_Int32);
        MOZ_ASSERT(initLength->type() == MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(LoadElementHole)

    static MLoadElementHole* New(TempAllocator& alloc, MDefinition* elements, MDefinition* index,
                                 MDefinition* initLength, JSValueType unboxedType,
                                 bool needsHoleCheck) {
        return new(alloc) MLoadElementHole(elements, index, initLength,
                                           unboxedType, needsHoleCheck);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    MDefinition* initLength() const {
        return getOperand(2);
    }
    JSValueType unboxedType() const {
        return unboxedType_;
    }
    bool needsNegativeIntCheck() const {
        return needsNegativeIntCheck_;
    }
    bool needsHoleCheck() const {
        return needsHoleCheck_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isLoadElementHole())
            return false;
        const MLoadElementHole* other = ins->toLoadElementHole();
        if (unboxedType() != other->unboxedType())
            return false;
        if (needsHoleCheck() != other->needsHoleCheck())
            return false;
        if (needsNegativeIntCheck() != other->needsNegativeIntCheck())
            return false;
        return congruentIfOperandsEqual(other);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::BoxedOrUnboxedElements(unboxedType()));
    }
    void collectRangeInfoPreTrunc() override;

    ALLOW_CLONE(MLoadElementHole)
};

class MLoadUnboxedObjectOrNull
  : public MBinaryInstruction,
    public SingleObjectPolicy::Data
{
  public:
    enum NullBehavior {
        HandleNull,
        BailOnNull,
        NullNotPossible
    };

  private:
    NullBehavior nullBehavior_;
    int32_t offsetAdjustment_;

    MLoadUnboxedObjectOrNull(MDefinition* elements, MDefinition* index,
                             NullBehavior nullBehavior, int32_t offsetAdjustment)
      : MBinaryInstruction(elements, index),
        nullBehavior_(nullBehavior),
        offsetAdjustment_(offsetAdjustment)
    {
        if (nullBehavior == BailOnNull) {
            // Don't eliminate loads which bail out on a null pointer, for the
            // same reason as MLoadElement.
            setGuard();
        }
        setResultType(nullBehavior == HandleNull ? MIRType_Value : MIRType_Object);
        setMovable();
        MOZ_ASSERT(IsValidElementsType(elements, offsetAdjustment));
        MOZ_ASSERT(index->type() == MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(LoadUnboxedObjectOrNull)

    static MLoadUnboxedObjectOrNull* New(TempAllocator& alloc,
                                         MDefinition* elements, MDefinition* index,
                                         NullBehavior nullBehavior, int32_t offsetAdjustment) {
        return new(alloc) MLoadUnboxedObjectOrNull(elements, index, nullBehavior,
                                                   offsetAdjustment);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    NullBehavior nullBehavior() const {
        return nullBehavior_;
    }
    int32_t offsetAdjustment() const {
        return offsetAdjustment_;
    }
    bool fallible() const {
        return nullBehavior() == BailOnNull;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isLoadUnboxedObjectOrNull())
            return false;
        const MLoadUnboxedObjectOrNull* other = ins->toLoadUnboxedObjectOrNull();
        if (nullBehavior() != other->nullBehavior())
            return false;
        if (offsetAdjustment() != other->offsetAdjustment())
            return false;
        return congruentIfOperandsEqual(other);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::UnboxedElement);
    }
    bool mightAlias(const MDefinition* store) const override;

    ALLOW_CLONE(MLoadUnboxedObjectOrNull)
};

class MLoadUnboxedString
  : public MBinaryInstruction,
    public SingleObjectPolicy::Data
{
    int32_t offsetAdjustment_;

    MLoadUnboxedString(MDefinition* elements, MDefinition* index, int32_t offsetAdjustment)
      : MBinaryInstruction(elements, index),
        offsetAdjustment_(offsetAdjustment)
    {
        setResultType(MIRType_String);
        setMovable();
        MOZ_ASSERT(IsValidElementsType(elements, offsetAdjustment));
        MOZ_ASSERT(index->type() == MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(LoadUnboxedString)

    static MLoadUnboxedString* New(TempAllocator& alloc,
                                   MDefinition* elements, MDefinition* index,
                                   int32_t offsetAdjustment = 0) {
        return new(alloc) MLoadUnboxedString(elements, index, offsetAdjustment);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    int32_t offsetAdjustment() const {
        return offsetAdjustment_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isLoadUnboxedString())
            return false;
        const MLoadUnboxedString* other = ins->toLoadUnboxedString();
        if (offsetAdjustment() != other->offsetAdjustment())
            return false;
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::UnboxedElement);
    }
    bool mightAlias(const MDefinition* store) const override;

    ALLOW_CLONE(MLoadUnboxedString)
};

class MStoreElementCommon
{
    MIRType elementType_;
    bool needsBarrier_;

  protected:
    MStoreElementCommon()
      : elementType_(MIRType_Value),
        needsBarrier_(false)
    { }

  public:
    MIRType elementType() const {
        return elementType_;
    }
    void setElementType(MIRType elementType) {
        MOZ_ASSERT(elementType != MIRType_None);
        elementType_ = elementType;
    }
    bool needsBarrier() const {
        return needsBarrier_;
    }
    void setNeedsBarrier() {
        needsBarrier_ = true;
    }
};

// Store a value to a dense array slots vector.
class MStoreElement
  : public MAryInstruction<3>,
    public MStoreElementCommon,
    public MixPolicy<SingleObjectPolicy, NoFloatPolicy<2> >::Data
{
    bool needsHoleCheck_;
    int32_t offsetAdjustment_;

    MStoreElement(MDefinition* elements, MDefinition* index, MDefinition* value,
                  bool needsHoleCheck, int32_t offsetAdjustment) {
        initOperand(0, elements);
        initOperand(1, index);
        initOperand(2, value);
        needsHoleCheck_ = needsHoleCheck;
        offsetAdjustment_ = offsetAdjustment;
        MOZ_ASSERT(IsValidElementsType(elements, offsetAdjustment));
        MOZ_ASSERT(index->type() == MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(StoreElement)

    static MStoreElement* New(TempAllocator& alloc, MDefinition* elements, MDefinition* index,
                              MDefinition* value,
                              bool needsHoleCheck, int32_t offsetAdjustment = 0) {
        return new(alloc) MStoreElement(elements, index, value, needsHoleCheck, offsetAdjustment);
    }
    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    MDefinition* value() const {
        return getOperand(2);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::Element);
    }
    bool needsHoleCheck() const {
        return needsHoleCheck_;
    }
    int32_t offsetAdjustment() const {
        return offsetAdjustment_;
    }
    bool fallible() const {
        return needsHoleCheck();
    }

    ALLOW_CLONE(MStoreElement)
};

// Like MStoreElement, but supports indexes >= initialized length, and can
// handle unboxed arrays. The downside is that we cannot hoist the elements
// vector and bounds check, since this instruction may update the (initialized)
// length and reallocate the elements vector.
class MStoreElementHole
  : public MAryInstruction<4>,
    public MStoreElementCommon,
    public MixPolicy<SingleObjectPolicy, NoFloatPolicy<3> >::Data
{
    JSValueType unboxedType_;

    MStoreElementHole(MDefinition* object, MDefinition* elements,
                      MDefinition* index, MDefinition* value, JSValueType unboxedType)
      : unboxedType_(unboxedType)
    {
        initOperand(0, object);
        initOperand(1, elements);
        initOperand(2, index);
        initOperand(3, value);
        MOZ_ASSERT(elements->type() == MIRType_Elements);
        MOZ_ASSERT(index->type() == MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(StoreElementHole)

    static MStoreElementHole* New(TempAllocator& alloc, MDefinition* object, MDefinition* elements,
                                  MDefinition* index, MDefinition* value, JSValueType unboxedType) {
        return new(alloc) MStoreElementHole(object, elements, index, value, unboxedType);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* elements() const {
        return getOperand(1);
    }
    MDefinition* index() const {
        return getOperand(2);
    }
    MDefinition* value() const {
        return getOperand(3);
    }
    JSValueType unboxedType() const {
        return unboxedType_;
    }
    AliasSet getAliasSet() const override {
        // StoreElementHole can update the initialized length, the array length
        // or reallocate obj->elements.
        return AliasSet::Store(AliasSet::ObjectFields |
                               AliasSet::BoxedOrUnboxedElements(unboxedType()));
    }

    ALLOW_CLONE(MStoreElementHole)
};

// Store an unboxed object or null pointer to a v\ector.
class MStoreUnboxedObjectOrNull
  : public MAryInstruction<4>,
    public StoreUnboxedObjectOrNullPolicy::Data
{
    int32_t offsetAdjustment_;
    bool preBarrier_;

    MStoreUnboxedObjectOrNull(MDefinition* elements, MDefinition* index,
                              MDefinition* value, MDefinition* typedObj,
                              int32_t offsetAdjustment, bool preBarrier)
      : offsetAdjustment_(offsetAdjustment), preBarrier_(preBarrier)
    {
        initOperand(0, elements);
        initOperand(1, index);
        initOperand(2, value);
        initOperand(3, typedObj);
        MOZ_ASSERT(IsValidElementsType(elements, offsetAdjustment));
        MOZ_ASSERT(index->type() == MIRType_Int32);
        MOZ_ASSERT(typedObj->type() == MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(StoreUnboxedObjectOrNull)

    static MStoreUnboxedObjectOrNull* New(TempAllocator& alloc,
                                          MDefinition* elements, MDefinition* index,
                                          MDefinition* value, MDefinition* typedObj,
                                          int32_t offsetAdjustment = 0,
                                          bool preBarrier = true) {
        return new(alloc) MStoreUnboxedObjectOrNull(elements, index, value, typedObj,
                                                    offsetAdjustment, preBarrier);
    }
    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    MDefinition* value() const {
        return getOperand(2);
    }
    MDefinition* typedObj() const {
        return getOperand(3);
    }
    int32_t offsetAdjustment() const {
        return offsetAdjustment_;
    }
    bool preBarrier() const {
        return preBarrier_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }

    // For StoreUnboxedObjectOrNullPolicy.
    void setValue(MDefinition* def) {
        replaceOperand(2, def);
    }

    ALLOW_CLONE(MStoreUnboxedObjectOrNull)
};

// Store an unboxed object or null pointer to a vector.
class MStoreUnboxedString
  : public MAryInstruction<3>,
    public MixPolicy<SingleObjectPolicy, ConvertToStringPolicy<2> >::Data
{
    int32_t offsetAdjustment_;
    bool preBarrier_;

    MStoreUnboxedString(MDefinition* elements, MDefinition* index, MDefinition* value,
                        int32_t offsetAdjustment, bool preBarrier)
      : offsetAdjustment_(offsetAdjustment), preBarrier_(preBarrier)
    {
        initOperand(0, elements);
        initOperand(1, index);
        initOperand(2, value);
        MOZ_ASSERT(IsValidElementsType(elements, offsetAdjustment));
        MOZ_ASSERT(index->type() == MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(StoreUnboxedString)

    static MStoreUnboxedString* New(TempAllocator& alloc,
                                    MDefinition* elements, MDefinition* index,
                                    MDefinition* value, int32_t offsetAdjustment = 0,
                                    bool preBarrier = true) {
        return new(alloc) MStoreUnboxedString(elements, index, value,
                                              offsetAdjustment, preBarrier);
    }
    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    MDefinition* value() const {
        return getOperand(2);
    }
    int32_t offsetAdjustment() const {
        return offsetAdjustment_;
    }
    bool preBarrier() const {
        return preBarrier_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }

    ALLOW_CLONE(MStoreUnboxedString)
};

// Passes through an object, after ensuring it is converted from an unboxed
// object to a native representation.
class MConvertUnboxedObjectToNative
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    CompilerObjectGroup group_;

    explicit MConvertUnboxedObjectToNative(MDefinition* obj, ObjectGroup* group)
      : MUnaryInstruction(obj),
        group_(group)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(ConvertUnboxedObjectToNative)

    static MConvertUnboxedObjectToNative* New(TempAllocator& alloc, MDefinition* obj,
                                              ObjectGroup* group);

    MDefinition* object() const {
        return getOperand(0);
    }
    ObjectGroup* group() const {
        return group_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!congruentIfOperandsEqual(ins))
            return false;
        return ins->toConvertUnboxedObjectToNative()->group() == group();
    }
    AliasSet getAliasSet() const override {
        // This instruction can read and write to all parts of the object, but
        // is marked as non-effectful so it can be consolidated by LICM and GVN
        // and avoid inhibiting other optimizations.
        //
        // This is valid to do because when unboxed objects might have a native
        // group they can be converted to, we do not optimize accesses to the
        // unboxed objects and do not guard on their group or shape (other than
        // in this opcode).
        //
        // Later accesses can assume the object has a native representation
        // and optimize accordingly. Those accesses cannot be reordered before
        // this instruction, however. This is prevented by chaining this
        // instruction with the object itself, in the same way as MBoundsCheck.
        return AliasSet::None();
    }
};

// Array.prototype.pop or Array.prototype.shift on a dense array.
class MArrayPopShift
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
  public:
    enum Mode {
        Pop,
        Shift
    };

  private:
    Mode mode_;
    JSValueType unboxedType_;
    bool needsHoleCheck_;
    bool maybeUndefined_;

    MArrayPopShift(MDefinition* object, Mode mode, JSValueType unboxedType,
                   bool needsHoleCheck, bool maybeUndefined)
      : MUnaryInstruction(object), mode_(mode), unboxedType_(unboxedType),
        needsHoleCheck_(needsHoleCheck), maybeUndefined_(maybeUndefined)
    { }

  public:
    INSTRUCTION_HEADER(ArrayPopShift)

    static MArrayPopShift* New(TempAllocator& alloc, MDefinition* object, Mode mode,
                               JSValueType unboxedType, bool needsHoleCheck, bool maybeUndefined)
    {
        return new(alloc) MArrayPopShift(object, mode, unboxedType, needsHoleCheck, maybeUndefined);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool needsHoleCheck() const {
        return needsHoleCheck_;
    }
    bool maybeUndefined() const {
        return maybeUndefined_;
    }
    bool mode() const {
        return mode_;
    }
    JSValueType unboxedType() const {
        return unboxedType_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::ObjectFields |
                               AliasSet::BoxedOrUnboxedElements(unboxedType()));
    }

    ALLOW_CLONE(MArrayPopShift)
};

// Array.prototype.push on a dense array. Returns the new array length.
class MArrayPush
  : public MBinaryInstruction,
    public MixPolicy<SingleObjectPolicy, NoFloatPolicy<1> >::Data
{
    JSValueType unboxedType_;

    MArrayPush(MDefinition* object, MDefinition* value, JSValueType unboxedType)
      : MBinaryInstruction(object, value), unboxedType_(unboxedType)
    {
        setResultType(MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(ArrayPush)

    static MArrayPush* New(TempAllocator& alloc, MDefinition* object, MDefinition* value,
                           JSValueType unboxedType) {
        return new(alloc) MArrayPush(object, value, unboxedType);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* value() const {
        return getOperand(1);
    }
    JSValueType unboxedType() const {
        return unboxedType_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::ObjectFields |
                               AliasSet::BoxedOrUnboxedElements(unboxedType()));
    }
    void computeRange(TempAllocator& alloc) override;

    ALLOW_CLONE(MArrayPush)
};

// Array.prototype.concat on two dense arrays.
class MArrayConcat
  : public MBinaryInstruction,
    public MixPolicy<ObjectPolicy<0>, ObjectPolicy<1> >::Data
{
    CompilerObject templateObj_;
    gc::InitialHeap initialHeap_;
    bool unboxedThis_, unboxedArg_;

    MArrayConcat(CompilerConstraintList* constraints, MDefinition* lhs, MDefinition* rhs,
                 JSObject* templateObj, gc::InitialHeap initialHeap,
                 bool unboxedThis, bool unboxedArg)
      : MBinaryInstruction(lhs, rhs),
        templateObj_(templateObj),
        initialHeap_(initialHeap),
        unboxedThis_(unboxedThis),
        unboxedArg_(unboxedArg)
    {
        setResultType(MIRType_Object);
        setResultTypeSet(MakeSingletonTypeSet(constraints, templateObj));
    }

  public:
    INSTRUCTION_HEADER(ArrayConcat)

    static MArrayConcat* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                             MDefinition* lhs, MDefinition* rhs,
                             JSObject* templateObj, gc::InitialHeap initialHeap,
                             bool unboxedThis, bool unboxedArg)
    {
        return new(alloc) MArrayConcat(constraints, lhs, rhs, templateObj,
                                       initialHeap, unboxedThis, unboxedArg);
    }

    JSObject* templateObj() const {
        return templateObj_;
    }

    gc::InitialHeap initialHeap() const {
        return initialHeap_;
    }

    bool unboxedThis() const {
        return unboxedThis_;
    }

    bool unboxedArg() const {
        return unboxedArg_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::BoxedOrUnboxedElements(unboxedThis() ? JSVAL_TYPE_INT32 : JSVAL_TYPE_MAGIC) |
                               AliasSet::BoxedOrUnboxedElements(unboxedArg() ? JSVAL_TYPE_INT32 : JSVAL_TYPE_MAGIC) |
                               AliasSet::ObjectFields);
    }
    bool possiblyCalls() const override {
        return true;
    }
};

// Array.prototype.slice on a dense array.
class MArraySlice
  : public MTernaryInstruction,
    public Mix3Policy<ObjectPolicy<0>, IntPolicy<1>, IntPolicy<2>>::Data
{
    CompilerObject templateObj_;
    gc::InitialHeap initialHeap_;
    JSValueType unboxedType_;

    MArraySlice(CompilerConstraintList* constraints, MDefinition* obj,
                MDefinition* begin, MDefinition* end,
                JSObject* templateObj, gc::InitialHeap initialHeap, JSValueType unboxedType)
      : MTernaryInstruction(obj, begin, end),
        templateObj_(templateObj),
        initialHeap_(initialHeap),
        unboxedType_(unboxedType)
    {
        setResultType(MIRType_Object);
        setResultTypeSet(obj->resultTypeSet());
    }

  public:
    INSTRUCTION_HEADER(ArraySlice)

    static MArraySlice* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                            MDefinition* obj, MDefinition* begin, MDefinition* end,
                            JSObject* templateObj, gc::InitialHeap initialHeap,
                            JSValueType unboxedType)
    {
        return new(alloc) MArraySlice(constraints, obj, begin, end, templateObj,
                                      initialHeap, unboxedType);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* begin() const {
        return getOperand(1);
    }
    MDefinition* end() const {
        return getOperand(2);
    }

    JSObject* templateObj() const {
        return templateObj_;
    }

    gc::InitialHeap initialHeap() const {
        return initialHeap_;
    }

    JSValueType unboxedType() const {
        return unboxedType_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::BoxedOrUnboxedElements(unboxedType()) |
                               AliasSet::ObjectFields);
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MArrayJoin
    : public MBinaryInstruction,
      public MixPolicy<ObjectPolicy<0>, StringPolicy<1> >::Data
{
    MArrayJoin(MDefinition* array, MDefinition* sep)
        : MBinaryInstruction(array, sep)
    {
        setResultType(MIRType_String);
    }
  public:
    INSTRUCTION_HEADER(ArrayJoin)
    static MArrayJoin* New(TempAllocator& alloc, MDefinition* array, MDefinition* sep)
    {
        return new (alloc) MArrayJoin(array, sep);
    }
    MDefinition* array() const {
        return getOperand(0);
    }
    MDefinition* sep() const {
        return getOperand(1);
    }
    bool possiblyCalls() const override {
        return true;
    }
    virtual AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::Element | AliasSet::ObjectFields);
    }
    MDefinition* foldsTo(TempAllocator& alloc) override;
};

// See comments above MMemoryBarrier, below.

enum MemoryBarrierRequirement
{
    DoesNotRequireMemoryBarrier,
    DoesRequireMemoryBarrier
};

// Also see comments above MMemoryBarrier, below.

// Load an unboxed scalar value from a typed array or other object.
class MLoadUnboxedScalar
  : public MBinaryInstruction,
    public SingleObjectPolicy::Data
{
    Scalar::Type storageType_;
    Scalar::Type readType_;
    unsigned numElems_; // used only for SIMD
    bool requiresBarrier_;
    int32_t offsetAdjustment_;
    bool canonicalizeDoubles_;

    MLoadUnboxedScalar(MDefinition* elements, MDefinition* index,
                       Scalar::Type storageType, MemoryBarrierRequirement requiresBarrier,
                       int32_t offsetAdjustment, bool canonicalizeDoubles)
      : MBinaryInstruction(elements, index),
        storageType_(storageType),
        readType_(storageType),
        numElems_(1),
        requiresBarrier_(requiresBarrier == DoesRequireMemoryBarrier),
        offsetAdjustment_(offsetAdjustment),
        canonicalizeDoubles_(canonicalizeDoubles)
    {
        setResultType(MIRType_Value);
        if (requiresBarrier_)
            setGuard();         // Not removable or movable
        else
            setMovable();
        MOZ_ASSERT(IsValidElementsType(elements, offsetAdjustment));
        MOZ_ASSERT(index->type() == MIRType_Int32);
        MOZ_ASSERT(storageType >= 0 && storageType < Scalar::MaxTypedArrayViewType);
    }

  public:
    INSTRUCTION_HEADER(LoadUnboxedScalar)

    static MLoadUnboxedScalar* New(TempAllocator& alloc, MDefinition* elements, MDefinition* index,
                                   Scalar::Type storageType,
                                   MemoryBarrierRequirement requiresBarrier
                                       = DoesNotRequireMemoryBarrier,
                                   int32_t offsetAdjustment = 0,
                                   bool canonicalizeDoubles = true)
    {
        return new(alloc) MLoadUnboxedScalar(elements, index, storageType,
                                             requiresBarrier, offsetAdjustment,
                                             canonicalizeDoubles);
    }

    void setSimdRead(Scalar::Type type, unsigned numElems) {
        readType_ = type;
        numElems_ = numElems;
    }
    unsigned numElems() const {
        return numElems_;
    }
    Scalar::Type readType() const {
        return readType_;
    }

    Scalar::Type storageType() const {
        return storageType_;
    }
    bool fallible() const {
        // Bailout if the result does not fit in an int32.
        return readType_ == Scalar::Uint32 && type() == MIRType_Int32;
    }
    bool requiresMemoryBarrier() const {
        return requiresBarrier_;
    }
    bool canonicalizeDoubles() const {
        return canonicalizeDoubles_;
    }
    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    int32_t offsetAdjustment() const {
        return offsetAdjustment_;
    }
    AliasSet getAliasSet() const override {
        // When a barrier is needed make the instruction effectful by
        // giving it a "store" effect.
        if (requiresBarrier_)
            return AliasSet::Store(AliasSet::UnboxedElement);
        return AliasSet::Load(AliasSet::UnboxedElement);
    }
    bool mightAlias(const MDefinition* store) const override;

    bool congruentTo(const MDefinition* ins) const override {
        if (requiresBarrier_)
            return false;
        if (!ins->isLoadUnboxedScalar())
            return false;
        const MLoadUnboxedScalar* other = ins->toLoadUnboxedScalar();
        if (storageType_ != other->storageType_)
            return false;
        if (readType_ != other->readType_)
            return false;
        if (numElems_ != other->numElems_)
            return false;
        if (offsetAdjustment() != other->offsetAdjustment())
            return false;
        if (canonicalizeDoubles() != other->canonicalizeDoubles())
            return false;
        return congruentIfOperandsEqual(other);
    }

    void printOpcode(GenericPrinter& out) const override;

    void computeRange(TempAllocator& alloc) override;

    bool canProduceFloat32() const override { return storageType_ == Scalar::Float32; }

    ALLOW_CLONE(MLoadUnboxedScalar)
};

// Load a value from a typed array. Out-of-bounds accesses are handled in-line.
class MLoadTypedArrayElementHole
  : public MBinaryInstruction,
    public SingleObjectPolicy::Data
{
    Scalar::Type arrayType_;
    bool allowDouble_;

    MLoadTypedArrayElementHole(MDefinition* object, MDefinition* index, Scalar::Type arrayType, bool allowDouble)
      : MBinaryInstruction(object, index), arrayType_(arrayType), allowDouble_(allowDouble)
    {
        setResultType(MIRType_Value);
        setMovable();
        MOZ_ASSERT(index->type() == MIRType_Int32);
        MOZ_ASSERT(arrayType >= 0 && arrayType < Scalar::MaxTypedArrayViewType);
    }

  public:
    INSTRUCTION_HEADER(LoadTypedArrayElementHole)

    static MLoadTypedArrayElementHole* New(TempAllocator& alloc, MDefinition* object, MDefinition* index,
                                           Scalar::Type arrayType, bool allowDouble)
    {
        return new(alloc) MLoadTypedArrayElementHole(object, index, arrayType, allowDouble);
    }

    Scalar::Type arrayType() const {
        return arrayType_;
    }
    bool allowDouble() const {
        return allowDouble_;
    }
    bool fallible() const {
        return arrayType_ == Scalar::Uint32 && !allowDouble_;
    }
    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isLoadTypedArrayElementHole())
            return false;
        const MLoadTypedArrayElementHole* other = ins->toLoadTypedArrayElementHole();
        if (arrayType() != other->arrayType())
            return false;
        if (allowDouble() != other->allowDouble())
            return false;
        return congruentIfOperandsEqual(other);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::UnboxedElement);
    }
    bool canProduceFloat32() const override { return arrayType_ == Scalar::Float32; }

    ALLOW_CLONE(MLoadTypedArrayElementHole)
};

// Load a value fallibly or infallibly from a statically known typed array.
class MLoadTypedArrayElementStatic
  : public MUnaryInstruction,
    public ConvertToInt32Policy<0>::Data
{
    MLoadTypedArrayElementStatic(JSObject* someTypedArray, MDefinition* ptr,
                                 int32_t offset, bool needsBoundsCheck)
      : MUnaryInstruction(ptr), someTypedArray_(someTypedArray), offset_(offset),
        needsBoundsCheck_(needsBoundsCheck), fallible_(true)
    {
        int type = accessType();
        if (type == Scalar::Float32)
            setResultType(MIRType_Float32);
        else if (type == Scalar::Float64)
            setResultType(MIRType_Double);
        else
            setResultType(MIRType_Int32);
    }

    CompilerObject someTypedArray_;

    // An offset to be encoded in the load instruction - taking advantage of the
    // addressing modes. This is only non-zero when the access is proven to be
    // within bounds.
    int32_t offset_;
    bool needsBoundsCheck_;
    bool fallible_;

  public:
    INSTRUCTION_HEADER(LoadTypedArrayElementStatic)

    static MLoadTypedArrayElementStatic* New(TempAllocator& alloc, JSObject* someTypedArray,
                                             MDefinition* ptr, int32_t offset = 0,
                                             bool needsBoundsCheck = true)
    {
        return new(alloc) MLoadTypedArrayElementStatic(someTypedArray, ptr, offset,
                                                       needsBoundsCheck);
    }

    Scalar::Type accessType() const {
        return AnyTypedArrayType(someTypedArray_);
    }
    SharedMem<void*> base() const;
    size_t length() const;

    MDefinition* ptr() const { return getOperand(0); }
    int32_t offset() const { return offset_; }
    void setOffset(int32_t offset) { offset_ = offset; }
    bool congruentTo(const MDefinition* ins) const override;
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::UnboxedElement);
    }

    bool needsBoundsCheck() const { return needsBoundsCheck_; }
    void setNeedsBoundsCheck(bool v) { needsBoundsCheck_ = v; }

    bool fallible() const {
        return fallible_;
    }

    void setInfallible() {
        fallible_ = false;
    }

    void computeRange(TempAllocator& alloc) override;
    bool needTruncation(TruncateKind kind) override;
    bool canProduceFloat32() const override { return accessType() == Scalar::Float32; }
    void collectRangeInfoPreTrunc() override;
};

// Base class for MIR ops that write unboxed scalar values.
class StoreUnboxedScalarBase
{
    Scalar::Type writeType_;

  protected:
    explicit StoreUnboxedScalarBase(Scalar::Type writeType)
      : writeType_(writeType)
    {
        MOZ_ASSERT(isIntegerWrite() || isFloatWrite() || isSimdWrite());
    }

  public:
    void setWriteType(Scalar::Type type) {
        writeType_ = type;
    }
    Scalar::Type writeType() const {
        return writeType_;
    }
    bool isByteWrite() const {
        return writeType_ == Scalar::Int8 ||
               writeType_ == Scalar::Uint8 ||
               writeType_ == Scalar::Uint8Clamped;
    }
    bool isIntegerWrite() const {
        return isByteWrite () ||
               writeType_ == Scalar::Int16 ||
               writeType_ == Scalar::Uint16 ||
               writeType_ == Scalar::Int32 ||
               writeType_ == Scalar::Uint32;
    }
    bool isFloatWrite() const {
        return writeType_ == Scalar::Float32 ||
               writeType_ == Scalar::Float64;
    }
    bool isSimdWrite() const {
        return Scalar::isSimdType(writeType());
    }
};

// Store an unboxed scalar value to a typed array or other object.
class MStoreUnboxedScalar
  : public MTernaryInstruction,
    public StoreUnboxedScalarBase,
    public StoreUnboxedScalarPolicy::Data
{
  public:
    enum TruncateInputKind {
        DontTruncateInput,
        TruncateInput
    };

  private:
    Scalar::Type storageType_;

    // Whether this store truncates out of range inputs, for use by range analysis.
    TruncateInputKind truncateInput_;

    bool requiresBarrier_;
    int32_t offsetAdjustment_;
    unsigned numElems_; // used only for SIMD

    MStoreUnboxedScalar(MDefinition* elements, MDefinition* index, MDefinition* value,
                        Scalar::Type storageType, TruncateInputKind truncateInput,
                        MemoryBarrierRequirement requiresBarrier, int32_t offsetAdjustment)
      : MTernaryInstruction(elements, index, value),
        StoreUnboxedScalarBase(storageType),
        storageType_(storageType),
        truncateInput_(truncateInput),
        requiresBarrier_(requiresBarrier == DoesRequireMemoryBarrier),
        offsetAdjustment_(offsetAdjustment),
        numElems_(1)
    {
        if (requiresBarrier_)
            setGuard();         // Not removable or movable
        else
            setMovable();
        MOZ_ASSERT(IsValidElementsType(elements, offsetAdjustment));
        MOZ_ASSERT(index->type() == MIRType_Int32);
        MOZ_ASSERT(storageType >= 0 && storageType < Scalar::MaxTypedArrayViewType);
    }

  public:
    INSTRUCTION_HEADER(StoreUnboxedScalar)

    static MStoreUnboxedScalar* New(TempAllocator& alloc,
                                    MDefinition* elements, MDefinition* index,
                                    MDefinition* value, Scalar::Type storageType,
                                    TruncateInputKind truncateInput,
                                    MemoryBarrierRequirement requiresBarrier =
                                        DoesNotRequireMemoryBarrier,
                                    int32_t offsetAdjustment = 0)
    {
        return new(alloc) MStoreUnboxedScalar(elements, index, value, storageType,
                                              truncateInput, requiresBarrier, offsetAdjustment);
    }

    void setSimdWrite(Scalar::Type writeType, unsigned numElems) {
        MOZ_ASSERT(Scalar::isSimdType(writeType));
        setWriteType(writeType);
        numElems_ = numElems;
    }
    unsigned numElems() const {
        return numElems_;
    }
    Scalar::Type storageType() const {
        return storageType_;
    }
    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    MDefinition* value() const {
        return getOperand(2);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }
    TruncateInputKind truncateInput() const {
        return truncateInput_;
    }
    bool requiresMemoryBarrier() const {
        return requiresBarrier_;
    }
    int32_t offsetAdjustment() const {
        return offsetAdjustment_;
    }
    TruncateKind operandTruncateKind(size_t index) const override;

    bool canConsumeFloat32(MUse* use) const override {
        return use == getUseFor(2) && writeType() == Scalar::Float32;
    }

    ALLOW_CLONE(MStoreUnboxedScalar)
};

class MStoreTypedArrayElementHole
  : public MAryInstruction<4>,
    public StoreUnboxedScalarBase,
    public StoreTypedArrayHolePolicy::Data
{
    MStoreTypedArrayElementHole(MDefinition* elements, MDefinition* length, MDefinition* index,
                                MDefinition* value, Scalar::Type arrayType)
      : MAryInstruction<4>(),
        StoreUnboxedScalarBase(arrayType)
    {
        initOperand(0, elements);
        initOperand(1, length);
        initOperand(2, index);
        initOperand(3, value);
        setMovable();
        MOZ_ASSERT(elements->type() == MIRType_Elements);
        MOZ_ASSERT(length->type() == MIRType_Int32);
        MOZ_ASSERT(index->type() == MIRType_Int32);
        MOZ_ASSERT(arrayType >= 0 && arrayType < Scalar::MaxTypedArrayViewType);
    }

  public:
    INSTRUCTION_HEADER(StoreTypedArrayElementHole)

    static MStoreTypedArrayElementHole* New(TempAllocator& alloc, MDefinition* elements,
                                            MDefinition* length, MDefinition* index,
                                            MDefinition* value, Scalar::Type arrayType)
    {
        return new(alloc) MStoreTypedArrayElementHole(elements, length, index, value, arrayType);
    }

    Scalar::Type arrayType() const {
        MOZ_ASSERT(!Scalar::isSimdType(writeType()),
                   "arrayType == writeType iff the write type isn't SIMD");
        return writeType();
    }
    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* length() const {
        return getOperand(1);
    }
    MDefinition* index() const {
        return getOperand(2);
    }
    MDefinition* value() const {
        return getOperand(3);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }
    TruncateKind operandTruncateKind(size_t index) const override;

    bool canConsumeFloat32(MUse* use) const override {
        return use == getUseFor(3) && arrayType() == Scalar::Float32;
    }

    ALLOW_CLONE(MStoreTypedArrayElementHole)
};

// Store a value infallibly to a statically known typed array.
class MStoreTypedArrayElementStatic :
    public MBinaryInstruction,
    public StoreUnboxedScalarBase,
    public StoreTypedArrayElementStaticPolicy::Data
{
    MStoreTypedArrayElementStatic(JSObject* someTypedArray, MDefinition* ptr, MDefinition* v,
                                  int32_t offset, bool needsBoundsCheck)
        : MBinaryInstruction(ptr, v),
          StoreUnboxedScalarBase(AnyTypedArrayType(someTypedArray)),
          someTypedArray_(someTypedArray),
          offset_(offset), needsBoundsCheck_(needsBoundsCheck)
    {}

    CompilerObject someTypedArray_;

    // An offset to be encoded in the store instruction - taking advantage of the
    // addressing modes. This is only non-zero when the access is proven to be
    // within bounds.
    int32_t offset_;
    bool needsBoundsCheck_;

  public:
    INSTRUCTION_HEADER(StoreTypedArrayElementStatic)

    static MStoreTypedArrayElementStatic* New(TempAllocator& alloc, JSObject* someTypedArray,
                                              MDefinition* ptr, MDefinition* v,
                                              int32_t offset = 0,
                                              bool needsBoundsCheck = true)
    {
        return new(alloc) MStoreTypedArrayElementStatic(someTypedArray, ptr, v,
                                                        offset, needsBoundsCheck);
    }

    Scalar::Type accessType() const {
        return writeType();
    }

    SharedMem<void*> base() const;
    size_t length() const;

    MDefinition* ptr() const { return getOperand(0); }
    MDefinition* value() const { return getOperand(1); }
    bool needsBoundsCheck() const { return needsBoundsCheck_; }
    void setNeedsBoundsCheck(bool v) { needsBoundsCheck_ = v; }
    int32_t offset() const { return offset_; }
    void setOffset(int32_t offset) { offset_ = offset; }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }
    TruncateKind operandTruncateKind(size_t index) const override;

    bool canConsumeFloat32(MUse* use) const override {
        return use == getUseFor(1) && accessType() == Scalar::Float32;
    }
    void collectRangeInfoPreTrunc() override;
};

// Compute an "effective address", i.e., a compound computation of the form:
//   base + index * scale + displacement
class MEffectiveAddress
  : public MBinaryInstruction,
    public NoTypePolicy::Data
{
    MEffectiveAddress(MDefinition* base, MDefinition* index, Scale scale, int32_t displacement)
      : MBinaryInstruction(base, index), scale_(scale), displacement_(displacement)
    {
        MOZ_ASSERT(base->type() == MIRType_Int32);
        MOZ_ASSERT(index->type() == MIRType_Int32);
        setMovable();
        setResultType(MIRType_Int32);
    }

    Scale scale_;
    int32_t displacement_;

  public:
    INSTRUCTION_HEADER(EffectiveAddress)

    static MEffectiveAddress* New(TempAllocator& alloc, MDefinition* base, MDefinition* index,
                                  Scale s, int32_t d)
    {
        return new(alloc) MEffectiveAddress(base, index, s, d);
    }
    MDefinition* base() const {
        return lhs();
    }
    MDefinition* index() const {
        return rhs();
    }
    Scale scale() const {
        return scale_;
    }
    int32_t displacement() const {
        return displacement_;
    }

    ALLOW_CLONE(MEffectiveAddress)
};

// Clamp input to range [0, 255] for Uint8ClampedArray.
class MClampToUint8
  : public MUnaryInstruction,
    public ClampPolicy::Data
{
    explicit MClampToUint8(MDefinition* input)
      : MUnaryInstruction(input)
    {
        setResultType(MIRType_Int32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(ClampToUint8)

    static MClampToUint8* New(TempAllocator& alloc, MDefinition* input) {
        return new(alloc) MClampToUint8(input);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    void computeRange(TempAllocator& alloc) override;

    ALLOW_CLONE(MClampToUint8)
};

class MLoadFixedSlot
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    size_t slot_;

  protected:
    MLoadFixedSlot(MDefinition* obj, size_t slot)
      : MUnaryInstruction(obj), slot_(slot)
    {
        setResultType(MIRType_Value);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(LoadFixedSlot)

    static MLoadFixedSlot* New(TempAllocator& alloc, MDefinition* obj, size_t slot) {
        return new(alloc) MLoadFixedSlot(obj, slot);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    size_t slot() const {
        return slot_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isLoadFixedSlot())
            return false;
        if (slot() != ins->toLoadFixedSlot()->slot())
            return false;
        return congruentIfOperandsEqual(ins);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::FixedSlot);
    }

    bool mightAlias(const MDefinition* store) const override;

    ALLOW_CLONE(MLoadFixedSlot)
};

class MLoadFixedSlotAndUnbox
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    size_t slot_;
    MUnbox::Mode mode_;
    BailoutKind bailoutKind_;
  protected:
    MLoadFixedSlotAndUnbox(MDefinition* obj, size_t slot, MUnbox::Mode mode, MIRType type,
                           BailoutKind kind)
      : MUnaryInstruction(obj), slot_(slot), mode_(mode), bailoutKind_(kind)
    {
        setResultType(type);
        setMovable();
        if (mode_ == MUnbox::TypeBarrier || mode_ == MUnbox::Fallible)
            setGuard();
    }

  public:
    INSTRUCTION_HEADER(LoadFixedSlotAndUnbox)

    static MLoadFixedSlotAndUnbox* New(TempAllocator& alloc, MDefinition* obj, size_t slot,
                                       MUnbox::Mode mode, MIRType type, BailoutKind kind)
    {
        return new(alloc) MLoadFixedSlotAndUnbox(obj, slot, mode, type, kind);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    size_t slot() const {
        return slot_;
    }
    MUnbox::Mode mode() const {
        return mode_;
    }
    BailoutKind bailoutKind() const {
        return bailoutKind_;
    }
    bool fallible() const {
        return mode_ != MUnbox::Infallible;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isLoadFixedSlotAndUnbox() ||
            slot() != ins->toLoadFixedSlotAndUnbox()->slot() ||
            mode() != ins->toLoadFixedSlotAndUnbox()->mode())
        {
            return false;
        }
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::FixedSlot);
    }

    bool mightAlias(const MDefinition* store) const override;

    ALLOW_CLONE(MLoadFixedSlotAndUnbox);
};

class MStoreFixedSlot
  : public MBinaryInstruction,
    public MixPolicy<SingleObjectPolicy, NoFloatPolicy<1> >::Data
{
    bool needsBarrier_;
    size_t slot_;

    MStoreFixedSlot(MDefinition* obj, MDefinition* rval, size_t slot, bool barrier)
      : MBinaryInstruction(obj, rval),
        needsBarrier_(barrier),
        slot_(slot)
    { }

  public:
    INSTRUCTION_HEADER(StoreFixedSlot)

    static MStoreFixedSlot* New(TempAllocator& alloc, MDefinition* obj, size_t slot,
                                MDefinition* rval)
    {
        return new(alloc) MStoreFixedSlot(obj, rval, slot, false);
    }
    static MStoreFixedSlot* NewBarriered(TempAllocator& alloc, MDefinition* obj, size_t slot,
                                         MDefinition* rval)
    {
        return new(alloc) MStoreFixedSlot(obj, rval, slot, true);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* value() const {
        return getOperand(1);
    }
    size_t slot() const {
        return slot_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::FixedSlot);
    }
    bool needsBarrier() const {
        return needsBarrier_;
    }
    void setNeedsBarrier(bool needsBarrier = true) {
        needsBarrier_ = needsBarrier;
    }

    ALLOW_CLONE(MStoreFixedSlot)
};

typedef Vector<JSObject*, 4, JitAllocPolicy> ObjectVector;
typedef Vector<bool, 4, JitAllocPolicy> BoolVector;

class InlinePropertyTable : public TempObject
{
    struct Entry : public TempObject {
        CompilerObjectGroup group;
        CompilerFunction func;

        Entry(ObjectGroup* group, JSFunction* func)
          : group(group), func(func)
        { }
    };

    jsbytecode* pc_;
    MResumePoint* priorResumePoint_;
    Vector<Entry*, 4, JitAllocPolicy> entries_;

  public:
    InlinePropertyTable(TempAllocator& alloc, jsbytecode* pc)
      : pc_(pc), priorResumePoint_(nullptr), entries_(alloc)
    { }

    void setPriorResumePoint(MResumePoint* resumePoint) {
        MOZ_ASSERT(priorResumePoint_ == nullptr);
        priorResumePoint_ = resumePoint;
    }
    MResumePoint* takePriorResumePoint() {
        MResumePoint* rp = priorResumePoint_;
        priorResumePoint_ = nullptr;
        return rp;
    }

    jsbytecode* pc() const {
        return pc_;
    }

    bool addEntry(TempAllocator& alloc, ObjectGroup* group, JSFunction* func) {
        return entries_.append(new(alloc) Entry(group, func));
    }

    size_t numEntries() const {
        return entries_.length();
    }

    ObjectGroup* getObjectGroup(size_t i) const {
        MOZ_ASSERT(i < numEntries());
        return entries_[i]->group;
    }

    JSFunction* getFunction(size_t i) const {
        MOZ_ASSERT(i < numEntries());
        return entries_[i]->func;
    }

    bool hasFunction(JSFunction* func) const;
    bool hasObjectGroup(ObjectGroup* group) const;

    TemporaryTypeSet* buildTypeSetForFunction(JSFunction* func) const;

    // Remove targets that vetoed inlining from the InlinePropertyTable.
    void trimTo(const ObjectVector& targets, const BoolVector& choiceSet);

    // Ensure that the InlinePropertyTable's domain is a subset of |targets|.
    void trimToTargets(const ObjectVector& targets);
};

class CacheLocationList : public InlineConcatList<CacheLocationList>
{
  public:
    CacheLocationList()
      : pc(nullptr),
        script(nullptr)
    { }

    jsbytecode* pc;
    JSScript* script;
};

class MGetPropertyCache
  : public MBinaryInstruction,
    public MixPolicy<ObjectPolicy<0>, CacheIdPolicy<1>>::Data
{
    bool idempotent_ : 1;
    bool monitoredResult_ : 1;

    CacheLocationList location_;

    InlinePropertyTable* inlinePropertyTable_;

    MGetPropertyCache(MDefinition* obj, MDefinition* id, bool monitoredResult)
      : MBinaryInstruction(obj, id),
        idempotent_(false),
        monitoredResult_(monitoredResult),
        location_(),
        inlinePropertyTable_(nullptr)
    {
        setResultType(MIRType_Value);

        // The cache will invalidate if there are objects with e.g. lookup or
        // resolve hooks on the proto chain. setGuard ensures this check is not
        // eliminated.
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(GetPropertyCache)

    static MGetPropertyCache* New(TempAllocator& alloc, MDefinition* obj, MDefinition* id,
                                  bool monitoredResult) {
        return new(alloc) MGetPropertyCache(obj, id, monitoredResult);
    }

    InlinePropertyTable* initInlinePropertyTable(TempAllocator& alloc, jsbytecode* pc) {
        MOZ_ASSERT(inlinePropertyTable_ == nullptr);
        inlinePropertyTable_ = new(alloc) InlinePropertyTable(alloc, pc);
        return inlinePropertyTable_;
    }

    void clearInlinePropertyTable() {
        inlinePropertyTable_ = nullptr;
    }

    InlinePropertyTable* propTable() const {
        return inlinePropertyTable_;
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* idval() const {
        return getOperand(1);
    }

    bool idempotent() const {
        return idempotent_;
    }
    void setIdempotent() {
        idempotent_ = true;
        setMovable();
    }
    bool monitoredResult() const {
        return monitoredResult_;
    }
    CacheLocationList& location() {
        return location_;
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!idempotent_)
            return false;
        if (!ins->isGetPropertyCache())
            return false;
        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        if (idempotent_) {
            return AliasSet::Load(AliasSet::ObjectFields |
                                  AliasSet::FixedSlot |
                                  AliasSet::DynamicSlot);
        }
        return AliasSet::Store(AliasSet::Any);
    }

    void setBlock(MBasicBlock* block) override;
    bool updateForReplacement(MDefinition* ins) override;

    bool allowDoubleResult() const;
};

// Emit code to load a value from an object if it matches one of the receivers
// observed by the baseline IC, else bails out.
class MGetPropertyPolymorphic
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    struct Entry {
        // The group and/or shape to guard against.
        ReceiverGuard receiver;

        // The property to load, null for loads from unboxed properties.
        Shape* shape;
    };

    Vector<Entry, 4, JitAllocPolicy> receivers_;
    CompilerPropertyName name_;

    MGetPropertyPolymorphic(TempAllocator& alloc, MDefinition* obj, PropertyName* name)
      : MUnaryInstruction(obj),
        receivers_(alloc),
        name_(name)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(GetPropertyPolymorphic)

    static MGetPropertyPolymorphic* New(TempAllocator& alloc, MDefinition* obj, PropertyName* name) {
        return new(alloc) MGetPropertyPolymorphic(alloc, obj, name);
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isGetPropertyPolymorphic())
            return false;
        if (name() != ins->toGetPropertyPolymorphic()->name())
            return false;
        return congruentIfOperandsEqual(ins);
    }

    bool addReceiver(const ReceiverGuard& receiver, Shape* shape) {
        Entry entry;
        entry.receiver = receiver;
        entry.shape = shape;
        return receivers_.append(entry);
    }
    size_t numReceivers() const {
        return receivers_.length();
    }
    const ReceiverGuard receiver(size_t i) const {
        return receivers_[i].receiver;
    }
    Shape* shape(size_t i) const {
        return receivers_[i].shape;
    }
    PropertyName* name() const {
        return name_;
    }
    MDefinition* obj() const {
        return getOperand(0);
    }
    AliasSet getAliasSet() const override {
        bool hasUnboxedLoad = false;
        for (size_t i = 0; i < numReceivers(); i++) {
            if (!shape(i)) {
                hasUnboxedLoad = true;
                break;
            }
        }
        return AliasSet::Load(AliasSet::ObjectFields |
                              AliasSet::FixedSlot |
                              AliasSet::DynamicSlot |
                              (hasUnboxedLoad ? AliasSet::UnboxedElement : 0));
    }

    bool mightAlias(const MDefinition* store) const override;
};

// Emit code to store a value to an object's slots if its shape/group matches
// one of the shapes/groups observed by the baseline IC, else bails out.
class MSetPropertyPolymorphic
  : public MBinaryInstruction,
    public MixPolicy<SingleObjectPolicy, NoFloatPolicy<1> >::Data
{
    struct Entry {
        // The group and/or shape to guard against.
        ReceiverGuard receiver;

        // The property to store, null for stores to unboxed properties.
        Shape* shape;
    };

    Vector<Entry, 4, JitAllocPolicy> receivers_;
    CompilerPropertyName name_;
    bool needsBarrier_;

    MSetPropertyPolymorphic(TempAllocator& alloc, MDefinition* obj, MDefinition* value,
                            PropertyName* name)
      : MBinaryInstruction(obj, value),
        receivers_(alloc),
        name_(name),
        needsBarrier_(false)
    {
    }

  public:
    INSTRUCTION_HEADER(SetPropertyPolymorphic)

    static MSetPropertyPolymorphic* New(TempAllocator& alloc, MDefinition* obj, MDefinition* value,
                                        PropertyName* name) {
        return new(alloc) MSetPropertyPolymorphic(alloc, obj, value, name);
    }

    bool addReceiver(const ReceiverGuard& receiver, Shape* shape) {
        Entry entry;
        entry.receiver = receiver;
        entry.shape = shape;
        return receivers_.append(entry);
    }
    size_t numReceivers() const {
        return receivers_.length();
    }
    const ReceiverGuard& receiver(size_t i) const {
        return receivers_[i].receiver;
    }
    Shape* shape(size_t i) const {
        return receivers_[i].shape;
    }
    PropertyName* name() const {
        return name_;
    }
    MDefinition* obj() const {
        return getOperand(0);
    }
    MDefinition* value() const {
        return getOperand(1);
    }
    bool needsBarrier() const {
        return needsBarrier_;
    }
    void setNeedsBarrier() {
        needsBarrier_ = true;
    }
    AliasSet getAliasSet() const override {
        bool hasUnboxedStore = false;
        for (size_t i = 0; i < numReceivers(); i++) {
            if (!shape(i)) {
                hasUnboxedStore = true;
                break;
            }
        }
        return AliasSet::Store(AliasSet::ObjectFields |
                               AliasSet::FixedSlot |
                               AliasSet::DynamicSlot |
                               (hasUnboxedStore ? AliasSet::UnboxedElement : 0));
    }
};

class MDispatchInstruction
  : public MControlInstruction,
    public SingleObjectPolicy::Data
{
    // Map from JSFunction* -> MBasicBlock.
    struct Entry {
        JSFunction* func;
        // If |func| has a singleton group, |funcGroup| is null. Otherwise,
        // |funcGroup| holds the ObjectGroup for |func|, and dispatch guards
        // on the group instead of directly on the function.
        ObjectGroup* funcGroup;
        MBasicBlock* block;

        Entry(JSFunction* func, ObjectGroup* funcGroup, MBasicBlock* block)
          : func(func), funcGroup(funcGroup), block(block)
        { }
    };
    Vector<Entry, 4, JitAllocPolicy> map_;

    // An optional fallback path that uses MCall.
    MBasicBlock* fallback_;
    MUse operand_;

    void initOperand(size_t index, MDefinition* operand) {
        MOZ_ASSERT(index == 0);
        operand_.init(operand, this);
    }

  public:
    MDispatchInstruction(TempAllocator& alloc, MDefinition* input)
      : map_(alloc), fallback_(nullptr)
    {
        initOperand(0, input);
    }

  protected:
    MUse* getUseFor(size_t index) final override {
        MOZ_ASSERT(index == 0);
        return &operand_;
    }
    const MUse* getUseFor(size_t index) const final override {
        MOZ_ASSERT(index == 0);
        return &operand_;
    }
    MDefinition* getOperand(size_t index) const final override {
        MOZ_ASSERT(index == 0);
        return operand_.producer();
    }
    size_t numOperands() const final override {
        return 1;
    }
    size_t indexOf(const MUse* u) const final override {
        MOZ_ASSERT(u == getUseFor(0));
        return 0;
    }
    void replaceOperand(size_t index, MDefinition* operand) final override {
        MOZ_ASSERT(index == 0);
        operand_.replaceProducer(operand);
    }

  public:
    void setSuccessor(size_t i, MBasicBlock* successor) {
        MOZ_ASSERT(i < numSuccessors());
        if (i == map_.length())
            fallback_ = successor;
        else
            map_[i].block = successor;
    }
    size_t numSuccessors() const final override {
        return map_.length() + (fallback_ ? 1 : 0);
    }
    void replaceSuccessor(size_t i, MBasicBlock* successor) final override {
        setSuccessor(i, successor);
    }
    MBasicBlock* getSuccessor(size_t i) const final override {
        MOZ_ASSERT(i < numSuccessors());
        if (i == map_.length())
            return fallback_;
        return map_[i].block;
    }

  public:
    bool addCase(JSFunction* func, ObjectGroup* funcGroup, MBasicBlock* block) {
        return map_.append(Entry(func, funcGroup, block));
    }
    uint32_t numCases() const {
        return map_.length();
    }
    JSFunction* getCase(uint32_t i) const {
        return map_[i].func;
    }
    ObjectGroup* getCaseObjectGroup(uint32_t i) const {
        return map_[i].funcGroup;
    }
    MBasicBlock* getCaseBlock(uint32_t i) const {
        return map_[i].block;
    }

    bool hasFallback() const {
        return bool(fallback_);
    }
    void addFallback(MBasicBlock* block) {
        MOZ_ASSERT(!hasFallback());
        fallback_ = block;
    }
    MBasicBlock* getFallback() const {
        MOZ_ASSERT(hasFallback());
        return fallback_;
    }

  public:
    MDefinition* input() const {
        return getOperand(0);
    }
};

// Polymorphic dispatch for inlining, keyed off incoming ObjectGroup.
class MObjectGroupDispatch : public MDispatchInstruction
{
    // Map ObjectGroup (of CallProp's Target Object) -> JSFunction (yielded by the CallProp).
    InlinePropertyTable* inlinePropertyTable_;

    MObjectGroupDispatch(TempAllocator& alloc, MDefinition* input, InlinePropertyTable* table)
      : MDispatchInstruction(alloc, input),
        inlinePropertyTable_(table)
    { }

  public:
    INSTRUCTION_HEADER(ObjectGroupDispatch)

    static MObjectGroupDispatch* New(TempAllocator& alloc, MDefinition* ins,
                                     InlinePropertyTable* table)
    {
        return new(alloc) MObjectGroupDispatch(alloc, ins, table);
    }

    InlinePropertyTable* propTable() const {
        return inlinePropertyTable_;
    }
};

// Polymorphic dispatch for inlining, keyed off incoming JSFunction*.
class MFunctionDispatch : public MDispatchInstruction
{
    MFunctionDispatch(TempAllocator& alloc, MDefinition* input)
      : MDispatchInstruction(alloc, input)
    { }

  public:
    INSTRUCTION_HEADER(FunctionDispatch)

    static MFunctionDispatch* New(TempAllocator& alloc, MDefinition* ins) {
        return new(alloc) MFunctionDispatch(alloc, ins);
    }
};

class MBindNameCache
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    CompilerPropertyName name_;
    CompilerScript script_;
    jsbytecode* pc_;

    MBindNameCache(MDefinition* scopeChain, PropertyName* name, JSScript* script, jsbytecode* pc)
      : MUnaryInstruction(scopeChain), name_(name), script_(script), pc_(pc)
    {
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(BindNameCache)

    static MBindNameCache* New(TempAllocator& alloc, MDefinition* scopeChain, PropertyName* name,
                               JSScript* script, jsbytecode* pc)
    {
        return new(alloc) MBindNameCache(scopeChain, name, script, pc);
    }

    MDefinition* scopeChain() const {
        return getOperand(0);
    }
    PropertyName* name() const {
        return name_;
    }
    JSScript* script() const {
        return script_;
    }
    jsbytecode* pc() const {
        return pc_;
    }
};

// Guard on an object's shape.
class MGuardShape
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    CompilerShape shape_;
    BailoutKind bailoutKind_;

    MGuardShape(MDefinition* obj, Shape* shape, BailoutKind bailoutKind)
      : MUnaryInstruction(obj),
        shape_(shape),
        bailoutKind_(bailoutKind)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Object);
        setResultTypeSet(obj->resultTypeSet());

        // Disallow guarding on unboxed object shapes. The group is better to
        // guard on, and guarding on the shape can interact badly with
        // MConvertUnboxedObjectToNative.
        MOZ_ASSERT(shape->getObjectClass() != &UnboxedPlainObject::class_);
    }

  public:
    INSTRUCTION_HEADER(GuardShape)

    static MGuardShape* New(TempAllocator& alloc, MDefinition* obj, Shape* shape,
                            BailoutKind bailoutKind)
    {
        return new(alloc) MGuardShape(obj, shape, bailoutKind);
    }

    MDefinition* obj() const {
        return getOperand(0);
    }
    const Shape* shape() const {
        return shape_;
    }
    BailoutKind bailoutKind() const {
        return bailoutKind_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isGuardShape())
            return false;
        if (shape() != ins->toGuardShape()->shape())
            return false;
        if (bailoutKind() != ins->toGuardShape()->bailoutKind())
            return false;
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
};

// Bail if the object's shape or unboxed group is not in the input list.
class MGuardReceiverPolymorphic
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    Vector<ReceiverGuard, 4, JitAllocPolicy> receivers_;

    MGuardReceiverPolymorphic(TempAllocator& alloc, MDefinition* obj)
      : MUnaryInstruction(obj),
        receivers_(alloc)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Object);
        setResultTypeSet(obj->resultTypeSet());
    }

  public:
    INSTRUCTION_HEADER(GuardReceiverPolymorphic)

    static MGuardReceiverPolymorphic* New(TempAllocator& alloc, MDefinition* obj) {
        return new(alloc) MGuardReceiverPolymorphic(alloc, obj);
    }

    MDefinition* obj() const {
        return getOperand(0);
    }

    bool addReceiver(const ReceiverGuard& receiver) {
        return receivers_.append(receiver);
    }
    size_t numReceivers() const {
        return receivers_.length();
    }
    const ReceiverGuard& receiver(size_t i) const {
        return receivers_[i];
    }

    bool congruentTo(const MDefinition* ins) const override;

    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
};

// Guard on an object's group, inclusively or exclusively.
class MGuardObjectGroup
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    CompilerObjectGroup group_;
    bool bailOnEquality_;
    BailoutKind bailoutKind_;

    MGuardObjectGroup(MDefinition* obj, ObjectGroup* group, bool bailOnEquality,
                      BailoutKind bailoutKind)
      : MUnaryInstruction(obj),
        group_(group),
        bailOnEquality_(bailOnEquality),
        bailoutKind_(bailoutKind)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Object);

        // Unboxed groups which might be converted to natives can't be guarded
        // on, due to MConvertUnboxedObjectToNative.
        MOZ_ASSERT_IF(group->maybeUnboxedLayoutDontCheckGeneration(),
                      !group->unboxedLayoutDontCheckGeneration().nativeGroup());
    }

  public:
    INSTRUCTION_HEADER(GuardObjectGroup)

    static MGuardObjectGroup* New(TempAllocator& alloc, MDefinition* obj, ObjectGroup* group,
                                  bool bailOnEquality, BailoutKind bailoutKind) {
        return new(alloc) MGuardObjectGroup(obj, group, bailOnEquality, bailoutKind);
    }

    MDefinition* obj() const {
        return getOperand(0);
    }
    const ObjectGroup* group() const {
        return group_;
    }
    bool bailOnEquality() const {
        return bailOnEquality_;
    }
    BailoutKind bailoutKind() const {
        return bailoutKind_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isGuardObjectGroup())
            return false;
        if (group() != ins->toGuardObjectGroup()->group())
            return false;
        if (bailOnEquality() != ins->toGuardObjectGroup()->bailOnEquality())
            return false;
        if (bailoutKind() != ins->toGuardObjectGroup()->bailoutKind())
            return false;
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
};

// Guard on an object's identity, inclusively or exclusively.
class MGuardObjectIdentity
  : public MBinaryInstruction,
    public SingleObjectPolicy::Data
{
    bool bailOnEquality_;

    MGuardObjectIdentity(MDefinition* obj, MDefinition* expected, bool bailOnEquality)
      : MBinaryInstruction(obj, expected),
        bailOnEquality_(bailOnEquality)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(GuardObjectIdentity)

    static MGuardObjectIdentity* New(TempAllocator& alloc, MDefinition* obj, MDefinition* expected,
                                     bool bailOnEquality) {
        return new(alloc) MGuardObjectIdentity(obj, expected, bailOnEquality);
    }

    MDefinition* obj() const {
        return getOperand(0);
    }
    MDefinition* expected() const {
        return getOperand(1);
    }
    bool bailOnEquality() const {
        return bailOnEquality_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isGuardObjectIdentity())
            return false;
        if (bailOnEquality() != ins->toGuardObjectIdentity()->bailOnEquality())
            return false;
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
};

// Guard on an object's class.
class MGuardClass
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    const Class* class_;

    MGuardClass(MDefinition* obj, const Class* clasp)
      : MUnaryInstruction(obj),
        class_(clasp)
    {
        setGuard();
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(GuardClass)

    static MGuardClass* New(TempAllocator& alloc, MDefinition* obj, const Class* clasp) {
        return new(alloc) MGuardClass(obj, clasp);
    }

    MDefinition* obj() const {
        return getOperand(0);
    }
    const Class* getClass() const {
        return class_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isGuardClass())
            return false;
        if (getClass() != ins->toGuardClass()->getClass())
            return false;
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }

    ALLOW_CLONE(MGuardClass)
};

// Guard on the presence or absence of an unboxed object's expando.
class MGuardUnboxedExpando
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    bool requireExpando_;
    BailoutKind bailoutKind_;

    MGuardUnboxedExpando(MDefinition* obj, bool requireExpando, BailoutKind bailoutKind)
      : MUnaryInstruction(obj),
        requireExpando_(requireExpando),
        bailoutKind_(bailoutKind)
    {
        setGuard();
        setMovable();
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(GuardUnboxedExpando)

    static MGuardUnboxedExpando* New(TempAllocator& alloc, MDefinition* obj,
                                     bool requireExpando, BailoutKind bailoutKind) {
        return new(alloc) MGuardUnboxedExpando(obj, requireExpando, bailoutKind);
    }

    MDefinition* obj() const {
        return getOperand(0);
    }
    bool requireExpando() const {
        return requireExpando_;
    }
    BailoutKind bailoutKind() const {
        return bailoutKind_;
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!congruentIfOperandsEqual(ins))
            return false;
        if (requireExpando() != ins->toGuardUnboxedExpando()->requireExpando())
            return false;
        return true;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
};

// Load an unboxed plain object's expando.
class MLoadUnboxedExpando
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
  private:
    explicit MLoadUnboxedExpando(MDefinition* object)
      : MUnaryInstruction(object)
    {
        setResultType(MIRType_Object);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(LoadUnboxedExpando)

    static MLoadUnboxedExpando* New(TempAllocator& alloc, MDefinition* object) {
        return new(alloc) MLoadUnboxedExpando(object);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::ObjectFields);
    }
};

// Load from vp[slot] (slots that are not inline in an object).
class MLoadSlot
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    uint32_t slot_;

    MLoadSlot(MDefinition* slots, uint32_t slot)
      : MUnaryInstruction(slots),
        slot_(slot)
    {
        setResultType(MIRType_Value);
        setMovable();
        MOZ_ASSERT(slots->type() == MIRType_Slots);
    }

  public:
    INSTRUCTION_HEADER(LoadSlot)

    static MLoadSlot* New(TempAllocator& alloc, MDefinition* slots, uint32_t slot) {
        return new(alloc) MLoadSlot(slots, slot);
    }

    MDefinition* slots() const {
        return getOperand(0);
    }
    uint32_t slot() const {
        return slot_;
    }

    HashNumber valueHash() const override;
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isLoadSlot())
            return false;
        if (slot() != ins->toLoadSlot()->slot())
            return false;
        return congruentIfOperandsEqual(ins);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    AliasSet getAliasSet() const override {
        MOZ_ASSERT(slots()->type() == MIRType_Slots);
        return AliasSet::Load(AliasSet::DynamicSlot);
    }
    bool mightAlias(const MDefinition* store) const override;

    ALLOW_CLONE(MLoadSlot)
};

// Inline call to access a function's environment (scope chain).
class MFunctionEnvironment
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
  public:
    explicit MFunctionEnvironment(MDefinition* function)
        : MUnaryInstruction(function)
    {
        setResultType(MIRType_Object);
        setMovable();
    }

    INSTRUCTION_HEADER(FunctionEnvironment)

    static MFunctionEnvironment* New(TempAllocator& alloc, MDefinition* function) {
        return new(alloc) MFunctionEnvironment(function);
    }

    MDefinition* function() const {
        return getOperand(0);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    // A function's environment is fixed.
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// Store to vp[slot] (slots that are not inline in an object).
class MStoreSlot
  : public MBinaryInstruction,
    public MixPolicy<ObjectPolicy<0>, NoFloatPolicy<1> >::Data
{
    uint32_t slot_;
    MIRType slotType_;
    bool needsBarrier_;

    MStoreSlot(MDefinition* slots, uint32_t slot, MDefinition* value, bool barrier)
        : MBinaryInstruction(slots, value),
          slot_(slot),
          slotType_(MIRType_Value),
          needsBarrier_(barrier)
    {
        MOZ_ASSERT(slots->type() == MIRType_Slots);
    }

  public:
    INSTRUCTION_HEADER(StoreSlot)

    static MStoreSlot* New(TempAllocator& alloc, MDefinition* slots, uint32_t slot,
                           MDefinition* value)
    {
        return new(alloc) MStoreSlot(slots, slot, value, false);
    }
    static MStoreSlot* NewBarriered(TempAllocator& alloc, MDefinition* slots, uint32_t slot,
                                    MDefinition* value)
    {
        return new(alloc) MStoreSlot(slots, slot, value, true);
    }

    MDefinition* slots() const {
        return getOperand(0);
    }
    MDefinition* value() const {
        return getOperand(1);
    }
    uint32_t slot() const {
        return slot_;
    }
    MIRType slotType() const {
        return slotType_;
    }
    void setSlotType(MIRType slotType) {
        MOZ_ASSERT(slotType != MIRType_None);
        slotType_ = slotType;
    }
    bool needsBarrier() const {
        return needsBarrier_;
    }
    void setNeedsBarrier() {
        needsBarrier_ = true;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::DynamicSlot);
    }

    ALLOW_CLONE(MStoreSlot)
};

class MGetNameCache
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
  public:
    enum AccessKind {
        NAMETYPEOF,
        NAME
    };

  private:
    CompilerPropertyName name_;
    AccessKind kind_;

    MGetNameCache(MDefinition* obj, PropertyName* name, AccessKind kind)
      : MUnaryInstruction(obj),
        name_(name),
        kind_(kind)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(GetNameCache)

    static MGetNameCache* New(TempAllocator& alloc, MDefinition* obj, PropertyName* name,
                              AccessKind kind)
    {
        return new(alloc) MGetNameCache(obj, name, kind);
    }
    MDefinition* scopeObj() const {
        return getOperand(0);
    }
    PropertyName* name() const {
        return name_;
    }
    AccessKind accessKind() const {
        return kind_;
    }
};

class MCallGetIntrinsicValue : public MNullaryInstruction
{
    CompilerPropertyName name_;

    explicit MCallGetIntrinsicValue(PropertyName* name)
      : name_(name)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(CallGetIntrinsicValue)

    static MCallGetIntrinsicValue* New(TempAllocator& alloc, PropertyName* name) {
        return new(alloc) MCallGetIntrinsicValue(name);
    }
    PropertyName* name() const {
        return name_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MSetPropertyInstruction : public MBinaryInstruction
{
    CompilerPropertyName name_;
    bool strict_;

  protected:
    MSetPropertyInstruction(MDefinition* obj, MDefinition* value, PropertyName* name,
                            bool strict)
      : MBinaryInstruction(obj, value),
        name_(name), strict_(strict)
    {}

  public:
    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* value() const {
        return getOperand(1);
    }
    PropertyName* name() const {
        return name_;
    }
    bool strict() const {
        return strict_;
    }
};

class MSetElementInstruction
  : public MTernaryInstruction
{
    bool strict_;
  protected:
    MSetElementInstruction(MDefinition* object, MDefinition* index, MDefinition* value, bool strict)
        : MTernaryInstruction(object, index, value),
          strict_(strict)
    {
    }

  public:
    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    MDefinition* value() const {
        return getOperand(2);
    }
    bool strict() const {
        return strict_;
    }
};

class MDeleteProperty
  : public MUnaryInstruction,
    public BoxInputsPolicy::Data
{
    CompilerPropertyName name_;
    bool strict_;

  protected:
    MDeleteProperty(MDefinition* val, PropertyName* name, bool strict)
      : MUnaryInstruction(val),
        name_(name),
        strict_(strict)
    {
        setResultType(MIRType_Boolean);
    }

  public:
    INSTRUCTION_HEADER(DeleteProperty)

    static MDeleteProperty* New(TempAllocator& alloc, MDefinition* obj, PropertyName* name,
                                bool strict)
    {
        return new(alloc) MDeleteProperty(obj, name, strict);
    }
    MDefinition* value() const {
        return getOperand(0);
    }
    PropertyName* name() const {
        return name_;
    }
    bool strict() const {
        return strict_;
    }
};

class MDeleteElement
  : public MBinaryInstruction,
    public BoxInputsPolicy::Data
{
    bool strict_;

    MDeleteElement(MDefinition* value, MDefinition* index, bool strict)
      : MBinaryInstruction(value, index),
        strict_(strict)
    {
        setResultType(MIRType_Boolean);
    }

  public:
    INSTRUCTION_HEADER(DeleteElement)

    static MDeleteElement* New(TempAllocator& alloc, MDefinition* value, MDefinition* index,
                               bool strict)
    {
        return new(alloc) MDeleteElement(value, index, strict);
    }
    MDefinition* value() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    bool strict() const {
        return strict_;
    }
};

// Note: This uses CallSetElementPolicy to always box its second input,
// ensuring we don't need two LIR instructions to lower this.
class MCallSetProperty
  : public MSetPropertyInstruction,
    public CallSetElementPolicy::Data
{
    MCallSetProperty(MDefinition* obj, MDefinition* value, PropertyName* name, bool strict)
      : MSetPropertyInstruction(obj, value, name, strict)
    {
    }

  public:
    INSTRUCTION_HEADER(CallSetProperty)

    static MCallSetProperty* New(TempAllocator& alloc, MDefinition* obj, MDefinition* value,
                                 PropertyName* name, bool strict)
    {
        return new(alloc) MCallSetProperty(obj, value, name, strict);
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MSetPropertyCache
  : public MTernaryInstruction,
    public Mix3Policy<SingleObjectPolicy, CacheIdPolicy<1>, NoFloatPolicy<2>>::Data
{
    bool strict_ : 1;
    bool needsTypeBarrier_ : 1;
    bool guardHoles_ : 1;

    MSetPropertyCache(MDefinition* obj, MDefinition* id, MDefinition* value, bool strict,
                      bool typeBarrier, bool guardHoles)
      : MTernaryInstruction(obj, id, value),
        strict_(strict),
        needsTypeBarrier_(typeBarrier),
        guardHoles_(guardHoles)
    {
    }

  public:
    INSTRUCTION_HEADER(SetPropertyCache)

    static MSetPropertyCache* New(TempAllocator& alloc, MDefinition* obj, MDefinition* id,
                                  MDefinition* value, bool strict, bool typeBarrier,
                                  bool guardHoles)
    {
        return new(alloc) MSetPropertyCache(obj, id, value, strict, typeBarrier, guardHoles);
    }

    bool needsTypeBarrier() const {
        return needsTypeBarrier_;
    }

    bool guardHoles() const {
        return guardHoles_;
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    MDefinition* idval() const {
        return getOperand(1);
    }
    MDefinition* value() const {
        return getOperand(2);
    }
    bool strict() const {
        return strict_;
    }
};

class MCallGetProperty
  : public MUnaryInstruction,
    public BoxInputsPolicy::Data
{
    CompilerPropertyName name_;
    bool idempotent_;

    MCallGetProperty(MDefinition* value, PropertyName* name)
      : MUnaryInstruction(value), name_(name),
        idempotent_(false)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(CallGetProperty)

    static MCallGetProperty* New(TempAllocator& alloc, MDefinition* value, PropertyName* name)
    {
        return new(alloc) MCallGetProperty(value, name);
    }
    MDefinition* value() const {
        return getOperand(0);
    }
    PropertyName* name() const {
        return name_;
    }

    // Constructors need to perform a GetProp on the function prototype.
    // Since getters cannot be set on the prototype, fetching is non-effectful.
    // The operation may be safely repeated in case of bailout.
    void setIdempotent() {
        idempotent_ = true;
    }
    AliasSet getAliasSet() const override {
        if (!idempotent_)
            return AliasSet::Store(AliasSet::Any);
        return AliasSet::None();
    }
    bool possiblyCalls() const override {
        return true;
    }
};

// Inline call to handle lhs[rhs]. The first input is a Value so that this
// instruction can handle both objects and strings.
class MCallGetElement
  : public MBinaryInstruction,
    public BoxInputsPolicy::Data
{
    MCallGetElement(MDefinition* lhs, MDefinition* rhs)
      : MBinaryInstruction(lhs, rhs)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(CallGetElement)

    static MCallGetElement* New(TempAllocator& alloc, MDefinition* lhs, MDefinition* rhs) {
        return new(alloc) MCallGetElement(lhs, rhs);
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MCallSetElement
  : public MSetElementInstruction,
    public CallSetElementPolicy::Data
{
    MCallSetElement(MDefinition* object, MDefinition* index, MDefinition* value, bool strict)
      : MSetElementInstruction(object, index, value, strict)
    {
    }

  public:
    INSTRUCTION_HEADER(CallSetElement)

    static MCallSetElement* New(TempAllocator& alloc, MDefinition* object, MDefinition* index,
                                MDefinition* value, bool strict)
    {
        return new(alloc) MCallSetElement(object, index, value, strict);
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MCallInitElementArray
  : public MAryInstruction<2>,
    public MixPolicy<ObjectPolicy<0>, BoxPolicy<1> >::Data
{
    uint32_t index_;

    MCallInitElementArray(MDefinition* obj, uint32_t index, MDefinition* val)
      : index_(index)
    {
        initOperand(0, obj);
        initOperand(1, val);
    }

  public:
    INSTRUCTION_HEADER(CallInitElementArray)

    static MCallInitElementArray* New(TempAllocator& alloc, MDefinition* obj, uint32_t index,
                                      MDefinition* val)
    {
        return new(alloc) MCallInitElementArray(obj, index, val);
    }

    MDefinition* object() const {
        return getOperand(0);
    }

    uint32_t index() const {
        return index_;
    }

    MDefinition* value() const {
        return getOperand(1);
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MSetDOMProperty
  : public MAryInstruction<2>,
    public MixPolicy<ObjectPolicy<0>, BoxPolicy<1> >::Data
{
    const JSJitSetterOp func_;

    MSetDOMProperty(const JSJitSetterOp func, MDefinition* obj, MDefinition* val)
      : func_(func)
    {
        initOperand(0, obj);
        initOperand(1, val);
    }

  public:
    INSTRUCTION_HEADER(SetDOMProperty)

    static MSetDOMProperty* New(TempAllocator& alloc, JSJitSetterOp func, MDefinition* obj,
                                MDefinition* val)
    {
        return new(alloc) MSetDOMProperty(func, obj, val);
    }

    JSJitSetterOp fun() const {
        return func_;
    }

    MDefinition* object() {
        return getOperand(0);
    }

    MDefinition* value()
    {
        return getOperand(1);
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MGetDOMProperty
  : public MVariadicInstruction,
    public ObjectPolicy<0>::Data
{
    const JSJitInfo* info_;

  protected:
    explicit MGetDOMProperty(const JSJitInfo* jitinfo)
      : info_(jitinfo)
    {
        MOZ_ASSERT(jitinfo);
        MOZ_ASSERT(jitinfo->type() == JSJitInfo::Getter);

        // We are movable iff the jitinfo says we can be.
        if (isDomMovable()) {
            MOZ_ASSERT(jitinfo->aliasSet() != JSJitInfo::AliasEverything);
            setMovable();
        } else {
            // If we're not movable, that means we shouldn't be DCEd either,
            // because we might throw an exception when called, and getting rid
            // of that is observable.
            setGuard();
        }

        setResultType(MIRType_Value);
    }

    const JSJitInfo* info() const {
        return info_;
    }

    bool init(TempAllocator& alloc, MDefinition* obj, MDefinition* guard,
              MDefinition* globalGuard) {
        MOZ_ASSERT(obj);
        // guard can be null.
        // globalGuard can be null.
        size_t operandCount = 1;
        if (guard)
            ++operandCount;
        if (globalGuard)
            ++operandCount;
        if (!MVariadicInstruction::init(alloc, operandCount))
            return false;
        initOperand(0, obj);

        size_t operandIndex = 1;
        // Pin the guard, if we have one as an operand if we want to hoist later.
        if (guard)
            initOperand(operandIndex++, guard);

        // And the same for the global guard, if we have one.
        if (globalGuard)
            initOperand(operandIndex, globalGuard);

        return true;
    }

  public:
    INSTRUCTION_HEADER(GetDOMProperty)

    static MGetDOMProperty* New(TempAllocator& alloc, const JSJitInfo* info, MDefinition* obj,
                                MDefinition* guard, MDefinition* globalGuard)
    {
        MGetDOMProperty* res = new(alloc) MGetDOMProperty(info);
        if (!res || !res->init(alloc, obj, guard, globalGuard))
            return nullptr;
        return res;
    }

    JSJitGetterOp fun() const {
        return info_->getter;
    }
    bool isInfallible() const {
        return info_->isInfallible;
    }
    bool isDomMovable() const {
        return info_->isMovable;
    }
    JSJitInfo::AliasSet domAliasSet() const {
        return info_->aliasSet();
    }
    size_t domMemberSlotIndex() const {
        MOZ_ASSERT(info_->isAlwaysInSlot || info_->isLazilyCachedInSlot);
        return info_->slotIndex;
    }
    bool valueMayBeInSlot() const {
        return info_->isLazilyCachedInSlot;
    }
    MDefinition* object() {
        return getOperand(0);
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isGetDOMProperty())
            return false;

        return congruentTo(ins->toGetDOMProperty());
    }

    bool congruentTo(const MGetDOMProperty* ins) const {
        if (!isDomMovable())
            return false;

        // Checking the jitinfo is the same as checking the constant function
        if (!(info() == ins->info()))
            return false;

        return congruentIfOperandsEqual(ins);
    }

    AliasSet getAliasSet() const override {
        JSJitInfo::AliasSet aliasSet = domAliasSet();
        if (aliasSet == JSJitInfo::AliasNone)
            return AliasSet::None();
        if (aliasSet == JSJitInfo::AliasDOMSets)
            return AliasSet::Load(AliasSet::DOMProperty);
        MOZ_ASSERT(aliasSet == JSJitInfo::AliasEverything);
        return AliasSet::Store(AliasSet::Any);
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MGetDOMMember : public MGetDOMProperty
{
    // We inherit everything from MGetDOMProperty except our
    // possiblyCalls value and the congruentTo behavior.
    explicit MGetDOMMember(const JSJitInfo* jitinfo)
        : MGetDOMProperty(jitinfo)
    {
        setResultType(MIRTypeFromValueType(jitinfo->returnType()));
    }

  public:
    INSTRUCTION_HEADER(GetDOMMember)

    static MGetDOMMember* New(TempAllocator& alloc, const JSJitInfo* info, MDefinition* obj,
                              MDefinition* guard, MDefinition* globalGuard)
    {
        MGetDOMMember* res = new(alloc) MGetDOMMember(info);
        if (!res || !res->init(alloc, obj, guard, globalGuard))
            return nullptr;
        return res;
    }

    bool possiblyCalls() const override {
        return false;
    }

    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isGetDOMMember())
            return false;

        return MGetDOMProperty::congruentTo(ins->toGetDOMMember());
    }
};

class MStringLength
  : public MUnaryInstruction,
    public StringPolicy<0>::Data
{
    explicit MStringLength(MDefinition* string)
      : MUnaryInstruction(string)
    {
        setResultType(MIRType_Int32);
        setMovable();
    }
  public:
    INSTRUCTION_HEADER(StringLength)

    static MStringLength* New(TempAllocator& alloc, MDefinition* string) {
        return new(alloc) MStringLength(string);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    MDefinition* string() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        // The string |length| property is immutable, so there is no
        // implicit dependency.
        return AliasSet::None();
    }

    void computeRange(TempAllocator& alloc) override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MStringLength)
};

// Inlined version of Math.floor().
class MFloor
  : public MUnaryInstruction,
    public FloatingPointPolicy<0>::Data
{
    explicit MFloor(MDefinition* num)
      : MUnaryInstruction(num)
    {
        setResultType(MIRType_Int32);
        specialization_ = MIRType_Double;
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Floor)

    static MFloor* New(TempAllocator& alloc, MDefinition* num) {
        return new(alloc) MFloor(num);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool isFloat32Commutative() const override {
        return true;
    }
    void trySpecializeFloat32(TempAllocator& alloc) override;
#ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override {
        return true;
    }
#endif
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    void computeRange(TempAllocator& alloc) override;
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MFloor)
};

// Inlined version of Math.ceil().
class MCeil
  : public MUnaryInstruction,
    public FloatingPointPolicy<0>::Data
{
    explicit MCeil(MDefinition* num)
      : MUnaryInstruction(num)
    {
        setResultType(MIRType_Int32);
        specialization_ = MIRType_Double;
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Ceil)

    static MCeil* New(TempAllocator& alloc, MDefinition* num) {
        return new(alloc) MCeil(num);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool isFloat32Commutative() const override {
        return true;
    }
    void trySpecializeFloat32(TempAllocator& alloc) override;
#ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override {
        return true;
    }
#endif
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    void computeRange(TempAllocator& alloc) override;
    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MCeil)
};

// Inlined version of Math.round().
class MRound
  : public MUnaryInstruction,
    public FloatingPointPolicy<0>::Data
{
    explicit MRound(MDefinition* num)
      : MUnaryInstruction(num)
    {
        setResultType(MIRType_Int32);
        specialization_ = MIRType_Double;
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(Round)

    static MRound* New(TempAllocator& alloc, MDefinition* num) {
        return new(alloc) MRound(num);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool isFloat32Commutative() const override {
        return true;
    }
    void trySpecializeFloat32(TempAllocator& alloc) override;
#ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override {
        return true;
    }
#endif
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MRound)
};

class MIteratorStart
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    uint8_t flags_;

    MIteratorStart(MDefinition* obj, uint8_t flags)
      : MUnaryInstruction(obj), flags_(flags)
    {
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(IteratorStart)

    static MIteratorStart* New(TempAllocator& alloc, MDefinition* obj, uint8_t flags) {
        return new(alloc) MIteratorStart(obj, flags);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    uint8_t flags() const {
        return flags_;
    }
};

class MIteratorMore
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MIteratorMore(MDefinition* iter)
      : MUnaryInstruction(iter)
    {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(IteratorMore)

    static MIteratorMore* New(TempAllocator& alloc, MDefinition* iter) {
        return new(alloc) MIteratorMore(iter);
    }

    MDefinition* iterator() const {
        return getOperand(0);
    }
};

class MIsNoIter
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    explicit MIsNoIter(MDefinition* def)
      : MUnaryInstruction(def)
    {
        setResultType(MIRType_Boolean);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(IsNoIter)

    static MIsNoIter* New(TempAllocator& alloc, MDefinition* def) {
        return new(alloc) MIsNoIter(def);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MIteratorEnd
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MIteratorEnd(MDefinition* iter)
      : MUnaryInstruction(iter)
    { }

  public:
    INSTRUCTION_HEADER(IteratorEnd)

    static MIteratorEnd* New(TempAllocator& alloc, MDefinition* iter) {
        return new(alloc) MIteratorEnd(iter);
    }

    MDefinition* iterator() const {
        return getOperand(0);
    }
};

// Implementation for 'in' operator.
class MIn
  : public MBinaryInstruction,
    public MixPolicy<BoxPolicy<0>, ObjectPolicy<1> >::Data
{
    MIn(MDefinition* key, MDefinition* obj)
      : MBinaryInstruction(key, obj)
    {
        setResultType(MIRType_Boolean);
    }

  public:
    INSTRUCTION_HEADER(In)

    static MIn* New(TempAllocator& alloc, MDefinition* key, MDefinition* obj) {
        return new(alloc) MIn(key, obj);
    }

    bool possiblyCalls() const override {
        return true;
    }
};


// Test whether the index is in the array bounds or a hole.
class MInArray
  : public MQuaternaryInstruction,
    public ObjectPolicy<3>::Data
{
    bool needsHoleCheck_;
    bool needsNegativeIntCheck_;
    JSValueType unboxedType_;

    MInArray(MDefinition* elements, MDefinition* index,
             MDefinition* initLength, MDefinition* object,
             bool needsHoleCheck, JSValueType unboxedType)
      : MQuaternaryInstruction(elements, index, initLength, object),
        needsHoleCheck_(needsHoleCheck),
        needsNegativeIntCheck_(true),
        unboxedType_(unboxedType)
    {
        setResultType(MIRType_Boolean);
        setMovable();
        MOZ_ASSERT(elements->type() == MIRType_Elements);
        MOZ_ASSERT(index->type() == MIRType_Int32);
        MOZ_ASSERT(initLength->type() == MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(InArray)

    static MInArray* New(TempAllocator& alloc, MDefinition* elements, MDefinition* index,
                         MDefinition* initLength, MDefinition* object,
                         bool needsHoleCheck, JSValueType unboxedType)
    {
        return new(alloc) MInArray(elements, index, initLength, object, needsHoleCheck,
                                   unboxedType);
    }

    MDefinition* elements() const {
        return getOperand(0);
    }
    MDefinition* index() const {
        return getOperand(1);
    }
    MDefinition* initLength() const {
        return getOperand(2);
    }
    MDefinition* object() const {
        return getOperand(3);
    }
    bool needsHoleCheck() const {
        return needsHoleCheck_;
    }
    bool needsNegativeIntCheck() const {
        return needsNegativeIntCheck_;
    }
    JSValueType unboxedType() const {
        return unboxedType_;
    }
    void collectRangeInfoPreTrunc() override;
    AliasSet getAliasSet() const override {
        return AliasSet::Load(AliasSet::Element);
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isInArray())
            return false;
        const MInArray* other = ins->toInArray();
        if (needsHoleCheck() != other->needsHoleCheck())
            return false;
        if (needsNegativeIntCheck() != other->needsNegativeIntCheck())
            return false;
        if (unboxedType() != other->unboxedType())
            return false;
        return congruentIfOperandsEqual(other);
    }
};

// Implementation for instanceof operator with specific rhs.
class MInstanceOf
  : public MUnaryInstruction,
    public InstanceOfPolicy::Data
{
    CompilerObject protoObj_;

    MInstanceOf(MDefinition* obj, JSObject* proto)
      : MUnaryInstruction(obj),
        protoObj_(proto)
    {
        setResultType(MIRType_Boolean);
    }

  public:
    INSTRUCTION_HEADER(InstanceOf)

    static MInstanceOf* New(TempAllocator& alloc, MDefinition* obj, JSObject* proto) {
        return new(alloc) MInstanceOf(obj, proto);
    }

    JSObject* prototypeObject() {
        return protoObj_;
    }
};

// Implementation for instanceof operator with unknown rhs.
class MCallInstanceOf
  : public MBinaryInstruction,
    public MixPolicy<BoxPolicy<0>, ObjectPolicy<1> >::Data
{
    MCallInstanceOf(MDefinition* obj, MDefinition* proto)
      : MBinaryInstruction(obj, proto)
    {
        setResultType(MIRType_Boolean);
    }

  public:
    INSTRUCTION_HEADER(CallInstanceOf)

    static MCallInstanceOf* New(TempAllocator& alloc, MDefinition* obj, MDefinition* proto) {
        return new(alloc) MCallInstanceOf(obj, proto);
    }

};

class MArgumentsLength : public MNullaryInstruction
{
    MArgumentsLength()
    {
        setResultType(MIRType_Int32);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(ArgumentsLength)

    static MArgumentsLength* New(TempAllocator& alloc) {
        return new(alloc) MArgumentsLength();
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        // Arguments |length| cannot be mutated by Ion Code.
        return AliasSet::None();
   }

    void computeRange(TempAllocator& alloc) override;

    bool writeRecoverData(CompactBufferWriter& writer) const override;

    bool canRecoverOnBailout() const override {
        return true;
    }
};

// This MIR instruction is used to get an argument from the actual arguments.
class MGetFrameArgument
  : public MUnaryInstruction,
    public IntPolicy<0>::Data
{
    bool scriptHasSetArg_;

    MGetFrameArgument(MDefinition* idx, bool scriptHasSetArg)
      : MUnaryInstruction(idx),
        scriptHasSetArg_(scriptHasSetArg)
    {
        setResultType(MIRType_Value);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(GetFrameArgument)

    static MGetFrameArgument* New(TempAllocator& alloc, MDefinition* idx, bool scriptHasSetArg) {
        return new(alloc) MGetFrameArgument(idx, scriptHasSetArg);
    }

    MDefinition* index() const {
        return getOperand(0);
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        // If the script doesn't have any JSOP_SETARG ops, then this instruction is never
        // aliased.
        if (scriptHasSetArg_)
            return AliasSet::Load(AliasSet::FrameArgument);
        return AliasSet::None();
    }
};

class MNewTarget : public MNullaryInstruction
{
    MNewTarget() : MNullaryInstruction() {
        setResultType(MIRType_Value);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(NewTarget)

    static MNewTarget* New(TempAllocator& alloc) {
        return new(alloc) MNewTarget();
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// This MIR instruction is used to set an argument value in the frame.
class MSetFrameArgument
  : public MUnaryInstruction,
    public NoFloatPolicy<0>::Data
{
    uint32_t argno_;

    MSetFrameArgument(uint32_t argno, MDefinition* value)
      : MUnaryInstruction(value),
        argno_(argno)
    {
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(SetFrameArgument)

    static MSetFrameArgument* New(TempAllocator& alloc, uint32_t argno, MDefinition* value) {
        return new(alloc) MSetFrameArgument(argno, value);
    }

    uint32_t argno() const {
        return argno_;
    }

    MDefinition* value() const {
        return getOperand(0);
    }

    bool congruentTo(const MDefinition* ins) const override {
        return false;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::FrameArgument);
    }
};

class MRestCommon
{
    unsigned numFormals_;
    CompilerGCPointer<ArrayObject*> templateObject_;

  protected:
    MRestCommon(unsigned numFormals, ArrayObject* templateObject)
      : numFormals_(numFormals),
        templateObject_(templateObject)
   { }

  public:
    unsigned numFormals() const {
        return numFormals_;
    }
    ArrayObject* templateObject() const {
        return templateObject_;
    }
};

class MRest
  : public MUnaryInstruction,
    public MRestCommon,
    public IntPolicy<0>::Data
{
    MRest(CompilerConstraintList* constraints, MDefinition* numActuals, unsigned numFormals,
          ArrayObject* templateObject)
      : MUnaryInstruction(numActuals),
        MRestCommon(numFormals, templateObject)
    {
        setResultType(MIRType_Object);
        setResultTypeSet(MakeSingletonTypeSet(constraints, templateObject));
    }

  public:
    INSTRUCTION_HEADER(Rest)

    static MRest* New(TempAllocator& alloc, CompilerConstraintList* constraints,
                      MDefinition* numActuals, unsigned numFormals,
                      ArrayObject* templateObject)
    {
        return new(alloc) MRest(constraints, numActuals, numFormals, templateObject);
    }

    MDefinition* numActuals() const {
        return getOperand(0);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool possiblyCalls() const override {
        return true;
    }
};

class MFilterTypeSet
  : public MUnaryInstruction,
    public FilterTypeSetPolicy::Data
{
    MFilterTypeSet(MDefinition* def, TemporaryTypeSet* types)
      : MUnaryInstruction(def)
    {
        MOZ_ASSERT(!types->unknown());
        setResultType(types->getKnownMIRType());
        setResultTypeSet(types);
    }

  public:
    INSTRUCTION_HEADER(FilterTypeSet)

    static MFilterTypeSet* New(TempAllocator& alloc, MDefinition* def, TemporaryTypeSet* types) {
        return new(alloc) MFilterTypeSet(def, types);
    }

    bool congruentTo(const MDefinition* def) const override {
        return false;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    virtual bool neverHoist() const override {
        return resultTypeSet()->empty();
    }
    void computeRange(TempAllocator& alloc) override;

    bool isFloat32Commutative() const override { return true; }
    bool canProduceFloat32() const override;
    bool canConsumeFloat32(MUse* operand) const override;
    void trySpecializeFloat32(TempAllocator& alloc) override;
};

// Given a value, guard that the value is in a particular TypeSet, then returns
// that value.
class MTypeBarrier
  : public MUnaryInstruction,
    public TypeBarrierPolicy::Data
{
    BarrierKind barrierKind_;

    MTypeBarrier(MDefinition* def, TemporaryTypeSet* types, BarrierKind kind)
      : MUnaryInstruction(def),
        barrierKind_(kind)
    {
        MOZ_ASSERT(kind == BarrierKind::TypeTagOnly || kind == BarrierKind::TypeSet);

        MOZ_ASSERT(!types->unknown());
        setResultType(types->getKnownMIRType());
        setResultTypeSet(types);

        setGuard();
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(TypeBarrier)

    static MTypeBarrier* New(TempAllocator& alloc, MDefinition* def, TemporaryTypeSet* types,
                             BarrierKind kind = BarrierKind::TypeSet) {
        return new(alloc) MTypeBarrier(def, types, kind);
    }

    void printOpcode(GenericPrinter& out) const override;
    bool congruentTo(const MDefinition* def) const override;

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    virtual bool neverHoist() const override {
        return resultTypeSet()->empty();
    }
    BarrierKind barrierKind() const {
        return barrierKind_;
    }

    bool alwaysBails() const {
        // If mirtype of input doesn't agree with mirtype of barrier,
        // we will definitely bail.
        MIRType type = resultTypeSet()->getKnownMIRType();
        if (type == MIRType_Value)
            return false;
        if (input()->type() == MIRType_Value)
            return false;
        if (input()->type() == MIRType_ObjectOrNull) {
            // The ObjectOrNull optimization is only performed when the
            // barrier's type is MIRType_Null.
            MOZ_ASSERT(type == MIRType_Null);
            return false;
        }
        return input()->type() != type;
    }

    ALLOW_CLONE(MTypeBarrier)
};

// Like MTypeBarrier, guard that the value is in the given type set. This is
// used before property writes to ensure the value being written is represented
// in the property types for the object.
class MMonitorTypes
  : public MUnaryInstruction,
    public BoxInputsPolicy::Data
{
    const TemporaryTypeSet* typeSet_;
    BarrierKind barrierKind_;

    MMonitorTypes(MDefinition* def, const TemporaryTypeSet* types, BarrierKind kind)
      : MUnaryInstruction(def),
        typeSet_(types),
        barrierKind_(kind)
    {
        MOZ_ASSERT(kind == BarrierKind::TypeTagOnly || kind == BarrierKind::TypeSet);

        setGuard();
        MOZ_ASSERT(!types->unknown());
    }

  public:
    INSTRUCTION_HEADER(MonitorTypes)

    static MMonitorTypes* New(TempAllocator& alloc, MDefinition* def, const TemporaryTypeSet* types,
                              BarrierKind kind) {
        return new(alloc) MMonitorTypes(def, types, kind);
    }

    const TemporaryTypeSet* typeSet() const {
        return typeSet_;
    }
    BarrierKind barrierKind() const {
        return barrierKind_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// Given a value being written to another object, update the generational store
// buffer if the value is in the nursery and object is in the tenured heap.
class MPostWriteBarrier : public MBinaryInstruction, public ObjectPolicy<0>::Data
{
    MPostWriteBarrier(MDefinition* obj, MDefinition* value)
      : MBinaryInstruction(obj, value)
    {
        setGuard();
    }

  public:
    INSTRUCTION_HEADER(PostWriteBarrier)

    static MPostWriteBarrier* New(TempAllocator& alloc, MDefinition* obj, MDefinition* value) {
        return new(alloc) MPostWriteBarrier(obj, value);
    }

    MDefinition* object() const {
        return getOperand(0);
    }

    MDefinition* value() const {
        return getOperand(1);
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

#ifdef DEBUG
    bool isConsistentFloat32Use(MUse* use) const override {
        // During lowering, values that neither have object nor value MIR type
        // are ignored, thus Float32 can show up at this point without any issue.
        return use == getUseFor(1);
    }
#endif

    ALLOW_CLONE(MPostWriteBarrier)
};

class MNewDeclEnvObject : public MNullaryInstruction
{
    CompilerGCPointer<DeclEnvObject*> templateObj_;

    explicit MNewDeclEnvObject(DeclEnvObject* templateObj)
      : MNullaryInstruction(),
        templateObj_(templateObj)
    {
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(NewDeclEnvObject)

    static MNewDeclEnvObject* New(TempAllocator& alloc, DeclEnvObject* templateObj) {
        return new(alloc) MNewDeclEnvObject(templateObj);
    }

    DeclEnvObject* templateObj() {
        return templateObj_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MNewCallObjectBase : public MNullaryInstruction
{
    CompilerGCPointer<CallObject*> templateObj_;

  protected:
    explicit MNewCallObjectBase(CallObject* templateObj)
      : MNullaryInstruction(),
        templateObj_(templateObj)
    {
        setResultType(MIRType_Object);
    }

  public:
    CallObject* templateObject() {
        return templateObj_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MNewCallObject : public MNewCallObjectBase
{
  public:
    INSTRUCTION_HEADER(NewCallObject)

    explicit MNewCallObject(CallObject* templateObj)
      : MNewCallObjectBase(templateObj)
    {}

    static MNewCallObject*
    New(TempAllocator& alloc, CallObject* templateObj)
    {
        return new(alloc) MNewCallObject(templateObj);
    }
};

class MNewRunOnceCallObject : public MNewCallObjectBase
{
  public:
    INSTRUCTION_HEADER(NewRunOnceCallObject)

    explicit MNewRunOnceCallObject(CallObject* templateObj)
      : MNewCallObjectBase(templateObj)
    {}

    static MNewRunOnceCallObject*
    New(TempAllocator& alloc, CallObject* templateObj)
    {
        return new(alloc) MNewRunOnceCallObject(templateObj);
    }
};

class MNewStringObject :
  public MUnaryInstruction,
  public ConvertToStringPolicy<0>::Data
{
    CompilerObject templateObj_;

    MNewStringObject(MDefinition* input, JSObject* templateObj)
      : MUnaryInstruction(input),
        templateObj_(templateObj)
    {
        setResultType(MIRType_Object);
    }

  public:
    INSTRUCTION_HEADER(NewStringObject)

    static MNewStringObject* New(TempAllocator& alloc, MDefinition* input, JSObject* templateObj) {
        return new(alloc) MNewStringObject(input, templateObj);
    }

    StringObject* templateObj() const;
};

// This is an alias for MLoadFixedSlot.
class MEnclosingScope : public MLoadFixedSlot
{
    explicit MEnclosingScope(MDefinition* obj)
      : MLoadFixedSlot(obj, ScopeObject::enclosingScopeSlot())
    {
        setResultType(MIRType_Object);
    }

  public:
    static MEnclosingScope* New(TempAllocator& alloc, MDefinition* obj) {
        return new(alloc) MEnclosingScope(obj);
    }

    AliasSet getAliasSet() const override {
        // ScopeObject reserved slots are immutable.
        return AliasSet::None();
    }
};

// This is an element of a spaghetti stack which is used to represent the memory
// context which has to be restored in case of a bailout.
struct MStoreToRecover : public TempObject, public InlineSpaghettiStackNode<MStoreToRecover>
{
    MDefinition* operand;

    explicit MStoreToRecover(MDefinition* operand)
      : operand(operand)
    { }
};

typedef InlineSpaghettiStack<MStoreToRecover> MStoresToRecoverList;

// A resume point contains the information needed to reconstruct the Baseline
// state from a position in the JIT. See the big comment near resumeAfter() in
// IonBuilder.cpp.
class MResumePoint final :
  public MNode
#ifdef DEBUG
  , public InlineForwardListNode<MResumePoint>
#endif
{
  public:
    enum Mode {
        ResumeAt,    // Resume until before the current instruction
        ResumeAfter, // Resume after the current instruction
        Outer        // State before inlining.
    };

  private:
    friend class MBasicBlock;
    friend void AssertBasicGraphCoherency(MIRGraph& graph);

    // List of stack slots needed to reconstruct the frame corresponding to the
    // function which is compiled by IonBuilder.
    FixedList<MUse> operands_;

    // List of stores needed to reconstruct the content of objects which are
    // emulated by EmulateStateOf variants.
    MStoresToRecoverList stores_;

    jsbytecode* pc_;
    MInstruction* instruction_;
    Mode mode_;

    MResumePoint(MBasicBlock* block, jsbytecode* pc, Mode mode);
    void inherit(MBasicBlock* state);

  protected:
    // Initializes operands_ to an empty array of a fixed length.
    // The array may then be filled in by inherit().
    bool init(TempAllocator& alloc);

    void clearOperand(size_t index) {
        // FixedList doesn't initialize its elements, so do an unchecked init.
        operands_[index].initUncheckedWithoutProducer(this);
    }

    MUse* getUseFor(size_t index) override {
        return &operands_[index];
    }
    const MUse* getUseFor(size_t index) const override {
        return &operands_[index];
    }

  public:
    static MResumePoint* New(TempAllocator& alloc, MBasicBlock* block, jsbytecode* pc,
                             Mode mode);
    static MResumePoint* New(TempAllocator& alloc, MBasicBlock* block, MResumePoint* model,
                             const MDefinitionVector& operands);
    static MResumePoint* Copy(TempAllocator& alloc, MResumePoint* src);

    MNode::Kind kind() const override {
        return MNode::ResumePoint;
    }
    size_t numAllocatedOperands() const {
        return operands_.length();
    }
    uint32_t stackDepth() const {
        return numAllocatedOperands();
    }
    size_t numOperands() const override {
        return numAllocatedOperands();
    }
    size_t indexOf(const MUse* u) const final override {
        MOZ_ASSERT(u >= &operands_[0]);
        MOZ_ASSERT(u <= &operands_[numOperands() - 1]);
        return u - &operands_[0];
    }
    void initOperand(size_t index, MDefinition* operand) {
        // FixedList doesn't initialize its elements, so do an unchecked init.
        operands_[index].initUnchecked(operand, this);
    }
    void replaceOperand(size_t index, MDefinition* operand) final override {
        operands_[index].replaceProducer(operand);
    }

    bool isObservableOperand(MUse* u) const;
    bool isObservableOperand(size_t index) const;
    bool isRecoverableOperand(MUse* u) const;

    MDefinition* getOperand(size_t index) const override {
        return operands_[index].producer();
    }
    jsbytecode* pc() const {
        return pc_;
    }
    MResumePoint* caller() const;
    uint32_t frameCount() const {
        uint32_t count = 1;
        for (MResumePoint* it = caller(); it; it = it->caller())
            count++;
        return count;
    }
    MInstruction* instruction() {
        return instruction_;
    }
    void setInstruction(MInstruction* ins) {
        MOZ_ASSERT(!instruction_);
        instruction_ = ins;
    }
    // Only to be used by stealResumePoint.
    void replaceInstruction(MInstruction* ins) {
        MOZ_ASSERT(instruction_);
        instruction_ = ins;
    }
    void resetInstruction() {
        MOZ_ASSERT(instruction_);
        instruction_ = nullptr;
    }
    Mode mode() const {
        return mode_;
    }

    void releaseUses() {
        for (size_t i = 0, e = numOperands(); i < e; i++) {
            if (operands_[i].hasProducer())
                operands_[i].releaseProducer();
        }
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;

    // Register a store instruction on the current resume point. This
    // instruction would be recovered when we are bailing out. The |cache|
    // argument can be any resume point, it is used to share memory if we are
    // doing the same modification.
    void addStore(TempAllocator& alloc, MDefinition* store, const MResumePoint* cache = nullptr);

    MStoresToRecoverList::iterator storesBegin() const {
        return stores_.begin();
    }
    MStoresToRecoverList::iterator storesEnd() const {
        return stores_.end();
    }

    virtual void dump(GenericPrinter& out) const override;
    virtual void dump() const override;
};

class MIsCallable
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MIsCallable(MDefinition* object)
      : MUnaryInstruction(object)
    {
        setResultType(MIRType_Boolean);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(IsCallable)

    static MIsCallable* New(TempAllocator& alloc, MDefinition* obj) {
        return new(alloc) MIsCallable(obj);
    }
    MDefinition* object() const {
        return getOperand(0);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MIsObject
  : public MUnaryInstruction,
    public BoxInputsPolicy::Data
{
    explicit MIsObject(MDefinition* object)
    : MUnaryInstruction(object)
    {
        setResultType(MIRType_Boolean);
        setMovable();
    }
  public:
    INSTRUCTION_HEADER(IsObject)
    static MIsObject* New(TempAllocator& alloc, MDefinition* obj) {
        return new(alloc) MIsObject(obj);
    }
    MDefinition* object() const {
        return getOperand(0);
    }
    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MHasClass
    : public MUnaryInstruction,
      public SingleObjectPolicy::Data
{
    const Class* class_;

    MHasClass(MDefinition* object, const Class* clasp)
      : MUnaryInstruction(object)
      , class_(clasp)
    {
        MOZ_ASSERT(object->type() == MIRType_Object);
        setResultType(MIRType_Boolean);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(HasClass)

    static MHasClass* New(TempAllocator& alloc, MDefinition* obj, const Class* clasp) {
        return new(alloc) MHasClass(obj, clasp);
    }

    MDefinition* object() const {
        return getOperand(0);
    }
    const Class* getClass() const {
        return class_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
    bool congruentTo(const MDefinition* ins) const override {
        if (!ins->isHasClass())
            return false;
        if (getClass() != ins->toHasClass()->getClass())
            return false;
        return congruentIfOperandsEqual(ins);
    }
};

class MCheckReturn
  : public MBinaryInstruction,
    public BoxInputsPolicy::Data
{
    explicit MCheckReturn(MDefinition* retVal, MDefinition* thisVal)
      : MBinaryInstruction(retVal, thisVal)
    {
        setGuard();
        setResultType(MIRType_Value);
        setResultTypeSet(retVal->resultTypeSet());
    }

  public:
    INSTRUCTION_HEADER(CheckReturn)

    static MCheckReturn* New(TempAllocator& alloc, MDefinition* retVal, MDefinition* thisVal) {
        return new (alloc) MCheckReturn(retVal, thisVal);
    }

    MDefinition* returnValue() const {
        return getOperand(0);
    }
    MDefinition* thisValue() const {
        return getOperand(1);
    }
};

// Increase the warm-up counter of the provided script upon execution and test if
// the warm-up counter surpasses the threshold. Upon hit it will recompile the
// outermost script (i.e. not the inlined script).
class MRecompileCheck : public MNullaryInstruction
{
  public:
    enum RecompileCheckType {
        RecompileCheck_OptimizationLevel,
        RecompileCheck_Inlining
    };

  private:
    JSScript* script_;
    uint32_t recompileThreshold_;
    bool forceRecompilation_;
    bool increaseWarmUpCounter_;

    MRecompileCheck(JSScript* script, uint32_t recompileThreshold, RecompileCheckType type)
      : script_(script),
        recompileThreshold_(recompileThreshold)
    {
        switch (type) {
          case RecompileCheck_OptimizationLevel:
            forceRecompilation_ = false;
            increaseWarmUpCounter_ = true;
            break;
          case RecompileCheck_Inlining:
            forceRecompilation_ = true;
            increaseWarmUpCounter_ = false;
            break;
          default:
            MOZ_CRASH("Unexpected recompile check type");
        }

        setGuard();
    }

  public:
    INSTRUCTION_HEADER(RecompileCheck)

    static MRecompileCheck* New(TempAllocator& alloc, JSScript* script_, uint32_t recompileThreshold,
                                RecompileCheckType type)
    {
        return new(alloc) MRecompileCheck(script_, recompileThreshold, type);
    }

    JSScript* script() const {
        return script_;
    }

    uint32_t recompileThreshold() const {
        return recompileThreshold_;
    }

    bool forceRecompilation() const {
        return forceRecompilation_;
    }

    bool increaseWarmUpCounter() const {
        return increaseWarmUpCounter_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

// All barriered operations - MMemoryBarrier, MCompareExchangeTypedArrayElement,
// MExchangeTypedArrayElement, and MAtomicTypedArrayElementBinop, as well as
// MLoadUnboxedScalar and MStoreUnboxedScalar when they are marked as requiring
// a memory barrer - have the following attributes:
//
// - Not movable
// - Not removable
// - Not congruent with any other instruction
// - Effectful (they alias every TypedArray store)
//
// The intended effect of those constraints is to prevent all loads
// and stores preceding the barriered operation from being moved to
// after the barriered operation, and vice versa, and to prevent the
// barriered operation from being removed or hoisted.

class MMemoryBarrier
  : public MNullaryInstruction
{
    // The type is a combination of the memory barrier types in AtomicOp.h.
    const MemoryBarrierBits type_;

    explicit MMemoryBarrier(MemoryBarrierBits type)
      : type_(type)
    {
        MOZ_ASSERT((type_ & ~MembarAllbits) == MembarNobits);
        setGuard();             // Not removable
    }

  public:
    INSTRUCTION_HEADER(MemoryBarrier)

    static MMemoryBarrier* New(TempAllocator& alloc, MemoryBarrierBits type = MembarFull) {
        return new(alloc) MMemoryBarrier(type);
    }
    MemoryBarrierBits type() const {
        return type_;
    }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }
};

class MAtomicIsLockFree
  : public MUnaryInstruction,
    public ConvertToInt32Policy<0>::Data
{
    explicit MAtomicIsLockFree(MDefinition* value)
      : MUnaryInstruction(value)
    {
        setResultType(MIRType_Boolean);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(AtomicIsLockFree)

    static MAtomicIsLockFree* New(TempAllocator& alloc, MDefinition* value) {
        return new(alloc) MAtomicIsLockFree(value);
    }

    MDefinition* foldsTo(TempAllocator& alloc) override;

    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }

    bool congruentTo(const MDefinition* ins) const override {
        return congruentIfOperandsEqual(ins);
    }

    bool writeRecoverData(CompactBufferWriter& writer) const override;
    bool canRecoverOnBailout() const override {
        return true;
    }

    ALLOW_CLONE(MAtomicIsLockFree)
};

// This applies to an object that is known to be a TypedArray, it bails out
// if the obj does not map a SharedArrayBuffer.

class MGuardSharedTypedArray
  : public MUnaryInstruction,
    public SingleObjectPolicy::Data
{
    explicit MGuardSharedTypedArray(MDefinition* obj)
      : MUnaryInstruction(obj)
    {
        setGuard();
        setMovable();
    }

public:
    INSTRUCTION_HEADER(GuardSharedTypedArray)

    static MGuardSharedTypedArray* New(TempAllocator& alloc, MDefinition* obj) {
        return new(alloc) MGuardSharedTypedArray(obj);
    }
    MDefinition* obj() const {
        return getOperand(0);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::None();
    }
};

class MCompareExchangeTypedArrayElement
  : public MAryInstruction<4>,
    public Mix4Policy<ObjectPolicy<0>, IntPolicy<1>, TruncateToInt32Policy<2>, TruncateToInt32Policy<3>>::Data
{
    Scalar::Type arrayType_;

    explicit MCompareExchangeTypedArrayElement(MDefinition* elements, MDefinition* index,
                                               Scalar::Type arrayType, MDefinition* oldval,
                                               MDefinition* newval)
      : arrayType_(arrayType)
    {
        initOperand(0, elements);
        initOperand(1, index);
        initOperand(2, oldval);
        initOperand(3, newval);
        setGuard();             // Not removable
    }

  public:
    INSTRUCTION_HEADER(CompareExchangeTypedArrayElement)

    static MCompareExchangeTypedArrayElement* New(TempAllocator& alloc, MDefinition* elements,
                                                  MDefinition* index, Scalar::Type arrayType,
                                                  MDefinition* oldval, MDefinition* newval)
    {
        return new(alloc) MCompareExchangeTypedArrayElement(elements, index, arrayType, oldval, newval);
    }
    bool isByteArray() const {
        return (arrayType_ == Scalar::Int8 ||
                arrayType_ == Scalar::Uint8);
    }
    MDefinition* elements() {
        return getOperand(0);
    }
    MDefinition* index() {
        return getOperand(1);
    }
    MDefinition* oldval() {
        return getOperand(2);
    }
    int oldvalOperand() {
        return 2;
    }
    MDefinition* newval() {
        return getOperand(3);
    }
    Scalar::Type arrayType() const {
        return arrayType_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }
};

class MAtomicExchangeTypedArrayElement
  : public MAryInstruction<3>,
    public Mix3Policy<ObjectPolicy<0>, IntPolicy<1>, TruncateToInt32Policy<2>>::Data
{
    Scalar::Type arrayType_;

    MAtomicExchangeTypedArrayElement(MDefinition* elements, MDefinition* index, MDefinition* value,
                                     Scalar::Type arrayType)
      : arrayType_(arrayType)
    {
        MOZ_ASSERT(arrayType <= Scalar::Uint32);
        initOperand(0, elements);
        initOperand(1, index);
        initOperand(2, value);
        setGuard();             // Not removable
    }

  public:
    INSTRUCTION_HEADER(AtomicExchangeTypedArrayElement)

    static MAtomicExchangeTypedArrayElement* New(TempAllocator& alloc, MDefinition* elements,
                                                 MDefinition* index, MDefinition* value,
                                                 Scalar::Type arrayType)
    {
        return new(alloc) MAtomicExchangeTypedArrayElement(elements, index, value, arrayType);
    }

    bool isByteArray() const {
        return (arrayType_ == Scalar::Int8 ||
                arrayType_ == Scalar::Uint8);
    }
    MDefinition* elements() {
        return getOperand(0);
    }
    MDefinition* index() {
        return getOperand(1);
    }
    MDefinition* value() {
        return getOperand(2);
    }
    Scalar::Type arrayType() const {
        return arrayType_;
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }
};

class MAtomicTypedArrayElementBinop
    : public MAryInstruction<3>,
      public Mix3Policy< ObjectPolicy<0>, IntPolicy<1>, TruncateToInt32Policy<2> >::Data
{
  private:
    AtomicOp op_;
    Scalar::Type arrayType_;

  protected:
    explicit MAtomicTypedArrayElementBinop(AtomicOp op, MDefinition* elements, MDefinition* index,
                                           Scalar::Type arrayType, MDefinition* value)
      : op_(op),
        arrayType_(arrayType)
    {
        initOperand(0, elements);
        initOperand(1, index);
        initOperand(2, value);
        setGuard();             // Not removable
    }

  public:
    INSTRUCTION_HEADER(AtomicTypedArrayElementBinop)

    static MAtomicTypedArrayElementBinop* New(TempAllocator& alloc, AtomicOp op,
                                              MDefinition* elements, MDefinition* index,
                                              Scalar::Type arrayType, MDefinition* value)
    {
        return new(alloc) MAtomicTypedArrayElementBinop(op, elements, index, arrayType, value);
    }

    bool isByteArray() const {
        return (arrayType_ == Scalar::Int8 ||
                arrayType_ == Scalar::Uint8);
    }
    AtomicOp operation() const {
        return op_;
    }
    Scalar::Type arrayType() const {
        return arrayType_;
    }
    MDefinition* elements() {
        return getOperand(0);
    }
    MDefinition* index() {
        return getOperand(1);
    }
    MDefinition* value() {
        return getOperand(2);
    }
    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::UnboxedElement);
    }
};

class MDebugger : public MNullaryInstruction
{
  public:
    INSTRUCTION_HEADER(Debugger)

    static MDebugger* New(TempAllocator& alloc) {
        return new(alloc) MDebugger();
    }
};

class MCheckObjCoercible
  : public MUnaryInstruction,
    public BoxInputsPolicy::Data
{
    explicit MCheckObjCoercible(MDefinition* toCheck)
      : MUnaryInstruction(toCheck)
    {
        setGuard();
        setResultType(MIRType_Value);
        setResultTypeSet(toCheck->resultTypeSet());
    }

  public:
    INSTRUCTION_HEADER(CheckObjCoercible)
    static MCheckObjCoercible* New(TempAllocator& alloc, MDefinition* toCheck) {
        return new(alloc) MCheckObjCoercible(toCheck);
    }

    MDefinition* checkValue() {
        return getOperand(0);
    }
};

class MAsmJSNeg
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    MAsmJSNeg(MDefinition* op, MIRType type)
      : MUnaryInstruction(op)
    {
        setResultType(type);
        setMovable();
    }

  public:
    INSTRUCTION_HEADER(AsmJSNeg)
    static MAsmJSNeg* NewAsmJS(TempAllocator& alloc, MDefinition* op, MIRType type) {
        return new(alloc) MAsmJSNeg(op, type);
    }
};

class MAsmJSHeapAccess
{
    int32_t offset_;
    Scalar::Type accessType_ : 8;
    bool needsBoundsCheck_;
    unsigned numSimdElems_;
    MemoryBarrierBits barrierBefore_;
    MemoryBarrierBits barrierAfter_;

  public:
    MAsmJSHeapAccess(Scalar::Type accessType, bool needsBoundsCheck, unsigned numSimdElems = 0,
                     MemoryBarrierBits barrierBefore = MembarNobits,
                     MemoryBarrierBits barrierAfter = MembarNobits)
      : offset_(0),
        accessType_(accessType),
        needsBoundsCheck_(needsBoundsCheck),
        numSimdElems_(numSimdElems),
        barrierBefore_(barrierBefore),
        barrierAfter_(barrierAfter)
    {
        MOZ_ASSERT(numSimdElems <= ScalarTypeToLength(accessType));
    }

    int32_t offset() const { return offset_; }
    int32_t endOffset() const { return offset() + byteSize(); }
    Scalar::Type accessType() const { return accessType_; }
    unsigned byteSize() const {
        return Scalar::isSimdType(accessType())
               ? Scalar::scalarByteSize(accessType()) * numSimdElems()
               : TypedArrayElemSize(accessType());
    }
    bool needsBoundsCheck() const { return needsBoundsCheck_; }
    void removeBoundsCheck() { needsBoundsCheck_ = false; }
    unsigned numSimdElems() const { MOZ_ASSERT(Scalar::isSimdType(accessType_)); return numSimdElems_; }
    void setOffset(int32_t o) {
        MOZ_ASSERT(o >= 0);
        offset_ = o;
    }
    MemoryBarrierBits barrierBefore() const { return barrierBefore_; }
    MemoryBarrierBits barrierAfter() const { return barrierAfter_; }
    bool isAtomicAccess() const { return (barrierBefore_|barrierAfter_) != MembarNobits; }
};

class MAsmJSLoadHeap
  : public MUnaryInstruction,
    public MAsmJSHeapAccess,
    public NoTypePolicy::Data
{
    MAsmJSLoadHeap(Scalar::Type accessType, MDefinition* ptr, bool needsBoundsCheck,
                   unsigned numSimdElems, MemoryBarrierBits before, MemoryBarrierBits after)
      : MUnaryInstruction(ptr),
        MAsmJSHeapAccess(accessType, needsBoundsCheck, numSimdElems, before, after)
    {
        if (before|after)
            setGuard();         // Not removable
        else
            setMovable();

        switch (accessType) {
          case Scalar::Int8:
          case Scalar::Uint8:
          case Scalar::Int16:
          case Scalar::Uint16:
          case Scalar::Int32:
          case Scalar::Uint32:
            setResultType(MIRType_Int32);
            break;
          case Scalar::Float32:
            setResultType(MIRType_Float32);
            break;
          case Scalar::Float64:
            setResultType(MIRType_Double);
            break;
          case Scalar::Float32x4:
            setResultType(MIRType_Float32x4);
            break;
          case Scalar::Int32x4:
            setResultType(MIRType_Int32x4);
            break;
          case Scalar::Uint8Clamped:
          case Scalar::MaxTypedArrayViewType:
            MOZ_CRASH("unexpected load heap in asm.js");
        }
    }

  public:
    INSTRUCTION_HEADER(AsmJSLoadHeap)

    static MAsmJSLoadHeap* New(TempAllocator& alloc, Scalar::Type accessType,
                               MDefinition* ptr, bool needsBoundsCheck,
                               unsigned numSimdElems = 0,
                               MemoryBarrierBits barrierBefore = MembarNobits,
                               MemoryBarrierBits barrierAfter = MembarNobits)
    {
        return new(alloc) MAsmJSLoadHeap(accessType, ptr, needsBoundsCheck,
                                         numSimdElems, barrierBefore, barrierAfter);
    }

    MDefinition* ptr() const { return getOperand(0); }
    void replacePtr(MDefinition* newPtr) { replaceOperand(0, newPtr); }

    bool congruentTo(const MDefinition* ins) const override;
    AliasSet getAliasSet() const override {
        // When a barrier is needed make the instruction effectful by
        // giving it a "store" effect.
        if (isAtomicAccess())
            return AliasSet::Store(AliasSet::AsmJSHeap);
        return AliasSet::Load(AliasSet::AsmJSHeap);
    }
    bool mightAlias(const MDefinition* def) const override;
};

class MAsmJSStoreHeap
  : public MBinaryInstruction,
    public MAsmJSHeapAccess,
    public NoTypePolicy::Data
{
    MAsmJSStoreHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* v, bool needsBoundsCheck,
                    unsigned numSimdElems, MemoryBarrierBits before, MemoryBarrierBits after)
      : MBinaryInstruction(ptr, v),
        MAsmJSHeapAccess(accessType, needsBoundsCheck, numSimdElems, before, after)
    {
        if (before|after)
            setGuard();         // Not removable
    }

  public:
    INSTRUCTION_HEADER(AsmJSStoreHeap)

    static MAsmJSStoreHeap* New(TempAllocator& alloc, Scalar::Type accessType,
                                MDefinition* ptr, MDefinition* v, bool needsBoundsCheck,
                                unsigned numSimdElems = 0,
                                MemoryBarrierBits barrierBefore = MembarNobits,
                                MemoryBarrierBits barrierAfter = MembarNobits)
    {
        return new(alloc) MAsmJSStoreHeap(accessType, ptr, v, needsBoundsCheck,
                                          numSimdElems, barrierBefore, barrierAfter);
    }

    MDefinition* ptr() const { return getOperand(0); }
    void replacePtr(MDefinition* newPtr) { replaceOperand(0, newPtr); }
    MDefinition* value() const { return getOperand(1); }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::AsmJSHeap);
    }
};

class MAsmJSCompareExchangeHeap
  : public MTernaryInstruction,
    public MAsmJSHeapAccess,
    public NoTypePolicy::Data
{
    MAsmJSCompareExchangeHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* oldv,
                              MDefinition* newv, bool needsBoundsCheck)
        : MTernaryInstruction(ptr, oldv, newv),
          MAsmJSHeapAccess(accessType, needsBoundsCheck)
    {
        setGuard();             // Not removable
        setResultType(MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(AsmJSCompareExchangeHeap)

    static MAsmJSCompareExchangeHeap* New(TempAllocator& alloc, Scalar::Type accessType,
                                          MDefinition* ptr, MDefinition* oldv,
                                          MDefinition* newv, bool needsBoundsCheck)
    {
        return new(alloc) MAsmJSCompareExchangeHeap(accessType, ptr, oldv, newv, needsBoundsCheck);
    }

    MDefinition* ptr() const { return getOperand(0); }
    MDefinition* oldValue() const { return getOperand(1); }
    MDefinition* newValue() const { return getOperand(2); }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::AsmJSHeap);
    }
};

class MAsmJSAtomicExchangeHeap
  : public MBinaryInstruction,
    public MAsmJSHeapAccess,
    public NoTypePolicy::Data
{
    MAsmJSAtomicExchangeHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* value,
                             bool needsBoundsCheck)
        : MBinaryInstruction(ptr, value),
          MAsmJSHeapAccess(accessType, needsBoundsCheck)
    {
        setGuard();             // Not removable
        setResultType(MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(AsmJSAtomicExchangeHeap)

    static MAsmJSAtomicExchangeHeap* New(TempAllocator& alloc, Scalar::Type accessType,
                                         MDefinition* ptr, MDefinition* value,
                                         bool needsBoundsCheck)
    {
        return new(alloc) MAsmJSAtomicExchangeHeap(accessType, ptr, value, needsBoundsCheck);
    }

    MDefinition* ptr() const { return getOperand(0); }
    MDefinition* value() const { return getOperand(1); }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::AsmJSHeap);
    }
};

class MAsmJSAtomicBinopHeap
  : public MBinaryInstruction,
    public MAsmJSHeapAccess,
    public NoTypePolicy::Data
{
    AtomicOp op_;

    MAsmJSAtomicBinopHeap(AtomicOp op, Scalar::Type accessType, MDefinition* ptr, MDefinition* v,
                          bool needsBoundsCheck)
        : MBinaryInstruction(ptr, v),
          MAsmJSHeapAccess(accessType, needsBoundsCheck),
          op_(op)
    {
        setGuard();         // Not removable
        setResultType(MIRType_Int32);
    }

  public:
    INSTRUCTION_HEADER(AsmJSAtomicBinopHeap)

    static MAsmJSAtomicBinopHeap* New(TempAllocator& alloc, AtomicOp op, Scalar::Type accessType,
                                      MDefinition* ptr, MDefinition* v, bool needsBoundsCheck)
    {
        return new(alloc) MAsmJSAtomicBinopHeap(op, accessType, ptr, v, needsBoundsCheck);
    }

    AtomicOp operation() const { return op_; }
    MDefinition* ptr() const { return getOperand(0); }
    MDefinition* value() const { return getOperand(1); }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::AsmJSHeap);
    }
};

class MAsmJSLoadGlobalVar : public MNullaryInstruction
{
    MAsmJSLoadGlobalVar(MIRType type, unsigned globalDataOffset, bool isConstant)
      : globalDataOffset_(globalDataOffset), isConstant_(isConstant)
    {
        MOZ_ASSERT(IsNumberType(type) || IsSimdType(type));
        setResultType(type);
        setMovable();
    }

    unsigned globalDataOffset_;
    bool isConstant_;

  public:
    INSTRUCTION_HEADER(AsmJSLoadGlobalVar)

    static MAsmJSLoadGlobalVar* New(TempAllocator& alloc, MIRType type, unsigned globalDataOffset,
                                    bool isConstant)
    {
        return new(alloc) MAsmJSLoadGlobalVar(type, globalDataOffset, isConstant);
    }

    unsigned globalDataOffset() const { return globalDataOffset_; }

    HashNumber valueHash() const override;
    bool congruentTo(const MDefinition* ins) const override;
    MDefinition* foldsTo(TempAllocator& alloc) override;

    AliasSet getAliasSet() const override {
        return isConstant_ ? AliasSet::None() : AliasSet::Load(AliasSet::AsmJSGlobalVar);
    }

    bool mightAlias(const MDefinition* def) const override;
};

class MAsmJSStoreGlobalVar
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    MAsmJSStoreGlobalVar(unsigned globalDataOffset, MDefinition* v)
      : MUnaryInstruction(v), globalDataOffset_(globalDataOffset)
    {}

    unsigned globalDataOffset_;

  public:
    INSTRUCTION_HEADER(AsmJSStoreGlobalVar)

    static MAsmJSStoreGlobalVar* New(TempAllocator& alloc, unsigned globalDataOffset, MDefinition* v) {
        return new(alloc) MAsmJSStoreGlobalVar(globalDataOffset, v);
    }

    unsigned globalDataOffset() const { return globalDataOffset_; }
    MDefinition* value() const { return getOperand(0); }

    AliasSet getAliasSet() const override {
        return AliasSet::Store(AliasSet::AsmJSGlobalVar);
    }
};

class MAsmJSLoadFuncPtr
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    MAsmJSLoadFuncPtr(unsigned globalDataOffset, MDefinition* index)
      : MUnaryInstruction(index), globalDataOffset_(globalDataOffset)
    {
        setResultType(MIRType_Pointer);
    }

    unsigned globalDataOffset_;

  public:
    INSTRUCTION_HEADER(AsmJSLoadFuncPtr)

    static MAsmJSLoadFuncPtr* New(TempAllocator& alloc, unsigned globalDataOffset,
                                  MDefinition* index)
    {
        return new(alloc) MAsmJSLoadFuncPtr(globalDataOffset, index);
    }

    unsigned globalDataOffset() const { return globalDataOffset_; }
    MDefinition* index() const { return getOperand(0); }

    HashNumber valueHash() const override;
    bool congruentTo(const MDefinition* ins) const override;
};

class MAsmJSLoadFFIFunc : public MNullaryInstruction
{
    explicit MAsmJSLoadFFIFunc(unsigned globalDataOffset)
      : globalDataOffset_(globalDataOffset)
    {
        setResultType(MIRType_Pointer);
    }

    unsigned globalDataOffset_;

  public:
    INSTRUCTION_HEADER(AsmJSLoadFFIFunc)

    static MAsmJSLoadFFIFunc* New(TempAllocator& alloc, unsigned globalDataOffset)
    {
        return new(alloc) MAsmJSLoadFFIFunc(globalDataOffset);
    }

    unsigned globalDataOffset() const { return globalDataOffset_; }

    HashNumber valueHash() const override;
    bool congruentTo(const MDefinition* ins) const override;
};

class MAsmJSParameter : public MNullaryInstruction
{
    ABIArg abi_;

    MAsmJSParameter(ABIArg abi, MIRType mirType)
      : abi_(abi)
    {
        setResultType(mirType);
    }

  public:
    INSTRUCTION_HEADER(AsmJSParameter)

    static MAsmJSParameter* New(TempAllocator& alloc, ABIArg abi, MIRType mirType) {
        return new(alloc) MAsmJSParameter(abi, mirType);
    }

    ABIArg abi() const { return abi_; }
};

class MAsmJSReturn
  : public MAryControlInstruction<1, 0>,
    public NoTypePolicy::Data
{
    explicit MAsmJSReturn(MDefinition* ins) {
        initOperand(0, ins);
    }

  public:
    INSTRUCTION_HEADER(AsmJSReturn)
    static MAsmJSReturn* New(TempAllocator& alloc, MDefinition* ins) {
        return new(alloc) MAsmJSReturn(ins);
    }
};

class MAsmJSVoidReturn
  : public MAryControlInstruction<0, 0>,
    public NoTypePolicy::Data
{
  public:
    INSTRUCTION_HEADER(AsmJSVoidReturn)
    static MAsmJSVoidReturn* New(TempAllocator& alloc) {
        return new(alloc) MAsmJSVoidReturn();
    }
};

class MAsmJSPassStackArg
  : public MUnaryInstruction,
    public NoTypePolicy::Data
{
    MAsmJSPassStackArg(uint32_t spOffset, MDefinition* ins)
      : MUnaryInstruction(ins),
        spOffset_(spOffset)
    {}

    uint32_t spOffset_;

  public:
    INSTRUCTION_HEADER(AsmJSPassStackArg)
    static MAsmJSPassStackArg* New(TempAllocator& alloc, uint32_t spOffset, MDefinition* ins) {
        return new(alloc) MAsmJSPassStackArg(spOffset, ins);
    }
    uint32_t spOffset() const {
        return spOffset_;
    }
    void incrementOffset(uint32_t inc) {
        spOffset_ += inc;
    }
    MDefinition* arg() const {
        return getOperand(0);
    }
};

class MAsmJSCall final
  : public MVariadicInstruction,
    public NoTypePolicy::Data
{
  public:
    class Callee {
      public:
        enum Which { Internal, Dynamic, Builtin };
      private:
        Which which_;
        union {
            AsmJSInternalCallee internal_;
            MDefinition* dynamic_;
            wasm::Builtin builtin_;
        } u;
      public:
        Callee() {}
        explicit Callee(AsmJSInternalCallee callee) : which_(Internal) { u.internal_ = callee; }
        explicit Callee(MDefinition* callee) : which_(Dynamic) { u.dynamic_ = callee; }
        explicit Callee(wasm::Builtin callee) : which_(Builtin) { u.builtin_ = callee; }
        Which which() const { return which_; }
        AsmJSInternalCallee internal() const { MOZ_ASSERT(which_ == Internal); return u.internal_; }
        MDefinition* dynamic() const { MOZ_ASSERT(which_ == Dynamic); return u.dynamic_; }
        wasm::Builtin builtin() const { MOZ_ASSERT(which_ == Builtin); return u.builtin_; }
    };

  private:
    wasm::CallSiteDesc desc_;
    Callee callee_;
    FixedList<AnyRegister> argRegs_;
    size_t spIncrement_;

    MAsmJSCall(const wasm::CallSiteDesc& desc, Callee callee, size_t spIncrement)
     : desc_(desc), callee_(callee), spIncrement_(spIncrement)
    { }

  public:
    INSTRUCTION_HEADER(AsmJSCall)

    struct Arg {
        AnyRegister reg;
        MDefinition* def;
        Arg(AnyRegister reg, MDefinition* def) : reg(reg), def(def) {}
    };
    typedef Vector<Arg, 8, SystemAllocPolicy> Args;

    static MAsmJSCall* New(TempAllocator& alloc, const wasm::CallSiteDesc& desc, Callee callee,
                           const Args& args, MIRType resultType, size_t spIncrement);

    size_t numArgs() const {
        return argRegs_.length();
    }
    AnyRegister registerForArg(size_t index) const {
        MOZ_ASSERT(index < numArgs());
        return argRegs_[index];
    }
    const wasm::CallSiteDesc& desc() const {
        return desc_;
    }
    Callee callee() const {
        return callee_;
    }
    size_t dynamicCalleeOperandIndex() const {
        MOZ_ASSERT(callee_.which() == Callee::Dynamic);
        MOZ_ASSERT(numArgs() == numOperands() - 1);
        return numArgs();
    }
    size_t spIncrement() const {
        return spIncrement_;
    }

    bool possiblyCalls() const override {
        return true;
    }
};

class MUnknownValue : public MNullaryInstruction
{
  protected:
    MUnknownValue() {
        setResultType(MIRType_Value);
    }

  public:
    INSTRUCTION_HEADER(UnknownValue)

    static MUnknownValue* New(TempAllocator& alloc) {
        return new(alloc) MUnknownValue();
    }
};

#undef INSTRUCTION_HEADER

void MUse::init(MDefinition* producer, MNode* consumer)
{
    MOZ_ASSERT(!consumer_, "Initializing MUse that already has a consumer");
    MOZ_ASSERT(!producer_, "Initializing MUse that already has a producer");
    initUnchecked(producer, consumer);
}

void MUse::initUnchecked(MDefinition* producer, MNode* consumer)
{
    MOZ_ASSERT(consumer, "Initializing to null consumer");
    consumer_ = consumer;
    producer_ = producer;
    producer_->addUseUnchecked(this);
}

void MUse::initUncheckedWithoutProducer(MNode* consumer)
{
    MOZ_ASSERT(consumer, "Initializing to null consumer");
    consumer_ = consumer;
    producer_ = nullptr;
}

void MUse::replaceProducer(MDefinition* producer)
{
    MOZ_ASSERT(consumer_, "Resetting MUse without a consumer");
    producer_->removeUse(this);
    producer_ = producer;
    producer_->addUse(this);
}

void MUse::releaseProducer()
{
    MOZ_ASSERT(consumer_, "Clearing MUse without a consumer");
    producer_->removeUse(this);
    producer_ = nullptr;
}

// Implement cast functions now that the compiler can see the inheritance.

MDefinition* MNode::toDefinition()
{
    MOZ_ASSERT(isDefinition());
    return (MDefinition*)this;
}

MResumePoint* MNode::toResumePoint()
{
    MOZ_ASSERT(isResumePoint());
    return (MResumePoint*)this;
}

MInstruction* MDefinition::toInstruction()
{
    MOZ_ASSERT(!isPhi());
    return (MInstruction*)this;
}

const MInstruction* MDefinition::toInstruction() const
{
    MOZ_ASSERT(!isPhi());
    return (const MInstruction*)this;
}

MControlInstruction* MDefinition::toControlInstruction() {
    MOZ_ASSERT(isControlInstruction());
    return (MControlInstruction*)this;
}

// Helper functions used to decide how to build MIR.

bool ElementAccessIsDenseNative(CompilerConstraintList* constraints,
                                MDefinition* obj, MDefinition* id);
JSValueType UnboxedArrayElementType(CompilerConstraintList* constraints, MDefinition* obj,
                                    MDefinition* id);
bool ElementAccessIsAnyTypedArray(CompilerConstraintList* constraints,
                                  MDefinition* obj, MDefinition* id,
                                  Scalar::Type* arrayType);
bool ElementAccessIsPacked(CompilerConstraintList* constraints, MDefinition* obj);
bool ElementAccessMightBeCopyOnWrite(CompilerConstraintList* constraints, MDefinition* obj);
bool ElementAccessHasExtraIndexedProperty(IonBuilder* builder, MDefinition* obj);
MIRType DenseNativeElementType(CompilerConstraintList* constraints, MDefinition* obj);
BarrierKind PropertyReadNeedsTypeBarrier(JSContext* propertycx,
                                         CompilerConstraintList* constraints,
                                         TypeSet::ObjectKey* key, PropertyName* name,
                                         TemporaryTypeSet* observed, bool updateObserved);
BarrierKind PropertyReadNeedsTypeBarrier(JSContext* propertycx,
                                         CompilerConstraintList* constraints,
                                         MDefinition* obj, PropertyName* name,
                                         TemporaryTypeSet* observed);
BarrierKind PropertyReadOnPrototypeNeedsTypeBarrier(IonBuilder* builder,
                                                    MDefinition* obj, PropertyName* name,
                                                    TemporaryTypeSet* observed);
bool PropertyReadIsIdempotent(CompilerConstraintList* constraints,
                              MDefinition* obj, PropertyName* name);
void AddObjectsForPropertyRead(MDefinition* obj, PropertyName* name,
                               TemporaryTypeSet* observed);
bool CanWriteProperty(TempAllocator& alloc, CompilerConstraintList* constraints,
                      HeapTypeSetKey property, MDefinition* value,
                      MIRType implicitType = MIRType_None);
bool PropertyWriteNeedsTypeBarrier(TempAllocator& alloc, CompilerConstraintList* constraints,
                                   MBasicBlock* current, MDefinition** pobj,
                                   PropertyName* name, MDefinition** pvalue,
                                   bool canModify, MIRType implicitType = MIRType_None);
bool ArrayPrototypeHasIndexedProperty(IonBuilder* builder, JSScript* script);
bool TypeCanHaveExtraIndexedProperties(IonBuilder* builder, TemporaryTypeSet* types);

} // namespace jit
} // namespace js

#endif /* jit_MIR_h */
