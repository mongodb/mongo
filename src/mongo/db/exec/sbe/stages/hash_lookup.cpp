/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/hash_lookup.h"

#include <set>

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/curop.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/stage_visitors.h"

namespace mongo::sbe {

HashLookupStage::HashLookupStage(std::unique_ptr<PlanStage> outer,
                                 std::unique_ptr<PlanStage> inner,
                                 value::SlotId outerKeySlot,
                                 value::SlotId innerKeySlot,
                                 value::SlotId innerProjectSlot,
                                 SlotExprPair innerAgg,
                                 boost::optional<value::SlotId> collatorSlot,
                                 PlanNodeId planNodeId,
                                 bool participateInTrialRunTracking)
    : PlanStage("hash_lookup"_sd, planNodeId, participateInTrialRunTracking),
      _outerKeySlot(outerKeySlot),
      _innerKeySlot(innerKeySlot),
      _innerProjectSlot(innerProjectSlot),
      _innerAgg(std::move(innerAgg)),
      _lookupStageOutputSlot(_innerAgg.first),
      _collatorSlot(collatorSlot) {
    _children.emplace_back(std::move(outer));
    _children.emplace_back(std::move(inner));
}

std::unique_ptr<PlanStage> HashLookupStage::clone() const {
    auto& [slotId, expr] = _innerAgg;
    SlotExprPair innerAgg{slotId, expr->clone()};

    return std::make_unique<HashLookupStage>(outerChild()->clone(),
                                             innerChild()->clone(),
                                             _outerKeySlot,
                                             _innerKeySlot,
                                             _innerProjectSlot,
                                             std::move(innerAgg),
                                             _collatorSlot,
                                             _commonStats.nodeId,
                                             _participateInTrialRunTracking);
}

void HashLookupStage::prepare(CompileCtx& ctx) {
    outerChild()->prepare(ctx);
    innerChild()->prepare(ctx);

    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(6367801,
                "collator accessor should exist if collator slot provided to HashJoinStage",
                _collatorAccessor != nullptr);
        auto [collatorTag, collatorVal] = _collatorAccessor->getViewOfValue();
        tassert(6367805,
                "collatorSlot must be of collator type",
                collatorTag == value::TypeTags::collator);
        // Stash the collator because we need it when spilling strings to the record store.
        _hashTable.setCollator(value::getCollatorView(collatorVal));
    }

    value::SlotSet inputSlots;

    value::SlotId slot = _outerKeySlot;
    inputSlots.emplace(slot);
    _inOuterMatchAccessor = outerChild()->getAccessor(ctx, slot);

    slot = _innerKeySlot;
    inputSlots.emplace(slot);
    _inInnerMatchAccessor = innerChild()->getAccessor(ctx, slot);

    // Accessor for '_innerProjectSlot' only when outside the VM.
    slot = _innerProjectSlot;
    inputSlots.emplace(slot);
    _inInnerProjectAccessor = innerChild()->getAccessor(ctx, slot);

    // Set '_compileInnerAgg' to make getAccessor() return '_outInnerProjectAccessor' as the
    // accessor for the SlotId '_innerProjectSlot' (called 'foreignRecordSlot' in the stage builder)
    // while compiling the EExpr, as the compiler binds that SlotId to that accessor inside the VM.
    _compileInnerAgg = true;
    ctx.root = this;
    ctx.aggExpression = true;
    ctx.accumulator = &_lookupStageOutputAccessor;  // VM output slot
    _aggCode = _innerAgg.second->compile(ctx);
    ctx.aggExpression = false;
    _compileInnerAgg = false;

    if (inputSlots.contains(_lookupStageOutputSlot)) {
        // 'errMsg' works around tasserted() macro's problem referencing '_lookupStageOutputSlot'.
        std::string errMsg = str::stream()
            << "conflicting input and result field: " << _lookupStageOutputSlot;
        tasserted(6367804, errMsg);
    }

    _outInnerProject.resize(1);
    _lookupStageOutput.resize(1);
}  // HashLookupStage::prepare

value::SlotAccessor* HashLookupStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compileInnerAgg) {
        if (slot == _innerProjectSlot) {
            return &_outInnerProjectAccessor;
        }
        return ctx.getAccessor(slot);
    } else {
        if (slot == _lookupStageOutputSlot) {
            return &_lookupStageOutputAccessor;
        }
        return outerChild()->getAccessor(ctx, slot);
    }
}

void HashLookupStage::doAttachToOperationContext(OperationContext* opCtx) {
    _hashTable.doAttachToOperationContext(_opCtx);
}

void HashLookupStage::doDetachFromOperationContext() {
    _hashTable.doDetachFromOperationContext();
}

void HashLookupStage::doSaveState(bool relinquishCursor) {
    _hashTable.doSaveState(relinquishCursor);
}

void HashLookupStage::doRestoreState(bool relinquishCursor) {
    _hashTable.doRestoreState(relinquishCursor);
}

void HashLookupStage::reset(bool fromClose) {
    // Also resets the memory threshold if the knob changes between re-open calls.
    _hashTable.reset(fromClose);
}

void HashLookupStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    if (reOpen) {
        reset(false /* fromClose */);
    }

    _commonStats.opens++;
    _hashTable.open();

    // Insert the inner side into the hash table.
    innerChild()->open(false);
    while (innerChild()->getNext() == PlanState::ADVANCED) {
        value::MaterializedRow value{1};

        // Copy the projected value.
        auto [tag, val] = _inInnerProjectAccessor->getCopyOfValue();
        value.reset(0, true, tag, val);

        // This where we put the value in here. This can grow need to spill.
        size_t bufferIndex = _hashTable.bufferValueOrSpill(value);

        auto [tagKeyView, valKeyView] = _inInnerMatchAccessor->getViewOfValue();
        if (value::isArray(tagKeyView)) {
            value::ArrayAccessor arrayAccessor;
            arrayAccessor.reset(_inInnerMatchAccessor);

            while (!arrayAccessor.atEnd()) {
                _hashTable.addHashTableEntry(&arrayAccessor, bufferIndex);
                arrayAccessor.advance();
            }
        } else {
            _hashTable.addHashTableEntry(_inInnerMatchAccessor, bufferIndex);
        }
    }
    innerChild()->close();
    outerChild()->open(reOpen);
}  // HashLookupStage::open

template <typename Container>
void HashLookupStage::accumulateFromValueIndices(const Container* bufferIndices) {
    for (const size_t bufferIdx : *bufferIndices) {
        boost::optional<std::pair<value::TypeTags, value::Value>> innerMatch =
            _hashTable.getValueAtIndex(bufferIdx);
        _outInnerProjectAccessor.reset(false /* owned */, innerMatch->first, innerMatch->second);

        // Run the VM code to "accumulate" the current inner doc into the lookup output array.
        auto [owned, tag, val] = _bytecode.run(_aggCode.get());
        _lookupStageOutput.reset(0 /* column */, owned, tag, val);
    }
}  // HashLookupStage::accumulateFromValueIndices

PlanState HashLookupStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    PlanState state = outerChild()->getNext();
    if (state == PlanState::ADVANCED) {
        // We just got this outer doc, so reset the $lookup "as" result array accumulator to nothing
        // and the hash table iterator to the outer key.
        _lookupStageOutput.reset(0, false, value::TypeTags::Nothing, 0);
        auto [outerKeyTag, outerKeyVal] = _inOuterMatchAccessor->getViewOfValue();
        _hashTable.htIter.reset(outerKeyTag, outerKeyVal);

        // Accumulate all the matching inner docs for the outer key(s).
        accumulateFromValueIndicesVariant(_hashTable.htIter.getAllMatchingIndices());
    }
    return trackPlanState(state);
}  // HashLookupStage::getNext

void HashLookupStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();
    outerChild()->close();
    reset(true /* fromClose */);
}

std::unique_ptr<PlanStageStats> HashLookupStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    invariant(ret);
    ret->children.emplace_back(outerChild()->getStats(includeDebugInfo));
    ret->children.emplace_back(innerChild()->getStats(includeDebugInfo));

    const HashLookupStats* specificStats = static_cast<const HashLookupStats*>(getSpecificStats());
    ret->specific = std::make_unique<HashLookupStats>(*specificStats);
    if (includeDebugInfo) {
        BSONObjBuilder bob(StorageAccessStatsVisitor::collectStats(*this, *ret).toBSON());
        // Spilling stats.
        bob.appendBool("usedDisk", specificStats->usedDisk)
            .appendNumber("spilledRecords", specificStats->getSpilledRecords())
            .appendNumber("spilledBytesApprox", specificStats->getSpilledBytesApprox());
        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* HashLookupStage::getSpecificStats() const {
    return _hashTable.getHashLookupStats();
}

std::vector<DebugPrinter::Block> HashLookupStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("[`"));
    auto& [slot, expr] = _innerAgg;
    DebugPrinter::addIdentifier(ret, slot);
    ret.emplace_back("=");
    DebugPrinter::addBlocks(ret, expr->debugPrint());
    ret.emplace_back("`]");

    if (_collatorSlot) {
        DebugPrinter::addIdentifier(ret, *_collatorSlot);
    }

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    DebugPrinter::addKeyword(ret, "outer");
    DebugPrinter::addIdentifier(ret, _outerKeySlot);
    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, outerChild()->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    DebugPrinter::addKeyword(ret, "inner");
    DebugPrinter::addIdentifier(ret, _innerKeySlot);
    DebugPrinter::addIdentifier(ret, _innerProjectSlot);

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, innerChild()->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    return ret;
}  // HashLookupStage::debugPrint

size_t HashLookupStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_innerProjectSlot);
    size += size_estimator::estimate(_innerAgg);
    return size;
}
}  // namespace mongo::sbe
