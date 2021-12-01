/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonControlFlow_h
#define jit_IonControlFlow_h

#include "mozilla/Array.h"

#include "jit/BytecodeAnalysis.h"
#include "jit/FixedList.h"
#include "jit/JitAllocPolicy.h"
#include "js/TypeDecls.h"

namespace js {
namespace jit {

class CFGControlInstruction;

// Adds MFoo::New functions which are mirroring the arguments of the
// constructors. Opcodes which are using this macro can be called with a
// TempAllocator, or the fallible version of the TempAllocator.
#define TRIVIAL_CFG_NEW_WRAPPERS                                              \
    template <typename... Args>                                               \
    static CFGThisOpcode* New(TempAllocator& alloc, Args&&... args) {         \
        return new(alloc) CFGThisOpcode(mozilla::Forward<Args>(args)...);     \
    }                                                                         \
    template <typename... Args>                                               \
    static CFGThisOpcode* New(TempAllocator::Fallible alloc, Args&&... args)  \
    {                                                                         \
        return new(alloc) CFGThisOpcode(mozilla::Forward<Args>(args)...);     \
    }

class CFGSpace
{
    static const size_t DEFAULT_CHUNK_SIZE = 4096;

  protected:
    LifoAlloc allocator_;
  public:

    explicit CFGSpace()
      : allocator_(DEFAULT_CHUNK_SIZE)
    {}

    LifoAlloc& lifoAlloc() {
        return allocator_;
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return allocator_.sizeOfExcludingThis(mallocSizeOf);
    }
};

class CFGBlock : public TempObject
{
    size_t id_;
    jsbytecode* start;
    jsbytecode* stop;
    CFGControlInstruction* end;
    bool inWorkList;

  public:
    explicit CFGBlock(jsbytecode* start)
      : id_(-1),
        start(start),
        stop(nullptr),
        end(nullptr),
        inWorkList(false)
    {}

    static CFGBlock* New(TempAllocator& alloc, jsbytecode* start) {
        return new(alloc) CFGBlock(start);
    }

    void operator=(const CFGBlock&) = delete;

    jsbytecode* startPc() const {
        return start;
    }
    void setStartPc(jsbytecode* startPc) {
        start = startPc;
    }
    jsbytecode* stopPc() const {
        MOZ_ASSERT(stop);
        return stop;
    }
    void setStopPc(jsbytecode* stopPc) {
        stop = stopPc;
    }
    CFGControlInstruction* stopIns() const {
        MOZ_ASSERT(end);
        return end;
    }
    void setStopIns(CFGControlInstruction* stopIns) {
        end = stopIns;
    }
    bool isInWorkList() const {
        return inWorkList;
    }
    void setInWorklist() {
        MOZ_ASSERT(!inWorkList);
        inWorkList = true;
    }
    void clearInWorkList() {
        MOZ_ASSERT(inWorkList);
        inWorkList = false;
    }
    size_t id() const {
        return id_;
    }
    void setId(size_t id) {
        id_ = id;
    }
};

#define CFG_CONTROL_OPCODE_LIST(_)                                          \
    _(Test)                                                                 \
    _(Compare)                                                              \
    _(Goto)                                                                 \
    _(Return)                                                               \
    _(RetRVal)                                                              \
    _(LoopEntry)                                                            \
    _(BackEdge)                                                             \
    _(TableSwitch)                                                          \
    _(Try)                                                                  \
    _(Throw)

// Forward declarations of MIR types.
#define FORWARD_DECLARE(type) class CFG##type;
 CFG_CONTROL_OPCODE_LIST(FORWARD_DECLARE)
#undef FORWARD_DECLARE

#define CFG_CONTROL_HEADER(type_name)                                        \
    static const Type classOpcode = CFGControlInstruction::Type_##type_name; \
    using CFGThisOpcode = CFG##type_name;                                    \
    Type type() const override {                                             \
        return classOpcode;                                                  \
    }                                                                        \
    const char* Name() const override {                                      \
        return #type_name;                                                   \
    }                                                                        \


class CFGControlInstruction : public TempObject
{
  public:
    enum Type {
#   define DEFINE_TYPES(type) Type_##type,
        CFG_CONTROL_OPCODE_LIST(DEFINE_TYPES)
#   undef DEFINE_TYPES
    };

    virtual size_t numSuccessors() const = 0;
    virtual CFGBlock* getSuccessor(size_t i) const = 0;
    virtual void replaceSuccessor(size_t i, CFGBlock* successor) = 0;
    virtual Type type() const = 0;
    virtual const char* Name() const = 0;

    template<typename CFGType> bool is() const {
        return type() == CFGType::classOpcode;
    }
    template<typename CFGType> CFGType* to() {
        MOZ_ASSERT(this->is<CFGType>());
        return static_cast<CFGType*>(this);
    }
    template<typename CFGType> const CFGType* to() const {
        MOZ_ASSERT(this->is<CFGType>());
        return static_cast<const CFGType*>(this);
    }
#   define TYPE_CASTS(type)             \
    bool is##type() const {             \
        return this->is<CFG##type>();   \
    }                                   \
    CFG##type* to##type() {             \
        return this->to<CFG##type>();   \
    }                                   \
    const CFG##type* to##type() const { \
        return this->to<CFG##type>();   \
    }
    CFG_CONTROL_OPCODE_LIST(TYPE_CASTS)
#   undef TYPE_CASTS
};

template <size_t Successors>
class CFGAryControlInstruction : public CFGControlInstruction
{
    mozilla::Array<CFGBlock*, Successors> successors_;

  public:
    size_t numSuccessors() const final {
        return Successors;
    }
    CFGBlock* getSuccessor(size_t i) const final {
        return successors_[i];
    }
    void replaceSuccessor(size_t i, CFGBlock* succ) final {
        successors_[i] = succ;
    }
};

class CFGTry : public CFGControlInstruction
{
    CFGBlock* tryBlock_;
    jsbytecode* catchStartPc_;
    CFGBlock* mergePoint_;

    CFGTry(CFGBlock* successor, jsbytecode* catchStartPc, CFGBlock* mergePoint)
      : tryBlock_(successor),
        catchStartPc_(catchStartPc),
        mergePoint_(mergePoint)
    { }

  public:
    CFG_CONTROL_HEADER(Try)
    TRIVIAL_CFG_NEW_WRAPPERS

    static CFGTry* CopyWithNewTargets(TempAllocator& alloc, CFGTry* old,
                                      CFGBlock* tryBlock, CFGBlock* merge)
    {
        return new(alloc) CFGTry(tryBlock, old->catchStartPc(), merge);
    }

    size_t numSuccessors() const final {
        return 2;
    }
    CFGBlock* getSuccessor(size_t i) const final {
        MOZ_ASSERT(i < numSuccessors());
        return (i == 0) ? tryBlock_ : mergePoint_;
    }
    void replaceSuccessor(size_t i, CFGBlock* succ) final {
        MOZ_ASSERT(i < numSuccessors());
        if (i == 0)
            tryBlock_ = succ;
        else
            mergePoint_ = succ;
    }

    CFGBlock* tryBlock() const {
        return getSuccessor(0);
    }

    jsbytecode* catchStartPc() const {
        return catchStartPc_;
    }

    CFGBlock* afterTryCatchBlock() const {
        return getSuccessor(1);
    }
};

class CFGTableSwitch : public CFGControlInstruction
{
    Vector<CFGBlock*, 4, JitAllocPolicy> successors_;
    size_t low_;
    size_t high_;

    CFGTableSwitch(TempAllocator& alloc, size_t low, size_t high)
      : successors_(alloc),
        low_(low),
        high_(high)
    {}

  public:
    CFG_CONTROL_HEADER(TableSwitch);

    static CFGTableSwitch* New(TempAllocator& alloc, size_t low, size_t high) {
        return new(alloc) CFGTableSwitch(alloc, low, high);
    }

    size_t numSuccessors() const final {
        return successors_.length();
    }
    CFGBlock* getSuccessor(size_t i) const final {
        MOZ_ASSERT(i < numSuccessors());
        return successors_[i];
    }
    void replaceSuccessor(size_t i, CFGBlock* succ) final {
        MOZ_ASSERT(i < numSuccessors());
        successors_[i] = succ;
    }

    bool addDefault(CFGBlock* defaultCase) {
        MOZ_ASSERT(successors_.length() == 0);
        return successors_.append(defaultCase);
    }

    bool addCase(CFGBlock* caseBlock) {
        MOZ_ASSERT(successors_.length() > 0);
        return successors_.append(caseBlock);
    }

    CFGBlock* defaultCase() const {
        return getSuccessor(0);
    }

    CFGBlock* getCase(size_t i) const {
        return getSuccessor(i + 1);
    }

    size_t high() const {
        return high_;
    }

    size_t low() const {
        return low_;
    }
};

/**
 * CFGCompare
 *
 * PEEK
 * PEEK
 * STRICTEQ
 *    POP truePopAmount
 *    JUMP succ1
 * STRICTNEQ
 *    POP falsePopAmount
 *    JUMP succ2
 */
class CFGCompare : public CFGAryControlInstruction<2>
{
    const size_t truePopAmount_;
    const size_t falsePopAmount_;

    CFGCompare(CFGBlock* succ1, size_t truePopAmount, CFGBlock* succ2, size_t falsePopAmount)
      : truePopAmount_(truePopAmount),
        falsePopAmount_(falsePopAmount)
    {
        replaceSuccessor(0, succ1);
        replaceSuccessor(1, succ2);
    }

  public:
    CFG_CONTROL_HEADER(Compare);

    static CFGCompare* NewFalseBranchIsDefault(TempAllocator& alloc, CFGBlock* case_,
                                               CFGBlock* default_)
    {
        // True and false branch both go to a body and don't need the lhs and
        // rhs to the compare. Pop them.
        return new(alloc) CFGCompare(case_, 2, default_, 2);
    }

    static CFGCompare* NewFalseBranchIsNextCompare(TempAllocator& alloc, CFGBlock* case_,
                                                   CFGBlock* nextCompare)
    {
        // True branch goes to the body and don't need the lhs and
        // rhs to the compare anymore. Pop them. The next compare still
        // needs the lhs.
        return new(alloc) CFGCompare(case_, 2, nextCompare, 1);
    }

    static CFGCompare* CopyWithNewTargets(TempAllocator& alloc, CFGCompare* old,
                                          CFGBlock* succ1, CFGBlock* succ2)
    {
        return new(alloc) CFGCompare(succ1, old->truePopAmount(), succ2, old->falsePopAmount());
    }

    CFGBlock* trueBranch() const {
        return getSuccessor(0);
    }
    CFGBlock* falseBranch() const {
        return getSuccessor(1);
    }
    size_t truePopAmount() const {
        return truePopAmount_;
    }
    size_t falsePopAmount() const {
        return falsePopAmount_;
    }
};

/**
 * CFGTest
 *
 * POP / PEEK (depending on keepCondition_)
 * IFEQ JUMP succ1
 * IFNEQ JUMP succ2
 *
 */
class CFGTest : public CFGAryControlInstruction<2>
{
    // By default the condition gets popped. This variable
    // keeps track if we want to keep the condition.
    bool keepCondition_;

    CFGTest(CFGBlock* succ1, CFGBlock* succ2)
      : keepCondition_(false)
    {
        replaceSuccessor(0, succ1);
        replaceSuccessor(1, succ2);
    }
    CFGTest(CFGBlock* succ1, CFGBlock* succ2, bool keepCondition)
      : keepCondition_(keepCondition)
    {
        replaceSuccessor(0, succ1);
        replaceSuccessor(1, succ2);
    }

  public:
    CFG_CONTROL_HEADER(Test);
    TRIVIAL_CFG_NEW_WRAPPERS

    static CFGTest* CopyWithNewTargets(TempAllocator& alloc, CFGTest* old,
                                       CFGBlock* succ1, CFGBlock* succ2)
    {
        return new(alloc) CFGTest(succ1, succ2, old->mustKeepCondition());
    }

    CFGBlock* trueBranch() const {
        return getSuccessor(0);
    }
    CFGBlock* falseBranch() const {
        return getSuccessor(1);
    }
    void keepCondition() {
        keepCondition_ = true;
    }
    bool mustKeepCondition() const {
        return keepCondition_;
    }
};

/**
 * CFGReturn
 *
 * POP
 * RETURN popped value
 *
 */
class CFGReturn : public CFGAryControlInstruction<0>
{
  public:
    CFG_CONTROL_HEADER(Return);
    TRIVIAL_CFG_NEW_WRAPPERS
};

/**
 * CFGRetRVal
 *
 * RETURN the value in the return value slot
 *
 */
class CFGRetRVal : public CFGAryControlInstruction<0>
{
  public:
    CFG_CONTROL_HEADER(RetRVal);
    TRIVIAL_CFG_NEW_WRAPPERS
};

/**
 * CFGThrow
 *
 * POP
 * THROW popped value
 *
 */
class CFGThrow : public CFGAryControlInstruction<0>
{
  public:
    CFG_CONTROL_HEADER(Throw);
    TRIVIAL_CFG_NEW_WRAPPERS
};

class CFGUnaryControlInstruction : public CFGAryControlInstruction<1>
{
  public:
    explicit CFGUnaryControlInstruction(CFGBlock* block) {
        MOZ_ASSERT(block);
        replaceSuccessor(0, block);
    }

    CFGBlock* successor() const {
        return getSuccessor(0);
    }
};

/**
 * CFGGOTO
 *
 * POP (x popAmount)
 * JMP block
 *
 */
class CFGGoto : public CFGUnaryControlInstruction
{
    const size_t popAmount_;

    explicit CFGGoto(CFGBlock* block)
      : CFGUnaryControlInstruction(block),
        popAmount_(0)
    {}

    CFGGoto(CFGBlock* block, size_t popAmount_)
      : CFGUnaryControlInstruction(block),
        popAmount_(popAmount_)
    {}

  public:
    CFG_CONTROL_HEADER(Goto);
    TRIVIAL_CFG_NEW_WRAPPERS

    static CFGGoto* CopyWithNewTargets(TempAllocator& alloc, CFGGoto* old, CFGBlock* block)
    {
        return new(alloc) CFGGoto(block, old->popAmount());
    }

    size_t popAmount() const {
        return popAmount_;
    }
};

/**
 * CFGBackEdge
 *
 * Jumps back to the start of the loop.
 *
 * JMP block
 *
 */
class CFGBackEdge : public CFGUnaryControlInstruction
{
    explicit CFGBackEdge(CFGBlock* block)
      : CFGUnaryControlInstruction(block)
    {}

  public:
    CFG_CONTROL_HEADER(BackEdge);
    TRIVIAL_CFG_NEW_WRAPPERS

    static CFGBackEdge* CopyWithNewTargets(TempAllocator& alloc, CFGBackEdge* old, CFGBlock* block)
    {
        return new(alloc) CFGBackEdge(block);
    }
};

/**
 * CFGLOOPENTRY
 *
 * Indicates the jumping block is the start of a loop.
 * That block is the only block allowed to have a backedge.
 *
 * JMP block
 *
 */
class CFGLoopEntry : public CFGUnaryControlInstruction
{
    bool canOsr_;
    bool isForIn_;
    size_t stackPhiCount_;
    jsbytecode* loopStopPc_;

    CFGLoopEntry(CFGBlock* block, size_t stackPhiCount)
      : CFGUnaryControlInstruction(block),
        canOsr_(false),
        isForIn_(false),
        stackPhiCount_(stackPhiCount),
        loopStopPc_(nullptr)
    {}

    CFGLoopEntry(CFGBlock* block, bool canOsr, bool isForIn, size_t stackPhiCount,
                 jsbytecode* loopStopPc)
      : CFGUnaryControlInstruction(block),
        canOsr_(canOsr),
        isForIn_(isForIn),
        stackPhiCount_(stackPhiCount),
        loopStopPc_(loopStopPc)
    {}

  public:
    CFG_CONTROL_HEADER(LoopEntry);
    TRIVIAL_CFG_NEW_WRAPPERS

    static CFGLoopEntry* CopyWithNewTargets(TempAllocator& alloc, CFGLoopEntry* old,
                                            CFGBlock* loopEntry)
    {
        return new(alloc) CFGLoopEntry(loopEntry, old->canOsr(), old->isForIn(),
                                       old->stackPhiCount(), old->loopStopPc());
    }

    void setCanOsr() {
        canOsr_ = true;
    }

    bool canOsr() const {
        return canOsr_;
    }

    void setIsForIn() {
        isForIn_ = true;
    }
    bool isForIn() const {
        return isForIn_;
    }

    size_t stackPhiCount() const {
        return stackPhiCount_;
    }

    jsbytecode* loopStopPc() const {
        MOZ_ASSERT(loopStopPc_);
        return loopStopPc_;
    }

    void setLoopStopPc(jsbytecode* loopStopPc) {
        loopStopPc_ = loopStopPc;
    }
};

typedef Vector<CFGBlock*, 4, JitAllocPolicy> CFGBlockVector;

class ControlFlowGraph : public TempObject
{
    // A list of blocks in RPO, containing per block a pc-range and
    // a control instruction.
    Vector<CFGBlock, 4, JitAllocPolicy> blocks_;

    explicit ControlFlowGraph(TempAllocator& alloc)
      : blocks_(alloc)
    {}

  public:
    static ControlFlowGraph* New(TempAllocator& alloc) {
        return new(alloc) ControlFlowGraph(alloc);
    }

    ControlFlowGraph(const ControlFlowGraph&) = delete;
    void operator=(const ControlFlowGraph&) = delete;

    void dump(GenericPrinter& print, JSScript* script);
    bool init(TempAllocator& alloc, const CFGBlockVector& blocks);

    const CFGBlock* block(size_t i) const {
        return &blocks_[i];
    }

    size_t numBlocks() const {
        return blocks_.length();
    }
};

class ControlFlowGenerator
{
    static int CmpSuccessors(const void* a, const void* b);

    JSScript* script;
    CFGBlock* current;
    jsbytecode* pc;
    GSNCache gsn;
    TempAllocator& alloc_;
    CFGBlockVector blocks_;

  public:
    ControlFlowGenerator(const ControlFlowGenerator&) = delete;
    void operator=(const ControlFlowGenerator&) = delete;

    TempAllocator& alloc() {
        return alloc_;
    }

    enum class ControlStatus {
        Error,
        Abort,
        Ended,        // There is no continuation/join point.
        Joined,       // Created a join node.
        Jumped,       // Parsing another branch at the same level.
        None          // No control flow.
    };

    struct DeferredEdge : public TempObject
    {
        CFGBlock* block;
        DeferredEdge* next;

        DeferredEdge(CFGBlock* block, DeferredEdge* next)
          : block(block), next(next)
        { }
    };

    struct ControlFlowInfo {
        // Entry in the cfgStack.
        uint32_t cfgEntry;

        // Label that continues go to.
        jsbytecode* continuepc;

        ControlFlowInfo(uint32_t cfgEntry, jsbytecode* continuepc)
          : cfgEntry(cfgEntry),
            continuepc(continuepc)
        { }
    };


    // To avoid recursion, the bytecode analyzer uses a stack where each entry
    // is a small state machine. As we encounter branches or jumps in the
    // bytecode, we push information about the edges on the stack so that the
    // CFG can be built in a tree-like fashion.
    struct CFGState {
        enum State {
            IF_TRUE,            // if() { }, no else.
            IF_TRUE_EMPTY_ELSE, // if() { }, empty else
            IF_ELSE_TRUE,       // if() { X } else { }
            IF_ELSE_FALSE,      // if() { } else { X }
            DO_WHILE_LOOP_BODY, // do { x } while ()
            DO_WHILE_LOOP_COND, // do { } while (x)
            WHILE_LOOP_COND,    // while (x) { }
            WHILE_LOOP_BODY,    // while () { x }
            FOR_LOOP_COND,      // for (; x;) { }
            FOR_LOOP_BODY,      // for (; ;) { x }
            FOR_LOOP_UPDATE,    // for (; ; x) { }
            TABLE_SWITCH,       // switch() { x }
            COND_SWITCH_CASE,   // switch() { case X: ... }
            COND_SWITCH_BODY,   // switch() { case ...: X }
            AND_OR,             // && x, || x
            LABEL,              // label: x
            TRY                 // try { x } catch(e) { }
        };

        State state;            // Current state of this control structure.
        jsbytecode* stopAt;     // Bytecode at which to stop the processing loop.

        // For if structures, this contains branch information.
        union {
            struct {
                CFGBlock* ifFalse;
                jsbytecode* falseEnd;
                CFGBlock* ifTrue;    // Set when the end of the true path is reached.
                CFGTest* test;
            } branch;
            struct {
                // Common entry point.
                CFGBlock* entry;

                // Position of where the loop body starts and ends.
                jsbytecode* bodyStart;
                jsbytecode* bodyEnd;

                // pc immediately after the loop exits.
                jsbytecode* exitpc;

                // Common exit point. Created lazily, so it may be nullptr.
                CFGBlock* successor;

                // Deferred break and continue targets.
                DeferredEdge* breaks;
                DeferredEdge* continues;

                // Initial state, in case loop processing is restarted.
                State initialState;
                jsbytecode* initialPc;
                jsbytecode* initialStopAt;
                jsbytecode* loopHead;

                // For-loops only.
                jsbytecode* condpc;
                jsbytecode* updatepc;
                jsbytecode* updateEnd;
            } loop;
            struct {
                // Vector of body blocks to process after the cases.
                FixedList<CFGBlock*>* bodies;

                // When processing case statements, this counter points at the
                // last uninitialized body.  When processing bodies, this
                // counter targets the next body to process.
                uint32_t currentIdx;

                // Remember the block index of the default case.
                jsbytecode* defaultTarget;
                uint32_t defaultIdx;

                // Block immediately after the switch.
                jsbytecode* exitpc;
                DeferredEdge* breaks;
            } switch_;
            struct {
                DeferredEdge* breaks;
            } label;
            struct {
                CFGBlock* successor;
            } try_;
        };

        inline bool isLoop() const {
            switch (state) {
              case DO_WHILE_LOOP_COND:
              case DO_WHILE_LOOP_BODY:
              case WHILE_LOOP_COND:
              case WHILE_LOOP_BODY:
              case FOR_LOOP_COND:
              case FOR_LOOP_BODY:
              case FOR_LOOP_UPDATE:
                return true;
              default:
                return false;
            }
        }

        static CFGState If(jsbytecode* join, CFGTest* test);
        static CFGState IfElse(jsbytecode* trueEnd, jsbytecode* falseEnd, CFGTest* test);
        static CFGState AndOr(jsbytecode* join, CFGBlock* lhs);
        static CFGState TableSwitch(TempAllocator& alloc, jsbytecode* exitpc);
        static CFGState CondSwitch(TempAllocator& alloc, jsbytecode* exitpc,
                                   jsbytecode* defaultTarget);
        static CFGState Label(jsbytecode* exitpc);
        static CFGState Try(jsbytecode* exitpc, CFGBlock* successor);
    };

    Vector<CFGState, 8, JitAllocPolicy> cfgStack_;
    Vector<ControlFlowInfo, 4, JitAllocPolicy> loops_;
    Vector<ControlFlowInfo, 0, JitAllocPolicy> switches_;
    Vector<ControlFlowInfo, 2, JitAllocPolicy> labels_;
    bool aborted_;
    bool checkedTryFinally_;

  public:
    ControlFlowGenerator(TempAllocator& alloc, JSScript* script);

    MOZ_MUST_USE bool traverseBytecode();
    MOZ_MUST_USE bool addBlock(CFGBlock* block);
    ControlFlowGraph* getGraph(TempAllocator& alloc) {
        ControlFlowGraph* cfg = ControlFlowGraph::New(alloc);
        if (!cfg)
            return nullptr;
        if (!cfg->init(alloc, blocks_))
            return nullptr;
        return cfg;
    }

    bool aborted() const {
        return aborted_;
    }

  private:
    void popCfgStack();
    MOZ_MUST_USE bool processDeferredContinues(CFGState& state);
    ControlStatus processControlEnd();
    ControlStatus processCfgStack();
    ControlStatus processCfgEntry(CFGState& state);
    ControlStatus processIfStart(JSOp op);
    ControlStatus processIfEnd(CFGState& state);
    ControlStatus processIfElseTrueEnd(CFGState& state);
    ControlStatus processIfElseFalseEnd(CFGState& state);
    ControlStatus processDoWhileLoop(jssrcnote* sn);
    ControlStatus processDoWhileBodyEnd(CFGState& state);
    ControlStatus processDoWhileCondEnd(CFGState& state);
    ControlStatus processWhileCondEnd(CFGState& state);
    ControlStatus processWhileBodyEnd(CFGState& state);
    ControlStatus processForLoop(JSOp op, jssrcnote* sn);
    ControlStatus processForCondEnd(CFGState& state);
    ControlStatus processForBodyEnd(CFGState& state);
    ControlStatus processForUpdateEnd(CFGState& state);
    ControlStatus processWhileOrForInLoop(jssrcnote* sn);
    ControlStatus processNextTableSwitchCase(CFGState& state);
    ControlStatus processCondSwitch();
    ControlStatus processCondSwitchCase(CFGState& state);
    ControlStatus processCondSwitchDefault(CFGState& state);
    ControlStatus processCondSwitchBody(CFGState& state);
    ControlStatus processSwitchBreak(JSOp op);
    ControlStatus processSwitchEnd(DeferredEdge* breaks, jsbytecode* exitpc);
    ControlStatus processTry();
    ControlStatus processTryEnd(CFGState& state);
    ControlStatus processThrow();
    ControlStatus processTableSwitch(JSOp op, jssrcnote* sn);
    ControlStatus processContinue(JSOp op);
    ControlStatus processBreak(JSOp op, jssrcnote* sn);
    ControlStatus processReturn(JSOp op);
    ControlStatus maybeLoop(JSOp op, jssrcnote* sn);
    ControlStatus snoopControlFlow(JSOp op);
    ControlStatus processBrokenLoop(CFGState& state);
    ControlStatus finishLoop(CFGState& state, CFGBlock* successor);
    ControlStatus processAndOr(JSOp op);
    ControlStatus processAndOrEnd(CFGState& state);
    ControlStatus processLabel();
    ControlStatus processLabelEnd(CFGState& state);

    MOZ_MUST_USE bool pushLoop(CFGState::State state, jsbytecode* stopAt, CFGBlock* entry,
                               jsbytecode* loopHead, jsbytecode* initialPc,
                               jsbytecode* bodyStart, jsbytecode* bodyEnd,
                               jsbytecode* exitpc, jsbytecode* continuepc);
    void endCurrentBlock(CFGControlInstruction* ins);
    CFGBlock* createBreakCatchBlock(DeferredEdge* edge, jsbytecode* pc);
};

} // namespace jit
} // namespace js

#endif /* jit_IonControlFlow_h */
