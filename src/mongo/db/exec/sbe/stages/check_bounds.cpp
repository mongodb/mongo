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

#include "mongo/db/exec/sbe/size_estimator.h"

namespace mongo::sbe {
CheckBoundsStage::CheckBoundsStage(std::unique_ptr<PlanStage> input,
                                   CheckBoundsParams params,
                                   value::SlotId inKeySlot,
                                   value::SlotId inRecordIdSlot,
                                   value::SlotId outSlot,
                                   PlanNodeId planNodeId,
                                   bool participateInTrialRunTracking)
    : PlanStage{"chkbounds"_sd, planNodeId, participateInTrialRunTracking},
      _params{std::move(params)},
      _inKeySlot{inKeySlot},
      _inRecordIdSlot{inRecordIdSlot},
      _outSlot{outSlot} {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> CheckBoundsStage::clone() const {
    return std::make_unique<CheckBoundsStage>(_children[0]->clone(),
                                              _params,
                                              _inKeySlot,
                                              _inRecordIdSlot,
                                              _outSlot,
                                              _commonStats.nodeId,
                                              _participateInTrialRunTracking);
}

void CheckBoundsStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    _inKeyAccessor = _children[0]->getAccessor(ctx, _inKeySlot);
    _inRecordIdAccessor = _children[0]->getAccessor(ctx, _inRecordIdSlot);

    // Set up the IndexBounds accessor for the slot where IndexBounds will be stored.
    _indexBoundsAccessor =
        stdx::visit(OverloadedVisitor{[](const IndexBounds&) -> RuntimeEnvironment::Accessor* {
                                          return nullptr;
                                      },
                                      [&ctx](CheckBoundsParams::RuntimeEnvironmentSlotId slot) {
                                          return ctx.getRuntimeEnvAccessor(slot);
                                      }},
                    _params.indexBounds);
}

value::SlotAccessor* CheckBoundsStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_outSlot == slot) {
        return &_outAccessor;
    }

    return _children[0]->getAccessor(ctx, slot);
}

void CheckBoundsStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);
    _isEOF = false;

    // Set up the IndexBoundsChecker by extracting the IndexBounds from the RuntimeEnvironment if
    // value is provided there.
    auto indexBoundsPtr = stdx::visit(
        OverloadedVisitor{
            [](const IndexBounds& indexBounds) { return &indexBounds; },
            [&](CheckBoundsParams::RuntimeEnvironmentSlotId) -> const IndexBounds* {
                tassert(6579900, "'_indexBoundsAccessor' must be populated", _indexBoundsAccessor);
                auto [tag, val] = _indexBoundsAccessor->getViewOfValue();
                tassert(6579901,
                        "The index bounds expression must be of type 'indexBounds'",
                        tag == value::TypeTags::indexBounds);
                return value::getIndexBoundsView(val);
            }},
        _params.indexBounds);
    _checker.emplace(indexBoundsPtr, _params.keyPattern, _params.direction);
}

PlanState CheckBoundsStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (_isEOF) {
        return trackPlanState(PlanState::IS_EOF);
    }

    // We are about to call getNext() on our child so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the getNext() call.
    disableSlotAccess();
    auto state = _children[0]->getNext();

    if (state == PlanState::ADVANCED) {
        auto [keyTag, keyVal] = _inKeyAccessor->getViewOfValue();
        const auto msgKeyTag = keyTag;
        uassert(ErrorCodes::BadValue,
                str::stream() << "Wrong index key type: " << msgKeyTag,
                keyTag == value::TypeTags::ksValue);

        auto key = value::getKeyStringView(keyVal);

        _keyBuffer.reset();
        BSONObjBuilder keyBuilder(_keyBuffer);
        KeyString::toBsonSafe(
            key->getBuffer(), key->getSize(), _params.ord, key->getTypeBits(), keyBuilder);
        auto bsonKey = keyBuilder.done();

        switch (_checker->checkKey(bsonKey, &_seekPoint)) {
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
                        _seekPoint, _params.version, _params.ord, _params.direction == 1));
                _outAccessor.reset(true,
                                   value::TypeTags::ksValue,
                                   value::bitcastFrom<KeyString::Value*>(seekKey.release()));
                // We should return the seek key provided by the 'IndexBoundsChecker' to restart
                // the index scan, but we should stop on the next call to 'getNext' since we've just
                // passed behind the current interval and need to signal the parent stage that we're
                // done and can only continue further once the stage is reopened.
                _isEOF = true;

                ++_specificStats.seeks;
                break;
            }
        }
    }
    return trackPlanState(state);
}

void CheckBoundsStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> CheckBoundsStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<CheckBoundsStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("seeks", static_cast<long long>(_specificStats.seeks));
        bob.appendNumber("inKeySlot", static_cast<long long>(_inKeySlot));
        bob.appendNumber("inRecordIdSlot", static_cast<long long>(_inRecordIdSlot));
        bob.appendNumber("outSlot", static_cast<long long>(_outSlot));
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* CheckBoundsStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> CheckBoundsStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    DebugPrinter::addIdentifier(ret, _inKeySlot);
    DebugPrinter::addIdentifier(ret, _inRecordIdSlot);
    DebugPrinter::addIdentifier(ret, _outSlot);

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    return ret;
}

size_t CheckBoundsStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_specificStats);
    size += size_estimator::estimate(_params.keyPattern);
    size += stdx::visit(
        OverloadedVisitor{
            [](const IndexBounds& indexBounds) { return size_estimator::estimate(indexBounds); },
            [](CheckBoundsParams::RuntimeEnvironmentSlotId) -> size_t { return 0; }},
        _params.indexBounds);
    size += size_estimator::estimate(_seekPoint);
    if (_checker) {
        size += size_estimator::estimate(*_checker);
    }
    size += size_estimator::estimate(_keyBuffer);
    return size;
}

void CheckBoundsStage::doSaveState(bool relinquishCursor) {
    if (!slotsAccessible() || !relinquishCursor) {
        return;
    }

    prepareForYielding(_outAccessor);
}
}  // namespace mongo::sbe
