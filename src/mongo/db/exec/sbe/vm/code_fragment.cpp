/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/sbe/vm/code_fragment.h"

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace sbe {
namespace vm {
std::string CodeFragment::toString() const {
    std::ostringstream ss;
    vm::CodeFragmentPrinter printer(vm::CodeFragmentPrinter::PrintFormat::Debug);
    printer.print(ss, *this);
    return ss.str();
}

template <typename... Ts>
void CodeFragment::adjustStackSimple(const Instruction& i, Ts&&... params) {
    // Get the stack delta from a table.
    int delta = Instruction::stackOffset[i.tag];
    // And adjust it by parameters coming from frames.
    ((delta += params.frameId ? 1 : 0), ...);

    _stackSize += delta;

    // Only if we grow the stack can we affect the maximum size.
    if (delta > 0) {
        _maxStackSize = std::max(_maxStackSize, _stackSize);
    }
}

void CodeFragment::declareFrame(FrameId frameId) {
    declareFrame(frameId, 0);
}

void CodeFragment::declareFrame(FrameId frameId, int stackOffset) {
    FrameInfo& frame = getOrDeclareFrame(frameId);
    tassert(7239101,
            str::stream() << "Frame stackPosition is already defined. frameId: " << frameId,
            frame.stackPosition == FrameInfo::kPositionNotSet);
    frame.stackPosition = _stackSize + stackOffset;
    if (!frame.fixupOffsets.empty()) {
        fixupFrame(frame);
    }
}

void CodeFragment::removeFrame(FrameId frameId) {
    auto p = _frames.find(frameId);
    if (p == _frames.end()) {
        return;
    }

    tassert(7239103,
            str::stream() << "Can't remove frame that has outstanding fixups. frameId:" << frameId,
            p->second.fixupOffsets.empty());

    _frames.erase(frameId);
}

bool CodeFragment::hasFrames() const {
    return !_frames.empty();
}

CodeFragment::FrameInfo& CodeFragment::getOrDeclareFrame(FrameId frameId) {
    auto [it, r] = _frames.try_emplace(frameId);
    return it->second;
}

void CodeFragment::fixupFrame(FrameInfo& frame) {
    tassert(7239105,
            "Frame must have defined stackPosition",
            frame.stackPosition != FrameInfo::kPositionNotSet);

    for (auto fixupOffset : frame.fixupOffsets) {
        int stackOffset = readFromMemory<int>(_instrs.data() + fixupOffset);
        writeToMemory(_instrs.data() + fixupOffset,
                      stackOffset - static_cast<int>(frame.stackPosition));
    }

    frame.fixupOffsets.clear();
}

void CodeFragment::fixupStackOffsets(int stackOffsetDelta) {
    if (stackOffsetDelta == 0) {
        return;
    }

    for (auto& p : _frames) {
        auto& frame = p.second;
        if (frame.stackPosition != FrameInfo::kPositionNotSet) {
            frame.stackPosition = frame.stackPosition + stackOffsetDelta;
        }

        for (auto& fixupOffset : frame.fixupOffsets) {
            int stackOffset = readFromMemory<int>(_instrs.data() + fixupOffset);
            writeToMemory<int>(_instrs.data() + fixupOffset, stackOffset + stackOffsetDelta);
        }
    }
}

void CodeFragment::removeLabel(LabelId labelId) {
    auto p = _labels.find(labelId);
    if (p == _labels.end()) {
        return;
    }

    tassert(7134601,
            str::stream() << "Can't remove label that has outstanding fixups. labelId:" << labelId,
            p->second.fixupOffsets.empty());

    _labels.erase(labelId);
}

void CodeFragment::appendLabel(LabelId labelId) {
    auto& label = getOrDeclareLabel(labelId);
    tassert(7134602,
            str::stream() << "Label definitionOffset is already defined. labelId: " << labelId,
            label.definitionOffset == LabelInfo::kOffsetNotSet);
    label.definitionOffset = _instrs.size();
    if (!label.fixupOffsets.empty()) {
        fixupLabel(label);
    }
}

void CodeFragment::fixupLabel(LabelInfo& label) {
    tassert(7134603,
            "Label must have defined definitionOffset",
            label.definitionOffset != LabelInfo::kOffsetNotSet);

    for (auto fixupOffset : label.fixupOffsets) {
        int jumpOffset = readFromMemory<int>(_instrs.data() + fixupOffset);
        writeToMemory(_instrs.data() + fixupOffset,
                      jumpOffset + static_cast<int>(label.definitionOffset - fixupOffset));
    }

    label.fixupOffsets.clear();
}

CodeFragment::LabelInfo& CodeFragment::getOrDeclareLabel(LabelId labelId) {
    auto [it, r] = _labels.try_emplace(labelId);
    return it->second;
}

void CodeFragment::validate() {
    if constexpr (kDebugBuild) {
        for (auto& p : _frames) {
            auto& frame = p.second;
            tassert(7134606,
                    str::stream() << "Unresolved frame fixup offsets. frameId: " << p.first,
                    frame.fixupOffsets.empty());
        }

        for (auto& p : _labels) {
            auto& label = p.second;
            tassert(7134607,
                    str::stream() << "Unresolved label fixup offsets. labelId: " << p.first,
                    label.fixupOffsets.empty());
        }
    }
}

void CodeFragment::copyCodeAndFixup(CodeFragment&& from) {
    auto instrsSize = _instrs.size();

    if (_instrs.empty()) {
        _instrs = std::move(from._instrs);
    } else {
        _instrs.insert(_instrs.end(), from._instrs.begin(), from._instrs.end());
    }

    for (auto& p : from._frames) {
        auto& fromFrame = p.second;
        for (auto& fixupOffset : fromFrame.fixupOffsets) {
            fixupOffset += instrsSize;
        }
        auto it = _frames.find(p.first);
        if (it != _frames.end()) {
            auto& frame = it->second;
            if (fromFrame.stackPosition != FrameInfo::kPositionNotSet) {
                tassert(7239104,
                        "Duplicate frame stackPosition",
                        frame.stackPosition == FrameInfo::kPositionNotSet);
                frame.stackPosition = fromFrame.stackPosition;
            }
            frame.fixupOffsets.insert(frame.fixupOffsets.end(),
                                      fromFrame.fixupOffsets.begin(),
                                      fromFrame.fixupOffsets.end());
            if (frame.stackPosition != FrameInfo::kPositionNotSet) {
                fixupFrame(frame);
            }
        } else {
            _frames.emplace(p.first, std::move(fromFrame));
        }
    }

    for (auto& p : from._labels) {
        auto& fromLabel = p.second;
        if (fromLabel.definitionOffset != LabelInfo::kOffsetNotSet) {
            fromLabel.definitionOffset += instrsSize;
        }
        for (auto& fixupOffset : fromLabel.fixupOffsets) {
            fixupOffset += instrsSize;
        }
        auto it = _labels.find(p.first);
        if (it != _labels.end()) {
            auto& label = it->second;
            if (fromLabel.definitionOffset != LabelInfo::kOffsetNotSet) {
                tassert(7134605,
                        "Duplicate label definitionOffset",
                        label.definitionOffset == LabelInfo::kOffsetNotSet);
                label.definitionOffset = fromLabel.definitionOffset;
            }
            label.fixupOffsets.insert(label.fixupOffsets.end(),
                                      fromLabel.fixupOffsets.begin(),
                                      fromLabel.fixupOffsets.end());
            if (label.definitionOffset != LabelInfo::kOffsetNotSet) {
                fixupLabel(label);
            }
        } else {
            _labels.emplace(p.first, std::move(fromLabel));
        }
    }
}

template <typename... Ts>
size_t CodeFragment::appendParameters(uint8_t* ptr, Ts&&... params) {
    int popCompensation = 0;
    ((popCompensation += params.frameId ? 0 : -1), ...);

    size_t size = 0;
    ((size += appendParameter(ptr + size, params, popCompensation)), ...);
    return size;
}

size_t CodeFragment::appendParameter(uint8_t* ptr,
                                     Instruction::Parameter param,
                                     int& popCompensation) {
    // 'pop' means that the location we're reading from is a temporary value on the VM stack
    // (i.e. not a local variable) and that it needs to be popped off the stack immediately
    // after we read it.
    bool pop = !param.frameId;

    // 'moveFrom' means that the location we're reading from is eligible to be the right hand
    // side of a "move assignment" (i.e. it's an "rvalue reference"). If 'pop' is true, then
    // 'moveFrom' must always be true as well.
    bool moveFrom = pop || param.moveFrom;

    // If the parameter is not coming from a frame then we have to pop it off the stack once the
    // instruction is done.
    uint8_t flags = static_cast<uint8_t>(pop) | (static_cast<uint8_t>(moveFrom) << 1);

    ptr += writeToMemory(ptr, flags);

    if (param.frameId) {
        auto& frame = getOrDeclareFrame(*param.frameId);

        // Compute the absolute variable stack offset based on the current stack depth and pop
        // compensation.
        int stackOffset = varToOffset(param.variable) + popCompensation + _stackSize;

        // If frame has stackPositiion defined, then compute the final relative stack offset.
        // Otherwise, register a fixup to compute the relative stack offset later.
        if (frame.stackPosition != FrameInfo::kPositionNotSet) {
            stackOffset -= frame.stackPosition;
        } else {
            size_t fixUpOffset = ptr - _instrs.data();
            frame.fixupOffsets.push_back(fixUpOffset);
        }

        ptr += writeToMemory(ptr, stackOffset);
    } else {
        ++popCompensation;
    }


    return param.size();
}

void CodeFragment::append(CodeFragment&& code) {
    // Fixup all stack offsets before copying.
    code.fixupStackOffsets(_stackSize);

    _maxStackSize = std::max(_maxStackSize, _stackSize + code._maxStackSize);
    _stackSize += code._stackSize;

    copyCodeAndFixup(std::move(code));
}

void CodeFragment::appendNoStack(CodeFragment&& code) {
    copyCodeAndFixup(std::move(code));
}

void CodeFragment::append(std::vector<CodeFragment>&& fragments) {
    if (fragments.empty()) {
        return;
    }
    for (const auto& fragment : fragments) {
        tassert(10130701,
                "Exclusive code fragments must have the same stack size",
                fragment.stackSize() == fragments[0].stackSize());
    }

    // Fixup all stack offsets before copying.
    for (auto& fragment : fragments) {
        fragment.fixupStackOffsets(_stackSize);
        _maxStackSize = std::max(_maxStackSize, _stackSize + fragment._maxStackSize);
    }

    _stackSize += fragments[0]._stackSize;

    for (auto&& fragment : fragments) {
        copyCodeAndFixup(std::move(fragment));
    }
}

void CodeFragment::appendConstVal(value::TypeTags tag, value::Value val) {
    Instruction i;
    i.tag = Instruction::pushConstVal;

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(tag) + sizeof(val));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, tag);
    offset += writeToMemory(offset, val);

    adjustStackSimple(i);
}

void CodeFragment::appendAccessVal(value::SlotAccessor* accessor) {
    Instruction i;
    i.tag = [](value::SlotAccessor* accessor) {
        if (accessor->is<value::OwnedValueAccessor>()) {
            return Instruction::pushOwnedAccessorVal;
        } else if (accessor->is<RuntimeEnvironment::Accessor>()) {
            return Instruction::pushEnvAccessorVal;
        }

        return Instruction::pushAccessVal;
    }(accessor);
    auto offset = allocateSpace(sizeof(Instruction) + sizeof(accessor));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, accessor);

    adjustStackSimple(i);
}

void CodeFragment::appendMoveVal(value::SlotAccessor* accessor) {
    Instruction i;
    i.tag = Instruction::pushMoveVal;

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(accessor));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, accessor);

    adjustStackSimple(i);
}

void CodeFragment::appendLocalVal(FrameId frameId, int variable, bool moveFrom) {
    Instruction i;
    i.tag = moveFrom ? Instruction::pushMoveLocalVal : Instruction::pushLocalVal;

    auto& frame = getOrDeclareFrame(frameId);

    // Compute the absolute variable stack offset based on the current stack depth
    int stackOffset = varToOffset(variable) + _stackSize;

    // If frame has stackPositiion defined, then compute the final relative stack offset.
    // Otherwise, register a fixup to compute the relative stack offset later.
    if (frame.stackPosition != FrameInfo::kPositionNotSet) {
        stackOffset -= frame.stackPosition;
    } else {
        auto fixUpOffset = _instrs.size() + sizeof(Instruction);
        frame.fixupOffsets.push_back(fixUpOffset);
    }

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(stackOffset));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, stackOffset);

    adjustStackSimple(i);
}

void CodeFragment::appendLocalLambda(int codePosition) {
    Instruction i;
    i.tag = Instruction::pushLocalLambda;

    auto size = sizeof(Instruction) + sizeof(codePosition);
    auto offset = allocateSpace(size);

    int codeOffset = codePosition - _instrs.size();

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, codeOffset);

    adjustStackSimple(i);
}

void CodeFragment::appendPop() {
    appendSimpleInstruction(Instruction::pop);
}

void CodeFragment::appendSwap() {
    appendSimpleInstruction(Instruction::swap);
}

void CodeFragment::appendMakeOwn(Instruction::Parameter arg) {
    appendSimpleInstruction(Instruction::makeOwn, arg);
}

void CodeFragment::appendCmp3w(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::cmp3w, lhs, rhs);
}

void CodeFragment::appendAdd(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::add, lhs, rhs);
}

void CodeFragment::appendNumericConvert(value::TypeTags targetTag) {
    Instruction i;
    i.tag = Instruction::numConvert;

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(targetTag));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, targetTag);
    adjustStackSimple(i);
}

void CodeFragment::appendSub(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::sub, lhs, rhs);
}

void CodeFragment::appendMul(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::mul, lhs, rhs);
}

void CodeFragment::appendDiv(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::div, lhs, rhs);
}

void CodeFragment::appendIDiv(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::idiv, lhs, rhs);
}

void CodeFragment::appendMod(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::mod, lhs, rhs);
}

void CodeFragment::appendNegate(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::negate, input);
}

void CodeFragment::appendNot(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::logicNot, input);
}

void CodeFragment::appendLess(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::less, lhs, rhs);
}

void CodeFragment::appendLessEq(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::lessEq, lhs, rhs);
}

void CodeFragment::appendGreater(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::greater, lhs, rhs);
}

void CodeFragment::appendGreaterEq(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::greaterEq, lhs, rhs);
}

void CodeFragment::appendEq(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::eq, lhs, rhs);
}

void CodeFragment::appendNeq(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::neq, lhs, rhs);
}

template <typename... Ts>
void CodeFragment::appendSimpleInstruction(Instruction::Tags tag, Ts&&... params) {
    Instruction i;
    i.tag = tag;

    // For every parameter that is popped (i.e. not coming from a frame) we have to compensate frame
    // offsets.
    size_t paramSize = 0;

    ((paramSize += params.size()), ...);

    auto offset = allocateSpace(sizeof(Instruction) + paramSize);

    offset += writeToMemory(offset, i);
    offset += appendParameters(offset, params...);

    adjustStackSimple(i, params...);
}

void CodeFragment::appendCollLess(Instruction::Parameter lhs,
                                  Instruction::Parameter rhs,
                                  Instruction::Parameter collator) {
    appendSimpleInstruction(Instruction::collLess, collator, lhs, rhs);
}

void CodeFragment::appendCollLessEq(Instruction::Parameter lhs,
                                    Instruction::Parameter rhs,
                                    Instruction::Parameter collator) {
    appendSimpleInstruction(Instruction::collLessEq, collator, lhs, rhs);
}

void CodeFragment::appendCollGreater(Instruction::Parameter lhs,
                                     Instruction::Parameter rhs,
                                     Instruction::Parameter collator) {
    appendSimpleInstruction(Instruction::collGreater, collator, lhs, rhs);
}

void CodeFragment::appendCollGreaterEq(Instruction::Parameter lhs,
                                       Instruction::Parameter rhs,
                                       Instruction::Parameter collator) {
    appendSimpleInstruction(Instruction::collGreaterEq, collator, lhs, rhs);
}

void CodeFragment::appendCollEq(Instruction::Parameter lhs,
                                Instruction::Parameter rhs,
                                Instruction::Parameter collator) {
    appendSimpleInstruction(Instruction::collEq, collator, lhs, rhs);
}

void CodeFragment::appendCollNeq(Instruction::Parameter lhs,
                                 Instruction::Parameter rhs,
                                 Instruction::Parameter collator) {
    appendSimpleInstruction(Instruction::collNeq, collator, lhs, rhs);
}

void CodeFragment::appendCollCmp3w(Instruction::Parameter lhs,
                                   Instruction::Parameter rhs,
                                   Instruction::Parameter collator) {
    appendSimpleInstruction(Instruction::collCmp3w, collator, lhs, rhs);
}

void CodeFragment::appendFillEmpty() {
    appendSimpleInstruction(Instruction::fillEmpty);
}

void CodeFragment::appendFillEmpty(Instruction::Constants k) {
    Instruction i;
    i.tag = Instruction::fillEmptyImm;

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(k));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, k);

    adjustStackSimple(i);
}

void CodeFragment::appendGetField(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::getField, lhs, rhs);
}

void CodeFragment::appendGetField(Instruction::Parameter input, StringData fieldName) {
    auto size = fieldName.size();
    invariant(size < Instruction::kMaxInlineStringSize);

    Instruction i;
    i.tag = Instruction::getFieldImm;

    auto offset = allocateSpace(sizeof(Instruction) + input.size() + sizeof(uint8_t) + size);

    offset += writeToMemory(offset, i);
    offset += appendParameters(offset, input);
    offset += writeToMemory(offset, static_cast<uint8_t>(size));
    for (auto ch : fieldName) {
        offset += writeToMemory(offset, ch);
    }

    adjustStackSimple(i, input);
}

void CodeFragment::appendGetElement(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::getElement, lhs, rhs);
}

void CodeFragment::appendCollComparisonKey(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::collComparisonKey, lhs, rhs);
}

void CodeFragment::appendGetFieldOrElement(Instruction::Parameter lhs, Instruction::Parameter rhs) {
    appendSimpleInstruction(Instruction::getFieldOrElement, lhs, rhs);
}

void CodeFragment::appendGetArraySize(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::getArraySize, input);
}

void CodeFragment::appendSetField() {
    appendSimpleInstruction(Instruction::setField);
}

void CodeFragment::appendSum() {
    appendSimpleInstruction(Instruction::aggSum);
}

void CodeFragment::appendCount() {
    appendSimpleInstruction(Instruction::aggCount);
}

void CodeFragment::appendMin() {
    appendSimpleInstruction(Instruction::aggMin);
}

void CodeFragment::appendMax() {
    appendSimpleInstruction(Instruction::aggMax);
}

void CodeFragment::appendFirst() {
    appendSimpleInstruction(Instruction::aggFirst);
}

void CodeFragment::appendLast() {
    appendSimpleInstruction(Instruction::aggLast);
}

void CodeFragment::appendCollMin() {
    appendSimpleInstruction(Instruction::aggCollMin);
}

void CodeFragment::appendCollMax() {
    appendSimpleInstruction(Instruction::aggCollMax);
}

void CodeFragment::appendExists(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::exists, input);
}

void CodeFragment::appendIsNull(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isNull, input);
}

void CodeFragment::appendIsObject(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isObject, input);
}

void CodeFragment::appendIsArray(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isArray, input);
}

void CodeFragment::appendIsInList(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isInList, input);
}

void CodeFragment::appendIsString(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isString, input);
}

void CodeFragment::appendIsNumber(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isNumber, input);
}

void CodeFragment::appendIsBinData(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isBinData, input);
}

void CodeFragment::appendIsDate(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isDate, input);
}

void CodeFragment::appendIsNaN(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isNaN, input);
}

void CodeFragment::appendIsInfinity(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isInfinity, input);
}

void CodeFragment::appendIsRecordId(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isRecordId, input);
}

void CodeFragment::appendIsMinKey(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isMinKey, input);
}

void CodeFragment::appendIsMaxKey(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isMaxKey, input);
}

void CodeFragment::appendIsTimestamp(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isTimestamp, input);
}

void CodeFragment::appendIsKeyString(Instruction::Parameter input) {
    appendSimpleInstruction(Instruction::isKeyString, input);
}

void CodeFragment::appendTraverseP() {
    appendSimpleInstruction(Instruction::traverseP);
}

void CodeFragment::appendTraverseP(int codePosition, Instruction::Constants k) {
    Instruction i;
    i.tag = Instruction::traversePImm;

    auto size = sizeof(Instruction) + sizeof(codePosition) + sizeof(k);
    auto offset = allocateSpace(size);

    int codeOffset = codePosition - _instrs.size();

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, k);
    offset += writeToMemory(offset, codeOffset);

    adjustStackSimple(i);
}

void CodeFragment::appendMagicTraverseF() {
    appendSimpleInstruction(Instruction::magicTraverseF);
}
void CodeFragment::appendTraverseF() {
    appendSimpleInstruction(Instruction::traverseF);
}

void CodeFragment::appendTraverseF(int codePosition, Instruction::Constants k) {
    Instruction i;
    i.tag = Instruction::traverseFImm;

    auto size = sizeof(Instruction) + sizeof(codePosition) + sizeof(k);
    auto offset = allocateSpace(size);

    int codeOffset = codePosition - _instrs.size();

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, k);
    offset += writeToMemory(offset, codeOffset);

    adjustStackSimple(i);
}


void CodeFragment::appendTypeMatch(Instruction::Parameter input, uint32_t mask) {
    Instruction i;
    i.tag = Instruction::typeMatchImm;

    auto size = sizeof(Instruction) + input.size() + sizeof(mask);
    auto offset = allocateSpace(size);

    offset += writeToMemory(offset, i);
    offset += appendParameters(offset, input);
    offset += writeToMemory(offset, mask);

    adjustStackSimple(i, input);
}

void CodeFragment::appendDateTrunc(TimeUnit unit,
                                   int64_t binSize,
                                   TimeZone timezone,
                                   DayOfWeek startOfWeek) {
    Instruction i;
    i.tag = Instruction::dateTruncImm;

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(unit) + sizeof(binSize) +
                                sizeof(timezone) + sizeof(startOfWeek));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, unit);
    offset += writeToMemory(offset, binSize);
    offset += writeToMemory(offset, timezone);
    offset += writeToMemory(offset, startOfWeek);

    adjustStackSimple(i);
}

void CodeFragment::appendValueBlockApplyLambda() {
    appendSimpleInstruction(Instruction::valueBlockApplyLambda);
}

void CodeFragment::appendFunction(Builtin f, ArityType arity) {
    Instruction i;
    const bool isSmallArity = (arity <= std::numeric_limits<SmallArityType>::max());
    const bool isSmallBuiltin =
        (f <= static_cast<Builtin>(std::numeric_limits<SmallBuiltinType>::max()));
    const bool isSmallFunction = isSmallArity && isSmallBuiltin;
    i.tag = isSmallFunction ? Instruction::functionSmall : Instruction::function;

    _maxStackSize = std::max(_maxStackSize, _stackSize + 1);
    // Account for consumed arguments
    _stackSize -= arity;
    // and the return value.
    _stackSize += 1;

    auto offset = allocateSpace(sizeof(Instruction) +
                                (isSmallFunction ? sizeof(SmallBuiltinType) : sizeof(Builtin)) +
                                (isSmallFunction ? sizeof(SmallArityType) : sizeof(ArityType)));

    offset += writeToMemory(offset, i);
    if (isSmallFunction) {
        SmallBuiltinType smallBuiltin = static_cast<SmallBuiltinType>(f);
        offset += writeToMemory(offset, smallBuiltin);
    } else {
        offset += writeToMemory(offset, f);
    }
    offset += isSmallFunction ? writeToMemory(offset, static_cast<SmallArityType>(arity))
                              : writeToMemory(offset, arity);
}

void CodeFragment::appendLabelJump(LabelId labelId) {
    appendLabelJumpInstruction(labelId, Instruction::jmp);
}

void CodeFragment::appendLabelJumpTrue(LabelId labelId) {
    appendLabelJumpInstruction(labelId, Instruction::jmpTrue);
}

void CodeFragment::appendLabelJumpFalse(LabelId labelId) {
    appendLabelJumpInstruction(labelId, Instruction::jmpFalse);
}

void CodeFragment::appendLabelJumpNothing(LabelId labelId) {
    appendLabelJumpInstruction(labelId, Instruction::jmpNothing);
}

void CodeFragment::appendLabelJumpNotNothing(LabelId labelId) {
    appendLabelJumpInstruction(labelId, Instruction::jmpNotNothing);
}

void CodeFragment::appendLabelJumpInstruction(LabelId labelId, Instruction::Tags tag) {
    auto& label = getOrDeclareLabel(labelId);

    Instruction i;
    i.tag = tag;

    int jumpOffset;
    auto offset = allocateSpace(sizeof(Instruction) + sizeof(jumpOffset));

    if (label.definitionOffset != LabelInfo::kOffsetNotSet) {
        jumpOffset = label.definitionOffset - _instrs.size();
    } else {
        // Fixup will compute the relative jump as if it was done from the fixup offset itself,
        // so initialize jumpOffset with the difference between jump offset and the end of
        // instruction.
        jumpOffset = -static_cast<int>(sizeof(jumpOffset));
        label.fixupOffsets.push_back(offset + sizeof(Instruction) - _instrs.data());
    }

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, jumpOffset);

    adjustStackSimple(i);
}

void CodeFragment::appendRet() {
    appendSimpleInstruction(Instruction::ret);
}

void CodeFragment::appendAllocStack(uint32_t size) {
    Instruction i;
    i.tag = Instruction::allocStack;

    auto offset = allocateSpace(sizeof(Instruction) + sizeof(size));

    offset += writeToMemory(offset, i);
    offset += writeToMemory(offset, size);

    adjustStackSimple(i);
}

void CodeFragment::appendFail() {
    appendSimpleInstruction(Instruction::fail);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
