/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <tuple>

#include <absl/container/inlined_vector.h>
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
namespace {
/**
 * Returns a materialized row with values owned by the caller.
 */
value::MaterializedRow materializeCopyOfRow(std::vector<value::SlotAccessor*>& accessors) {
    value::MaterializedRow row{accessors.size()};

    size_t idx = 0;
    for (auto& accessor : accessors) {
        auto [tag, val] = accessor->getViewOfValue();
        std::tie(tag, val) = value::copyValue(tag, val);
        row.reset(idx++, true, tag, val);
    }
    return row;
}

/**
 * Returns a materialized row shallow non-owning view. The materialized row values are valid only
 * until the contents of the slots change (usually as a result of calling getNext()).
 */
value::MaterializedRow materializeViewOfRow(std::vector<value::SlotAccessor*>& accessors) {
    value::MaterializedRow row{accessors.size()};

    size_t idx = 0;
    for (auto& accessor : accessors) {
        auto [tag, val] = accessor->getViewOfValue();
        row.reset(idx++, false, tag, val);
    }
    return row;
}
}  // namespace

MergeJoinStage::MergeJoinStage(std::unique_ptr<PlanStage> outer,
                               std::unique_ptr<PlanStage> inner,
                               value::SlotVector outerKeys,
                               value::SlotVector outerProjects,
                               value::SlotVector innerKeys,
                               value::SlotVector innerProjects,
                               std::vector<value::SortDirection> sortDirs,
                               PlanNodeId planNodeId,
                               bool participateInTrialRunTracking)
    : PlanStage("mj"_sd, nullptr /* yieldPolicy */, planNodeId, participateInTrialRunTracking),
      _outerKeys(std::move(outerKeys)),
      _outerProjects(std::move(outerProjects)),
      _innerKeys(std::move(innerKeys)),
      _innerProjects(std::move(innerProjects)),
      _dirs(std::move(sortDirs)),
      _currentOuterKey(0),
      _currentInnerKey(0),
      _rowEq(),
      _rowLt(_dirs) {
    tassert(5073700, "outer and inner size do not match", _outerKeys.size() == _innerKeys.size());
    tassert(5073701,
            "sort direction size does not match number of key slots",
            _outerKeys.size() == _dirs.size());

    _children.emplace_back(std::move(outer));
    _children.emplace_back(std::move(inner));
}

std::unique_ptr<PlanStage> MergeJoinStage::clone() const {
    return std::make_unique<MergeJoinStage>(_children[0]->clone(),
                                            _children[1]->clone(),
                                            _outerKeys,
                                            _outerProjects,
                                            _innerKeys,
                                            _innerProjects,
                                            _dirs,
                                            _commonStats.nodeId,
                                            _participateInTrialRunTracking);
}

void MergeJoinStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);
    _children[1]->prepare(ctx);

    size_t counter = 0;
    value::SlotSet dupCheck;
    for (auto& slot : _outerKeys) {
        auto [it, inserted] = dupCheck.emplace(slot);
        uassert(5073702, str::stream() << "duplicate field: " << slot, inserted);

        _outerKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));

        // Keys from the outer side should point to the keys in the current '_bufferKey' row, since
        // that is the where we point whichever key that we are using to attempt the join and we
        // want to make those outer keys accessible to the stage above.
        _outOuterInnerKeyAccessors.emplace_back(
            std::make_unique<value::MaterializedSingleRowAccessor>(_bufferKey, counter++));
        _outOuterAccessors[slot] = _outOuterInnerKeyAccessors.back().get();
    }

    counter = 0;
    for (auto& slot : _innerKeys) {
        auto [it, inserted] = dupCheck.emplace(slot);
        uassert(5073703, str::stream() << "duplicate field: " << slot, inserted);

        _innerKeyAccessors.emplace_back(_children[1]->getAccessor(ctx, slot));

        // Keys from the inner side should point to the same accessors as for the outer side keys
        // for same reason as the outer keys, since we attempt to join on whatever the '_bufferKey'
        // is and make inner keys also accessible to the stage above.
        _outOuterAccessors[slot] = _outOuterInnerKeyAccessors[counter++].get();
    }

    counter = 0;
    for (auto& slot : _outerProjects) {
        auto [it, inserted] = dupCheck.emplace(slot);
        uassert(5073704, str::stream() << "duplicate field: " << slot, inserted);

        _outerProjectAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));
        _outOuterProjectAccessors.emplace_back(std::make_unique<MergeJoinBufferAccessor>(
            _outerProjectsBuffer, _outerProjectsBufferIt, counter++));
        _outOuterAccessors[slot] = _outOuterProjectAccessors.back().get();
    }

    counter = 0;
    for (auto& slot : _innerProjects) {
        auto [it, inserted] = dupCheck.emplace(slot);
        uassert(5073705, str::stream() << "duplicate field: " << slot, inserted);

        _innerProjectAccessors.emplace_back(_children[1]->getAccessor(ctx, slot));

        _outInnerProjectAccessors.emplace_back(
            std::make_unique<value::MaterializedSingleRowAccessor>(_currentInnerProject,
                                                                   counter++));
        _outOuterAccessors[slot] = _outInnerProjectAccessors.back().get();
    }
}

value::SlotAccessor* MergeJoinStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _outOuterAccessors.find(slot); it != _outOuterAccessors.end()) {
        return it->second;
    }

    return ctx.getAccessor(slot);
}

void MergeJoinStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);
    _children[1]->open(reOpen);

    // Start with an initially empty buffer.
    _outerProjectsBuffer.clear();
    _outerProjectsBufferIt = 0;

    _bufferKey.resize(0);
    _currentOuterKey.resize(0);
    _currentInnerKey.resize(0);
    _currentInnerProject.resize(0);
}

PlanState MergeJoinStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // First append rows from the outer side into the buffer such that can join them with the inner
    // later, but only if have a valid row read from outer, i.e. this is not true during the first
    // time getNext() is called on the stage.
    if (!_currentOuterKey.isEmpty()) {
        while (!_isOuterDone) {
            // Get the next row, which may or may not have the same key as the one currently
            // buffering for.
            auto state = _children[0]->getNext();
            if (state == PlanState::IS_EOF) {
                // At this point have run out of rows from the outer, but may still need to go
                // through the inner and yield some rows in case the keys match, so do not return
                // EOF but mark outer as done and fallthrough to try joining with inner.
                _isOuterDone = true;
            } else {
                _currentOuterKey = materializeViewOfRow(_outerKeyAccessors);

                if (_rowEq(_bufferKey, _currentOuterKey)) {
                    _outerProjectsBuffer.emplace_back(materializeCopyOfRow(_outerProjectAccessors));
                } else {
                    break;
                }
            }
        }
        // Advance to the next row in the buffer of outer projects, since we must have already
        // produced a row earlier with whichever project in the current position of the iterator.
        ++_outerProjectsBufferIt;
    }

    // At this point have populated the buffer with some rows that have the same key, and
    // '_currentOuterKey' is advanced one row further to the row on outer with a different key or
    // this is the first time getNext() has been called so in that case skip over to the part where
    // looking for equal outer and inner rows.
    if (!_currentOuterKey.isEmpty()) {
        // Go through the inner side, and for each row where the key is equal to the buffer key,
        // iterate all rows in the buffer and yield rows. The requirement is to stop when the row
        // coming from the inner has a different key then the buffer key, i.e. then the situation is
        // that both outer and inner have advanced to new rows that may or may not be equal, so then
        // proceed to look for an equal outer and inner row.
        do {
            _currentInnerKey = materializeViewOfRow(_innerKeyAccessors);

            if (_rowEq(_bufferKey, _currentInnerKey)) {
                // If the iterator is not at the end of the buffer, that means we have advanced the
                // iterator to a new row in the buffer from outer that is still a valid row.
                if (_outerProjectsBufferIt != _outerProjectsBuffer.size()) {
                    // Fix the inner side row (key and project) by making a deep copy.
                    _currentInnerKey = materializeCopyOfRow(_innerKeyAccessors);
                    _currentInnerProject = materializeCopyOfRow(_innerProjectAccessors);

                    return trackPlanState(PlanState::ADVANCED);
                } else {
                    // Since iterated over all of the elements in the buffer and are currently
                    // pointing to the end of the buffer, need to reset the buffer to the beginning
                    // and call getNext() on the inner child in order to get the next row from the
                    // inner, which may or may not have the same key as the key for the outer rows
                    // that are being joined currently.
                    _outerProjectsBufferIt = 0;

                    auto state = _children[1]->getNext();
                    if (state == PlanState::IS_EOF) {
                        return trackPlanState(state);
                    }
                }
            } else {
                // Having called getNext() on the inner enough times such that the row coming from
                // the inner now has a different key then the one being joined with in the buffer,
                // now done doing the joins and can clear the buffer, reset the iterator and since
                // '_currentInnerKey' has already been updated to the new inner key, break out of
                // the loop.
                _outerProjectsBuffer.clear();
                _outerProjectsBufferIt = 0;

                break;
            }
        } while (true);
    }

    // It is possible that all rows from outer were exhausted earlier and may have been joined with
    // some from the inner while the keys matched, but now even though may have more on the inner
    // with different keys, need to return EOF.
    if (_isOuterDone)
        return trackPlanState(PlanState::IS_EOF);

    // At this point outer and inner keys are not equal or getNext() has not been called on either
    // outer or inner, so loop and get rows from both outer and inner until get an equal pair.
    do {
        while (_currentOuterKey.isEmpty() ||
               (!_currentInnerKey.isEmpty() && _rowLt(_currentOuterKey, _currentInnerKey))) {
            auto state = _children[0]->getNext();
            if (state == PlanState::IS_EOF) {
                return trackPlanState(state);
            }
            _currentOuterKey = materializeViewOfRow(_outerKeyAccessors);
        }

        while (_currentInnerKey.isEmpty() || _rowLt(_currentInnerKey, _currentOuterKey)) {
            auto state = _children[1]->getNext();
            if (state == PlanState::IS_EOF) {
                return trackPlanState(state);
            }
            _currentInnerKey = materializeViewOfRow(_innerKeyAccessors);
        }
    } while (!_rowEq(_currentOuterKey, _currentInnerKey));

    // Now that outer is equal to inner, before advancing need to make sure to populate the buffer
    // with the row from the outer.
    _outerProjectsBuffer.emplace_back(materializeCopyOfRow(_outerProjectAccessors));

    // Now also need to fix the single row from inner.
    _currentInnerKey = materializeCopyOfRow(_innerKeyAccessors);
    _currentInnerProject = materializeCopyOfRow(_innerProjectAccessors);

    // Set the key for the buffer here since whatever further rows will be appended to buffer from
    // outer (if any) will have the same key. Currently '_outerKeyAccessors' is a view-only shallow
    // copy, so we need to materialize the row this time getting a deep copy.
    _bufferKey = materializeCopyOfRow(_outerKeyAccessors);

    return trackPlanState(PlanState::ADVANCED);
}

void MergeJoinStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
    _children[1]->close();
    _outerProjectsBuffer.clear();
}

void MergeJoinStage::doSaveState(bool relinquishCursor) {
    if (!relinquishCursor) {
        return;
    }

    // We only have to save shallow non-owning materialized rows.
    prepareForYielding(_currentOuterKey, true);
    prepareForYielding(_currentInnerKey, true);
}

std::unique_ptr<PlanStageStats> MergeJoinStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.append("outerKeys", _outerKeys.begin(), _outerKeys.end());
        bob.append("outerProjects", _outerProjects.begin(), _outerProjects.end());
        bob.append("innerKeys", _innerKeys.begin(), _innerKeys.end());
        bob.append("innerProjects", _innerProjects.begin(), _innerProjects.end());
        bob.append("sortDirs", _dirs);
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    ret->children.emplace_back(_children[1]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* MergeJoinStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> MergeJoinStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _dirs.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret,
                                    _dirs[idx] == value::SortDirection::Ascending ? "asc" : "desc");
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    DebugPrinter::addKeyword(ret, "left");

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outerKeys.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _outerKeys[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _outerProjects.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _outerProjects[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    DebugPrinter::addKeyword(ret, "right");
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _innerKeys.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _innerKeys[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _innerProjects.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _innerProjects[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, _children[1]->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    return ret;
}

size_t MergeJoinStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_outerKeys);
    size += size_estimator::estimate(_outerProjects);
    size += size_estimator::estimate(_innerKeys);
    size += size_estimator::estimate(_innerProjects);
    size += size_estimator::estimate(_dirs);
    return size;
}
}  // namespace sbe
}  // namespace mongo
