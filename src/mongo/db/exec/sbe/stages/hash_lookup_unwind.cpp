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

#include "mongo/db/exec/sbe/stages/hash_lookup_unwind.h"

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

HashLookupUnwindStage::HashLookupUnwindStage(std::unique_ptr<PlanStage> outer,
                                             std::unique_ptr<PlanStage> inner,
                                             value::SlotId outerKeySlot,
                                             value::SlotId innerKeySlot,
                                             value::SlotId innerProjectSlot,
                                             value::SlotId lookupStageOutputSlot,
                                             boost::optional<value::SlotId> collatorSlot,
                                             PlanNodeId planNodeId,
                                             bool participateInTrialRunTracking)
    : PlanStage("hash_lookup_unwind"_sd, planNodeId, participateInTrialRunTracking),
      _outerKeySlot(outerKeySlot),
      _innerKeySlot(innerKeySlot),
      _innerProjectSlot(innerProjectSlot),
      _lookupStageOutputSlot(lookupStageOutputSlot),
      _collatorSlot(collatorSlot) {
    _children.emplace_back(std::move(outer));
    _children.emplace_back(std::move(inner));
}

std::unique_ptr<PlanStage> HashLookupUnwindStage::clone() const {
    return std::make_unique<HashLookupUnwindStage>(outerChild()->clone(),
                                                   innerChild()->clone(),
                                                   _outerKeySlot,
                                                   _innerKeySlot,
                                                   _innerProjectSlot,
                                                   _lookupStageOutputSlot,
                                                   _collatorSlot,
                                                   _commonStats.nodeId,
                                                   _participateInTrialRunTracking);
}

void HashLookupUnwindStage::prepare(CompileCtx& ctx) {
    outerChild()->prepare(ctx);
    innerChild()->prepare(ctx);

    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(8229802,
                "collator accessor should exist if collator slot provided to HashJoinStage",
                _collatorAccessor != nullptr);
        auto [collatorTag, collatorVal] = _collatorAccessor->getViewOfValue();
        tassert(8229803,
                "collatorSlot must be of collator type",
                collatorTag == value::TypeTags::collator);
        // Stash the collator because we need it when spilling strings to the record store.
        _hashTable.setCollator(value::getCollatorView(collatorVal));
    }

    // Set of all input slot IDs, used only to check that '_lookupStageOutputSlot' does not collide.
    value::SlotSet inputSlots;

    value::SlotId slot = _outerKeySlot;
    inputSlots.emplace(slot);
    _inOuterMatchAccessor = outerChild()->getAccessor(ctx, slot);

    slot = _innerKeySlot;
    inputSlots.emplace(slot);
    _inInnerMatchAccessor = innerChild()->getAccessor(ctx, slot);

    slot = _innerProjectSlot;
    inputSlots.emplace(slot);
    _inInnerProjectAccessor = innerChild()->getAccessor(ctx, slot);

    if (inputSlots.contains(_lookupStageOutputSlot)) {
        tasserted(8229806,
                  str::stream() << "conflicting input and result field: "
                                << _lookupStageOutputSlot);
    }

    _lookupStageOutput.resize(1);
}  // HashLookupUnwindStage::prepare

value::SlotAccessor* HashLookupUnwindStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (slot == _lookupStageOutputSlot) {
        return &_lookupStageOutputAccessor;
    }
    return outerChild()->getAccessor(ctx, slot);
}

void HashLookupUnwindStage::doAttachToOperationContext(OperationContext* opCtx) {
    _hashTable.doAttachToOperationContext(_opCtx);
}

void HashLookupUnwindStage::doDetachFromOperationContext() {
    _hashTable.doDetachFromOperationContext();
}

void HashLookupUnwindStage::doSaveState(bool relinquishCursor) {
    _hashTable.doSaveState(relinquishCursor);
}

void HashLookupUnwindStage::doRestoreState(bool relinquishCursor) {
    _hashTable.doRestoreState(relinquishCursor);
}

void HashLookupUnwindStage::reset(bool fromClose) {
    _outerKeyOpen = false;
    // Also resets the memory threshold if the knob changes between re-open calls.
    _hashTable.reset(fromClose);
}

void HashLookupUnwindStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    if (reOpen) {
        reset(false /* fromClose */);
    }
    _commonStats.opens++;
    _hashTable.open();

    // Insert the inner side into the hash table.
    innerChild()->open(false);
    while (innerChild()->getNext() == PlanState::ADVANCED) {
        value::MaterializedRow value{1 /* columns */};

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
}  // HashLookupUnwindStage::open

PlanState HashLookupUnwindStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    while (true) {
        if (!_outerKeyOpen) {
            if (outerChild()->getNext() != PlanState::ADVANCED) {
                return trackPlanState(PlanState::IS_EOF);
            }
            _outerKeyOpen = true;
            // We just got this outer doc, so reset the iterator to the outer key.
            auto [outerKeyTag, outerKeyVal] = _inOuterMatchAccessor->getViewOfValue();
            _hashTable.htIter.reset(outerKeyTag, outerKeyVal);
        }

        size_t matchIndex = _hashTable.htIter.getNextMatchingIndex();
        if (matchIndex != LookupHashTableIter::kNoMatchingIndex) {
            boost::optional<std::pair<value::TypeTags, value::Value>> innerMatch =
                _hashTable.getValueAtIndex(matchIndex);
            if (innerMatch) {
                _lookupStageOutputAccessor.reset(
                    false /* owned */, innerMatch->first, innerMatch->second);
                return trackPlanState(PlanState::ADVANCED);
            } else {
                _outerKeyOpen = false;
            }
        } else {
            _outerKeyOpen = false;
        }
    }  // while true
    return trackPlanState(PlanState::IS_EOF);
}  // HashLookupUnwindStage::getNext

void HashLookupUnwindStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();
    outerChild()->close();
    reset(true /* fromClose */);
}

std::unique_ptr<PlanStageStats> HashLookupUnwindStage::getStats(bool includeDebugInfo) const {
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

const SpecificStats* HashLookupUnwindStage::getSpecificStats() const {
    return _hashTable.getHashLookupStats();
}

std::vector<DebugPrinter::Block> HashLookupUnwindStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    DebugPrinter::addIdentifier(ret, _lookupStageOutputSlot);

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
}

size_t HashLookupUnwindStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_innerProjectSlot);
    return size;
}
}  // namespace mongo::sbe
