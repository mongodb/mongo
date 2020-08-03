/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/check_bounds.h"

namespace mongo::sbe {
CheckBoundsStage::CheckBoundsStage(std::unique_ptr<PlanStage> input,
                                   const CheckBoundsParams& params,
                                   value::SlotId inKeySlot,
                                   value::SlotId inRecordIdSlot,
                                   value::SlotId outSlot)
    : PlanStage{"chkbounds"_sd},
      _params{params},
      _checker{&_params.bounds, _params.keyPattern, _params.direction},
      _inKeySlot{inKeySlot},
      _inRecordIdSlot{inRecordIdSlot},
      _outSlot{outSlot} {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> CheckBoundsStage::clone() const {
    return std::make_unique<CheckBoundsStage>(
        _children[0]->clone(), _params, _inKeySlot, _inRecordIdSlot, _outSlot);
}

void CheckBoundsStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    _inKeyAccessor = _children[0]->getAccessor(ctx, _inKeySlot);
    _inRecordIdAccessor = _children[0]->getAccessor(ctx, _inRecordIdSlot);
}

value::SlotAccessor* CheckBoundsStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_outSlot == slot) {
        return &_outAccessor;
    }

    return _children[0]->getAccessor(ctx, slot);
}

void CheckBoundsStage::open(bool reOpen) {
    _commonStats.opens++;
    _children[0]->open(reOpen);
    _isEOF = false;
}

PlanState CheckBoundsStage::getNext() {
    if (_isEOF) {
        return trackPlanState(PlanState::IS_EOF);
    }

    auto state = _children[0]->getNext();

    if (state == PlanState::ADVANCED) {
        auto [keyTag, keyVal] = _inKeyAccessor->getViewOfValue();
        const auto msgKeyTag = keyTag;
        uassert(ErrorCodes::BadValue,
                str::stream() << "Wrong index key type: " << msgKeyTag,
                keyTag == value::TypeTags::ksValue);

        auto key = value::getKeyStringView(keyVal);
        auto bsonKey = KeyString::toBson(*key, _params.ord);
        IndexSeekPoint seekPoint;

        switch (_checker.checkKey(bsonKey, &seekPoint)) {
            case IndexBoundsChecker::VALID: {
                auto [tag, val] = _inRecordIdAccessor->getViewOfValue();
                _outAccessor.reset(false, tag, val);
                break;
            }

            case IndexBoundsChecker::DONE:
                state = PlanState::IS_EOF;
                break;

            case IndexBoundsChecker::MUST_ADVANCE: {
                auto seekKey = std::make_unique<KeyString::Value>(
                    IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                        seekPoint, _params.version, _params.ord, _params.direction == 1));
                _outAccessor.reset(
                    true, value::TypeTags::ksValue, value::bitcastFrom(seekKey.release()));
                // We should return the seek key provided by the 'IndexBoundsChecker' to restart
                // the index scan, but we should stop on the next call to 'getNext' since we've just
                // passed behind the current interval and need to signal the parent stage that we're
                // done and can only continue further once the stage is reopened.
                _isEOF = true;
                break;
            }
        }
    }
    return trackPlanState(state);
}

void CheckBoundsStage::close() {
    _commonStats.closes++;
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> CheckBoundsStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats());
    return ret;
}

const SpecificStats* CheckBoundsStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> CheckBoundsStage::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "chkbounds");

    DebugPrinter::addIdentifier(ret, _inKeySlot);
    DebugPrinter::addIdentifier(ret, _inRecordIdSlot);
    DebugPrinter::addIdentifier(ret, _outSlot);

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    return ret;
}
}  // namespace mongo::sbe
