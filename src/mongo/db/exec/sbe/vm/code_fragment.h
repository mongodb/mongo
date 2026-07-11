// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm_builtin.h"
#include "mongo/db/exec/sbe/vm/vm_instruction.h"
#include "mongo/db/exec/sbe/vm/vm_types.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
namespace vm {
/**
 * Represents a fragment of byte code executable in the ByteCode VM.
 */
class CodeFragment {
public:
    enum PrintFormat {
        /** Debug format, that prints all memory pointers. */
        Debug,
        /** Stable format, that prints relative offsets and masks other memory pointers. */
        Stable
    };
    const auto& frames() const {
        return _frames;
    }
    auto& instrs() {
        return _instrs;
    }
    const auto& instrs() const {
        return _instrs;
    }
    auto stackSize() const {
        return _stackSize;
    }
    auto maxStackSize() const {
        return _maxStackSize;
    }

    void append(CodeFragment&& code);
    void appendNoStack(CodeFragment&& code);
    // Used when only one of the fragments will run, but not more. This method will adjust the stack
    // size once in this call, rather than N (once for each CodeFragment). The CodeFragments
    // must have the same stack size for us to know how to adjust the stack at compile time.
    void append(std::vector<CodeFragment>&& fragments);
    void appendConstVal(value::TypeTags tag, value::Value val);
    void appendAccessVal(value::SlotAccessor* accessor);
    void appendMoveVal(value::SlotAccessor* accessor);
    void appendLocalVal(FrameId frameId, int variable, bool moveFrom);
    void appendLocalLambda(int codePosition, size_t numArgs);
    void appendPop();
    void appendSwapAndPop(size_t numPops);
    void appendMakeOwn(Instruction::Parameter arg);
    void appendAdd(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendSub(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendMul(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendDiv(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendIDiv(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendMod(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendNegate(Instruction::Parameter input);
    void appendNot(Instruction::Parameter input);
    void appendLess(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendLessEq(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendGreater(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendGreaterEq(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendEq(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendNeq(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendCmp3w(Instruction::Parameter lhs, Instruction::Parameter rhs);

    void appendCollLess(Instruction::Parameter lhs,
                        Instruction::Parameter rhs,
                        Instruction::Parameter collator);

    void appendCollLessEq(Instruction::Parameter lhs,
                          Instruction::Parameter rhs,
                          Instruction::Parameter collator);

    void appendCollGreater(Instruction::Parameter lhs,
                           Instruction::Parameter rhs,
                           Instruction::Parameter collator);

    void appendCollGreaterEq(Instruction::Parameter lhs,
                             Instruction::Parameter rhs,
                             Instruction::Parameter collator);

    void appendCollEq(Instruction::Parameter lhs,
                      Instruction::Parameter rhs,
                      Instruction::Parameter collator);

    void appendCollNeq(Instruction::Parameter lhs,
                       Instruction::Parameter rhs,
                       Instruction::Parameter collator);

    void appendCollCmp3w(Instruction::Parameter lhs,
                         Instruction::Parameter rhs,
                         Instruction::Parameter collator);

    void appendFillEmpty();
    void appendFillEmpty(Instruction::Constants k);
    void appendGetField(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendGetField(Instruction::Parameter input, std::string_view fieldName);
    void appendGetElement(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendCollComparisonKey(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendGetFieldOrElement(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendTraverseP();
    void appendTraverseP(int codePosition, size_t numArgs, Instruction::Constants k);
    void appendTraverseF();
    void appendTraverseF(int codePosition, size_t numArgs, Instruction::Constants k);
    void appendGetArraySize(Instruction::Parameter input);
    void appendDateTrunc(TimeUnit unit, int64_t binSize, TimeZone timezone, DayOfWeek startOfWeek);
    void appendValueBlockApplyLambda();

    void appendSum();
    void appendCount();
    void appendMin();
    void appendMax();
    void appendFirst();
    void appendLast();
    void appendCollMin();
    void appendCollMax();
    void appendExists(Instruction::Parameter input);
    void appendIsNull(Instruction::Parameter input);
    void appendIsNullish(Instruction::Parameter input);
    void appendIsObject(Instruction::Parameter input);
    void appendIsArray(Instruction::Parameter input);
    void appendIsInList(Instruction::Parameter input);
    void appendIsString(Instruction::Parameter input);
    void appendIsNumber(Instruction::Parameter input);
    void appendIsBinData(Instruction::Parameter input);
    void appendIsDate(Instruction::Parameter input);
    void appendIsNaN(Instruction::Parameter input);
    void appendIsInfinity(Instruction::Parameter input);
    void appendIsRecordId(Instruction::Parameter input);
    void appendIsMinKey(Instruction::Parameter input);
    void appendIsMaxKey(Instruction::Parameter input);
    void appendIsTimestamp(Instruction::Parameter input);
    void appendIsKeyString(Instruction::Parameter input);
    void appendTypeMatch(Instruction::Parameter input, uint32_t mask);
    void appendFunction(Builtin f, ArityType arity);
    void appendLabelJump(LabelId labelId);
    void appendLabelJumpTrue(LabelId labelId);
    void appendLabelJumpFalse(LabelId labelId);
    void appendLabelJumpNothing(LabelId labelId);
    void appendLabelJumpNotNothing(LabelId labelId);
    void appendRet();
    void appendAllocStack(uint32_t size);
    void appendFail();
    void appendNumericConvert(value::TypeTags targetTag);

    // For printing from an interactive debugger.
    std::string toString(
        vm::CodeFragment::PrintFormat format = vm::CodeFragment::PrintFormat::Debug) const;

    // Declares and defines a local variable frame at the current depth.
    // Local frame declaration is used to resolve the stack offsets of local variable access.
    // All references local variables must have matching frame declaration. The
    // variable reference and frame declaration is allowed to happen in any order.
    void declareFrame(FrameId frameId);

    // Declares and defines a local variable frame at the current stack depth modifies by the given
    // offset.
    void declareFrame(FrameId frameId, int stackOffset);

    // Removes the frame from scope. The frame must have no outstanding fixups.
    // That is: must be declared or never referenced.
    void removeFrame(FrameId frameId);

    // Returns whether the are any frames currently in scope.
    bool hasFrames() const;

    // Associates the current code position with a label.
    void appendLabel(LabelId labelId);

    // Removes the label from scope. The label must have no outstanding fixups.
    // That is: must be associated with code position or never referenced.
    void removeLabel(LabelId labelId);

    void validate();

private:
    // Adjusts all the stack offsets in the outstanding fixups by the provided delta as follows: for
    // a given 'stackOffsetDelta' of frames in this CodeFragment:
    //   1. Adds this delta to the 'stackPosition' of all frames having a defined stack position.
    //   2. Adds this delta to all uses of frame stack posn's in code (located at 'fixupOffset's).
    // The net effect is to change the stack offsets of all frames with defined stack positions and
    // all code references to frame offsets in this CodeFragment by 'stackOffsetDelta'.
    void fixupStackOffsets(int stackOffsetDelta);

    // Stores the fixup information for stack frames.
    // fixupOffsets - byte offsets in the code where the stack depth of the frame was used and need
    //   fixup.
    // stackPosition - stack depth in elements of where the frame was declared, or kPositionNotSet
    //   if not known yet.
    struct FrameInfo {
        static constexpr int64_t kPositionNotSet = std::numeric_limits<int64_t>::min();

        absl::InlinedVector<size_t, 2> fixupOffsets;
        int64_t stackPosition{kPositionNotSet};
    };

    // Stores the fixup information for labels.
    // fixupOffsets - offsets in the code where the label was used and need fixup.
    // definitionOffset - offset in the code where label was defined.
    struct LabelInfo {
        static constexpr int64_t kOffsetNotSet = std::numeric_limits<int64_t>::min();
        absl::InlinedVector<size_t, 2> fixupOffsets;
        int64_t definitionOffset{kOffsetNotSet};
    };

    template <typename... Ts>
    void appendSimpleInstruction(Instruction::Tags tag, Ts&&... params);
    void appendLabelJumpInstruction(LabelId labelId, Instruction::Tags tag);

    auto allocateSpace(size_t size) {
        auto oldSize = _instrs.size();
        _instrs.resize(oldSize + size);
        return _instrs.data() + oldSize;
    }

    template <typename... Ts>
    void adjustStackSimple(const Instruction& i, Ts&&... params);
    void copyCodeAndFixup(CodeFragment&& from);

    template <typename... Ts>
    size_t appendParameters(uint8_t* ptr, Ts&&... params);
    size_t appendParameter(uint8_t* ptr, Instruction::Parameter param, int& popCompensation);

    // Convert a variable index to a stack offset.
    constexpr int varToOffset(int var) const {
        return -var - 1;
    }

    // Returns the frame with ID 'frameId' if it already exists, else creates and returns it.
    FrameInfo& getOrDeclareFrame(FrameId frameId);

    // For a given 'frame' in this CodeFragment, subtracts the frame's 'stackPosition' from all the
    // refs to this frame in code (located at 'fixupOffset's). This is done once the true stack
    // position of the frame is known, so code refs point to the correct location in the frame.
    void fixupFrame(FrameInfo& frame);

    LabelInfo& getOrDeclareLabel(LabelId labelId);
    void fixupLabel(LabelInfo& label);

    // The sequence of byte code instructions this CodeFragment represents.
    absl::InlinedVector<uint8_t, 16> _instrs;

    // A collection of frame information for local variables.
    // Variables can be declared or referenced out of order and at the time of variable reference
    // it may not be known the relative stack offset of variable declaration w.r.t to its use.
    // This tracks both declaration info (stack depth) and use info (code offset).
    // When code is concatenated the offsets are adjusted if needed and when declaration stack depth
    // becomes known all fixups are resolved.
    absl::flat_hash_map<FrameId, FrameInfo> _frames;

    // A collection of label information for labels that are currently in scope.
    // Labels can be defined or referenced out of order and at at time of label reference (e.g:
    // jumps or lambda creation), the exact relative offset may not be yet known.
    // This tracks both label definition (code offset where label is defined) and use info for jumps
    // or lambdas (code offset). When code is concatenated the offsets are adjusted, if needed, and
    // when label definition offset becomes known all fixups are resolved.
    absl::flat_hash_map<LabelId, LabelInfo> _labels;

    // Delta number of '_argStack' entries effect of this CodeFragment; may be negative.
    int64_t _stackSize{0};

    // Maximum absolute number of entries in '_argStack' from this CodeFragment.
    int64_t _maxStackSize{0};
};  // class CodeFragment

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
