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

#include "mongo/db/exec/sbe/stages/unwind.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>


namespace mongo::sbe {
UnwindStage::UnwindStage(std::unique_ptr<PlanStage> input,
                         value::SlotId inField,
                         value::SlotId outField,
                         value::SlotId outIndex,
                         bool preserveNullAndEmptyArrays,
                         PlanNodeId planNodeId,
                         PlanYieldPolicy* yieldPolicy,
                         bool participateInTrialRunTracking)
    : PlanStage("unwind"_sd, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _inField(inField),
      _outField(outField),
      _outIndex(outIndex),
      _preserveNullAndEmptyArrays(preserveNullAndEmptyArrays) {
    _children.emplace_back(std::move(input));

    if (_outField == _outIndex) {
        uasserted(4822805, str::stream() << "duplicate field name: " << _outField);
    }
}

std::unique_ptr<PlanStage> UnwindStage::clone() const {
    return std::make_unique<UnwindStage>(_children[0]->clone(),
                                         _inField,
                                         _outField,
                                         _outIndex,
                                         _preserveNullAndEmptyArrays,
                                         _commonStats.nodeId,
                                         _yieldPolicy,
                                         participateInTrialRunTracking());
}

void UnwindStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    // Get the inField (incoming) accessor.
    _inFieldAccessor = _children[0]->getAccessor(ctx, _inField);

    // Prepare the outField output accessor.
    _outFieldOutputAccessor = std::make_unique<value::OwnedValueAccessor>();

    // Prepare the outIndex output accessor.
    _outIndexOutputAccessor = std::make_unique<value::OwnedValueAccessor>();
}

value::SlotAccessor* UnwindStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_outField == slot) {
        return _outFieldOutputAccessor.get();
    }

    if (_outIndex == slot) {
        return _outIndexOutputAccessor.get();
    }

    return _children[0]->getAccessor(ctx, slot);
}

void UnwindStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);

    _index = 0;
    _inArray = false;
}

PlanState UnwindStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // For performance, we intentionally do not yield while '_inArray' is true. When it is false, we
    // unconditionally call our child's getNext() so are not required to yield here.
    checkForInterruptNoYield(_opCtx);

#if defined(MONGO_CONFIG_DEBUG_BUILD)
    // If we are iterating over the array, then we are not going to advance the child.
    // In such case, we expect that child slots are accessible, as otherwise our enumerator state is
    // not valid.
    tassert(7200405,
            "Child stage slots must be accessible while iterating over array elements",
            !_inArray || _children[0]->slotsAccessible());
#endif

    while (!_inArray) {
        // We are about to call getNext() on our child so do not bother saving our internal
        // state in case it yields as the state will be completely overwritten after the
        // getNext() call.
        disableSlotAccess();
        auto state = _children[0]->getNext();
        if (state != PlanState::ADVANCED) {
            return trackPlanState(state);
        }

        // Get the value.
        auto [tag, val] = _inFieldAccessor->getViewOfValue();

        if (value::isArray(tag)) {
            _inArrayAccessor.reset(_inFieldAccessor);
            _index = 0;
            _inArray = true;

            // Empty input array.
            if (_inArrayAccessor.atEnd()) {
                _inArray = false;
                if (_preserveNullAndEmptyArrays) {
                    _outFieldOutputAccessor->reset(false, value::TypeTags::Nothing, 0);
                    // -1 array index indicates the unwind field was an array, but it was empty.
                    _outIndexOutputAccessor->reset(
                        false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-1));
                    return trackPlanState(PlanState::ADVANCED);
                }
            }
        } else {
            bool nullOrNothing = tag == value::TypeTags::Null || tag == value::TypeTags::Nothing;

            if (!nullOrNothing || _preserveNullAndEmptyArrays) {
                _outFieldOutputAccessor->reset(false, tag, val);
                // Null array index indicates the unwind field was not an array.
                _outIndexOutputAccessor->reset(false, value::TypeTags::Null, 0);
                return trackPlanState(PlanState::ADVANCED);
            }
        }
    }  // while (!_inArray)

    // We are inside the array so pull out the current element and advance.
    auto [tagElem, valElem] = _inArrayAccessor.getViewOfValue();

    _outFieldOutputAccessor->reset(false, tagElem, valElem);
    _outIndexOutputAccessor->reset(
        false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(_index));

    _inArrayAccessor.advance();
    ++_index;

    if (_inArrayAccessor.atEnd()) {
        _inArray = false;
    }

    return trackPlanState(PlanState::ADVANCED);
}

void UnwindStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
    _index = 0;
    _inArray = false;
}

std::unique_ptr<PlanStageStats> UnwindStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("inputSlot", static_cast<long long>(_inField));
        bob.appendNumber("outSlot", static_cast<long long>(_outField));
        bob.appendNumber("outIndexSlot", static_cast<long long>(_outIndex));
        bob.appendNumber("preserveNullAndEmptyArrays", _preserveNullAndEmptyArrays);
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* UnwindStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> UnwindStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    DebugPrinter::addIdentifier(ret, _outField);
    DebugPrinter::addIdentifier(ret, _outIndex);
    DebugPrinter::addIdentifier(ret, _inField);
    ret.emplace_back(_preserveNullAndEmptyArrays ? "true" : "false");

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

void UnwindStage::doSaveState() {
    if (_outFieldOutputAccessor) {
        prepareForYielding(*_outFieldOutputAccessor, slotsAccessible());
    }
    if (_outIndexOutputAccessor) {
        prepareForYielding(*_outIndexOutputAccessor, slotsAccessible());
    }
}

void UnwindStage::doRestoreState() {
    if (!_inArray) {
        // If we were once in an array but no longer are, this saves us from doing a refresh() on
        // obsolete slot contents.
        return;
    }

    // The child stage will have copied the in-flight contents of this slot because WiredTiger
    // will free the memory owned by the cursor it points to, so on restore we must update the
    // embedded array iterator to point to the new memory location.
    _inArrayAccessor.refresh();
}

size_t UnwindStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    return size;
}
}  // namespace mongo::sbe
