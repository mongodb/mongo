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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/stages/unwind.h"

#include "mongo/util/str.h"

namespace mongo::sbe {
UnwindStage::UnwindStage(std::unique_ptr<PlanStage> input,
                         value::SlotId inField,
                         value::SlotId outField,
                         value::SlotId outIndex,
                         bool preserveNullAndEmptyArrays,
                         PlanNodeId planNodeId)
    : PlanStage("unwind"_sd, planNodeId),
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
                                         _commonStats.nodeId);
}

void UnwindStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    // Get the inField (incoming) accessor.
    _inFieldAccessor = _children[0]->getAccessor(ctx, _inField);

    // Prepare the outField output accessor.
    _outFieldOutputAccessor = std::make_unique<value::ViewOfValueAccessor>();

    // Prepare the outIndex output accessor.
    _outIndexOutputAccessor = std::make_unique<value::ViewOfValueAccessor>();
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
    _commonStats.opens++;
    _children[0]->open(reOpen);

    _index = 0;
    _inArray = false;
}

PlanState UnwindStage::getNext() {
    if (!_inArray) {
        do {
            auto state = _children[0]->getNext();
            if (state != PlanState::ADVANCED) {
                return trackPlanState(state);
            }

            // Get the value.
            auto [tag, val] = _inFieldAccessor->getViewOfValue();

            if (value::isArray(tag)) {
                _inArrayAccessor.reset(tag, val);
                _index = 0;
                _inArray = true;

                // Empty input array.
                if (_inArrayAccessor.atEnd()) {
                    _inArray = false;
                    if (_preserveNullAndEmptyArrays) {
                        _outFieldOutputAccessor->reset(value::TypeTags::Nothing, 0);
                        _outIndexOutputAccessor->reset(value::TypeTags::NumberInt64,
                                                       value::bitcastFrom<int64_t>(_index));
                        return trackPlanState(PlanState::ADVANCED);
                    }
                }
            } else {
                bool nullOrNothing =
                    tag == value::TypeTags::Null || tag == value::TypeTags::Nothing;

                if (!nullOrNothing || _preserveNullAndEmptyArrays) {
                    _outFieldOutputAccessor->reset(tag, val);
                    _outIndexOutputAccessor->reset(value::TypeTags::Nothing, 0);
                    return trackPlanState(PlanState::ADVANCED);
                }
            }
        } while (!_inArray);
    }

    // We are inside the array so pull out the current element and advance.
    auto [tagElem, valElem] = _inArrayAccessor.getViewOfValue();

    _outFieldOutputAccessor->reset(tagElem, valElem);
    _outIndexOutputAccessor->reset(value::TypeTags::NumberInt64,
                                   value::bitcastFrom<int64_t>(_index));

    _inArrayAccessor.advance();
    ++_index;

    if (_inArrayAccessor.atEnd()) {
        _inArray = false;
    }

    return trackPlanState(PlanState::ADVANCED);
}

void UnwindStage::close() {
    _commonStats.closes++;
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> UnwindStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats());
    return ret;
}

const SpecificStats* UnwindStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> UnwindStage::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "unwind");

    DebugPrinter::addIdentifier(ret, _outField);
    DebugPrinter::addIdentifier(ret, _outIndex);
    DebugPrinter::addIdentifier(ret, _inField);
    ret.emplace_back(_preserveNullAndEmptyArrays ? "true" : "false");

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}
}  // namespace mongo::sbe
