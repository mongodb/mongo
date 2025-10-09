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

#include "mongo/db/exec/sbe/stages/hash_agg.h"

#include <absl/container/inlined_vector.h>
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <type_traits>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
HashAggStage::HashAggStage(std::unique_ptr<PlanStage> input,
                           value::SlotVector gbs,
                           AggExprVector aggs,
                           value::SlotVector seekKeysSlots,
                           bool optimizedClose,
                           boost::optional<value::SlotId> collatorSlot,
                           bool allowDiskUse,
                           SlotExprPairVector mergingExprs,
                           PlanYieldPolicy* yieldPolicy,
                           PlanNodeId planNodeId,
                           bool participateInTrialRunTracking,
                           bool forceIncreasedSpilling)
    : HashAggBaseStage("group"_sd,
                       yieldPolicy,
                       planNodeId,
                       nullptr,
                       participateInTrialRunTracking,
                       allowDiskUse,
                       forceIncreasedSpilling),
      _gbs(std::move(gbs)),
      _aggs(std::move(aggs)),
      _collatorSlot(collatorSlot),
      _seekKeysSlots(std::move(seekKeysSlots)),
      _optimizedClose(optimizedClose),
      _mergingExprs(std::move(mergingExprs)) {
    _children.emplace_back(std::move(input));
    invariant(_seekKeysSlots.empty() || _seekKeysSlots.size() == _gbs.size());
    tassert(5843100,
            "HashAgg stage was given optimizedClose=false and seek keys",
            _seekKeysSlots.empty() || _optimizedClose);

    if (_allowDiskUse) {
        tassert(7039549,
                "disk use enabled for HashAggStage but incorrect number of merging expresssions",
                _aggs.size() == _mergingExprs.size());
    }
}

std::unique_ptr<PlanStage> HashAggStage::clone() const {
    AggExprVector aggs;
    aggs.reserve(_aggs.size());
    for (auto& [slot, expr] : _aggs) {
        std::unique_ptr<EExpression> initExpr{nullptr};
        if (expr.init) {
            initExpr = expr.init->clone();
        }
        aggs.push_back(std::make_pair(slot, AggExprPair{std::move(initExpr), expr.agg->clone()}));
    }

    SlotExprPairVector mergingExprsClone;
    mergingExprsClone.reserve(_mergingExprs.size());
    for (auto&& [k, v] : _mergingExprs) {
        mergingExprsClone.push_back({k, v->clone()});
    }

    return std::make_unique<HashAggStage>(_children[0]->clone(),
                                          _gbs,
                                          std::move(aggs),
                                          _seekKeysSlots,
                                          _optimizedClose,
                                          _collatorSlot,
                                          _allowDiskUse,
                                          std::move(mergingExprsClone),
                                          _yieldPolicy,
                                          _commonStats.nodeId,
                                          participateInTrialRunTracking(),
                                          _forceIncreasedSpilling);
}

void HashAggStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(5402501,
                "collator accessor should exist if collator slot provided to HashAggStage",
                _collatorAccessor != nullptr);
    }

    value::SlotSet dupCheck;
    auto throwIfDupSlot = [&dupCheck](value::SlotId slot) {
        auto [_, inserted] = dupCheck.emplace(slot);
        tassert(7039551, "duplicate slot id", inserted);
    };

    size_t counter = 0;
    // Process group by columns.
    for (auto& slot : _gbs) {
        throwIfDupSlot(slot);

        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));

        // Construct accessors for obtaining the key values from either the hash table '_ht' or the
        // '_recordStore'.
        _outHashKeyAccessors.emplace_back(std::make_unique<HashKeyAccessor>(_htIt, counter));
        _outRecordStoreKeyAccessors.emplace_back(
            std::make_unique<value::MaterializedSingleRowAccessor>(_outKeyRowRecordStore, counter));

        // A 'SwitchAccessor' is used to point the '_outKeyAccessors' to the key coming from the
        // '_ht' or the '_recordStore' when draining the HashAgg stage in getNext(). If no spilling
        // occurred, the keys will be obtained from the hash table. If spilling kicked in, then all
        // of the data is written out to the record store, so the 'SwitchAccessor' is reconfigured
        // to obtain all of the keys from the spill table.
        _outKeyAccessors.emplace_back(
            std::make_unique<value::SwitchAccessor>(std::vector<value::SlotAccessor*>{
                _outHashKeyAccessors.back().get(), _outRecordStoreKeyAccessors.back().get()}));

        _outAccessors[slot] = _outKeyAccessors.back().get();

        ++counter;
    }

    // Process seek keys (if any). The keys must come from outside of the subtree (by definition) so
    // we go directly to the compilation context.
    for (auto& slot : _seekKeysSlots) {
        _seekKeysAccessors.emplace_back(ctx.getAccessor(slot));
    }

    counter = 0;
    for (auto& [slot, expr] : _aggs) {
        throwIfDupSlot(slot);

        // Just like with the output accessors for the keys, we construct output accessors for the
        // aggregate values that read from either the hash table '_ht' or the '_recordStore'.
        _outRecordStoreAggAccessors.emplace_back(
            std::make_unique<value::MaterializedSingleRowAccessor>(_outAggRowRecordStore, counter));
        _outHashAggAccessors.emplace_back(std::make_unique<HashAggAccessor>(_htIt, counter));

        // A 'SwitchAccessor' is used to toggle the '_outAggAccessors' between the '_ht' and the
        // '_recordStore'. Just like the key values, the aggregate values are always obtained from
        // the hash table if no spilling occurred and are always obtained from the record store if
        // spilling occurred.
        _outAggAccessors.emplace_back(
            std::make_unique<value::SwitchAccessor>(std::vector<value::SlotAccessor*>{
                _outHashAggAccessors.back().get(), _outRecordStoreAggAccessors.back().get()}));

        _outAccessors[slot] = _outAggAccessors.back().get();

        ctx.root = this;
        std::unique_ptr<vm::CodeFragment> initCode{nullptr};
        if (expr.init) {
            initCode = expr.init->compile(ctx);
        }
        ctx.aggExpression = true;
        ctx.accumulator = _outAggAccessors.back().get();
        _aggCodes.emplace_back(std::move(initCode), expr.agg->compile(ctx));
        ctx.aggExpression = false;

        ++counter;
    }

    // If disk use is allowed, then we need to compile the merging expressions as well.
    if (_allowDiskUse) {
        counter = 0;
        for (auto&& [spillSlot, mergingExpr] : _mergingExprs) {
            throwIfDupSlot(spillSlot);

            _spilledAggsAccessors.push_back(
                std::make_unique<value::MaterializedSingleRowAccessor>(_spilledAggRow, counter));
            _spilledAggsAccessorMap[spillSlot] = _spilledAggsAccessors.back().get();

            ctx.root = this;
            ctx.aggExpression = true;
            ctx.accumulator = _outAggAccessors[counter].get();
            _mergingExprCodes.emplace_back(mergingExpr->compile(ctx));
            ctx.aggExpression = false;

            ++counter;
        }
    }

    _compiled = true;
}

value::SlotAccessor* HashAggStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compiled) {
        if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
            return it->second;
        }
    } else {
        // The slots into which we read spilled partial aggregates, accessible via
        // '_spilledAggsAccessors', should only be visible to this stage. They are used internally
        // when merging spilled partial aggregates and should never be read by ancestor stages.
        // Therefore, they are only made visible when this stage is in the process of compiling
        // itself.
        if (auto it = _spilledAggsAccessorMap.find(slot); it != _spilledAggsAccessorMap.end()) {
            return it->second;
        }

        return _children[0]->getAccessor(ctx, slot);
    }

    return ctx.getAccessor(slot);
}

void HashAggStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;

    if (!reOpen || _seekKeysAccessors.empty()) {
        tassert(10226701, "Expecting _opCtx to be populated", _opCtx);
        _children[0]->open(_childOpened);
        _childOpened = true;
        if (_collatorAccessor) {
            auto [tag, collatorVal] = _collatorAccessor->getViewOfValue();
            uassert(
                5402503, "collatorSlot must be of collator type", tag == value::TypeTags::collator);
            auto collatorView = value::getCollatorView(collatorVal);
            const value::MaterializedRowHasher hasher(collatorView);
            _keyEq = value::MaterializedRowEq(collatorView);
            _ht.emplace(0, hasher, _keyEq);
        } else {
            _ht.emplace();
        }

        _seekKeys.resize(_seekKeysAccessors.size());

        // Reset state since this stage may have been previously opened.
        for (auto&& accessor : _outKeyAccessors) {
            accessor->setIndex(0);
        }
        for (auto&& accessor : _outAggAccessors) {
            accessor->setIndex(0);
        }
        if (_recordStore) {
            _recordStore->resetCursor(_opCtx, _rsCursor);
        }
        _rsCursor.reset();
        _recordStore.reset();
        _outKeyRowRecordStore = {0};
        _outAggRowRecordStore = {0};
        _spilledAggRow = {0};
        _stashedNextRow = {0, 0};

        MemoryCheckData memoryCheckData;

        value::MaterializedRow key{_inKeyAccessors.size()};

        // If the group-by key is empty, we aggregate into a single row. In this case, avoid hash
        // table lookups for each child document.
        bool first = true;
        const bool groupByListHasSlots = !_inKeyAccessors.empty();
        while (_children[0]->getNext() == PlanState::ADVANCED) {
            bool newKey = false;
            if (groupByListHasSlots || first) {
                // Copy keys in order to do the lookup.
                size_t idx = 0;
                for (auto& p : _inKeyAccessors) {
                    auto [tag, val] = p->getViewOfValue();
                    key.reset(idx++, false, tag, val);
                }
                _htIt = _ht->find(key);
                first = false;
            }
            dassert(_htIt == _ht->find(key));
            if (_htIt == _ht->end()) {
                // The key is not present in the hash table yet, so we insert it and initialize the
                // corresponding accumulator. Note that as a future optimization, we could avoid
                // doing a lookup both in the 'find()' call and in 'emplace()'.
                newKey = true;
                value::MaterializedRow keyCopy(key);
                keyCopy.makeOwned();
                auto [it, _] = _ht->emplace(std::move(keyCopy), value::MaterializedRow{0});
                it->second.resize(_outAggAccessors.size());

                _htIt = it;

                // Run accumulator initializer if needed.
                for (size_t idx = 0; idx < _outAggAccessors.size(); ++idx) {
                    if (_aggCodes[idx].first) {
                        auto [owned, tag, val] = _bytecode.run(_aggCodes[idx].first.get());
                        _outHashAggAccessors[idx]->reset(owned, tag, val);
                    }
                }
            }

            // Accumulate state in '_ht'.
            for (size_t idx = 0; idx < _outAggAccessors.size(); ++idx) {
                auto [owned, tag, val] = _bytecode.run(_aggCodes[idx].second.get());
                _outHashAggAccessors[idx]->reset(owned, tag, val);
            }

            // If the group-by key is empty we will only ever aggregate into a single row so no
            // sense in spilling.
            if (groupByListHasSlots) {
                if (_forceIncreasedSpilling && !newKey) {
                    // If configured to spill more than usual, we spill after seeing the same key
                    // twice.
                    spill(memoryCheckData);
                } else {
                    // Estimates how much memory is being used. If we estimate that the hash table
                    // exceeds the allotted memory budget, its contents are spilled to the
                    // '_recordStore' and '_ht' is cleared.
                    checkMemoryUsageAndSpillIfNecessary(memoryCheckData);
                }
            }

            trackResult();
        }

        if (_optimizedClose) {
            _children[0]->close();
            _childOpened = false;
        }

        // If we spilled at any point while consuming the input, then do one final spill to write
        // any leftover contents of '_ht' to the record store. That way, when recovering the input
        // from the record store and merging partial aggregates we don't have to worry about the
        // possibility of some of the data being in the hash table and some being in the record
        // store.
        if (_recordStore) {
            if (!_ht->empty()) {
                spill(memoryCheckData);
            }

            _specificStats.spilledDataStorageSize = _recordStore->rs()->storageSize(_opCtx);
            groupCounters.incrementGroupCountersPerQuery(_specificStats.spilledDataStorageSize);

            // Establish a cursor, positioned at the beginning of the record store.
            _rsCursor = _recordStore->getCursor(_opCtx);

            // Callers will be obtaining the results from the spill table, so set the
            // 'SwitchAccessors' so that they refer to the rows recovered from the record store
            // under the hood.
            for (auto&& accessor : _outKeyAccessors) {
                accessor->setIndex(1);
            }
            for (auto&& accessor : _outAggAccessors) {
                accessor->setIndex(1);
            }
        }
    }

    if (!_seekKeysAccessors.empty()) {
        // Copy keys in order to do the lookup.
        size_t idx = 0;
        for (auto& p : _seekKeysAccessors) {
            auto [tag, val] = p->getViewOfValue();
            _seekKeys.reset(idx++, false, tag, val);
        }
    }

    _htIt = _ht->end();
}

HashAggBaseStage<HashAggStage>::SpilledRow HashAggStage::deserializeSpilledRecordWithCollation(
    const Record& record, const CollatorInterface& collator) {
    BufReader valReader(record.data.data(), record.data.size());

    // When a collator has been defined, both the key and the value are stored in the data part of
    // the record. First read the key and then read the value.
    auto key = value::MaterializedRow::deserializeForSorter(valReader, {&collator});
    auto val = value::MaterializedRow::deserializeForSorter(valReader, {&collator});
    return {std::move(key), std::move(val)};
}

PlanState HashAggStage::getNextSpilled() {
    CollatorInterface* collator = nullptr;
    if (_collatorAccessor) {
        auto [colTag, colVal] = _collatorAccessor->getViewOfValue();
        collator = value::getCollatorView(colVal);
    }

    // Use the appropriate method to deserialize the record based on whether a collator is used.
    auto recoverSpilledRecord =
        [this](const Record& record, BufBuilder& keyBuffer, const CollatorInterface* collator) {
            if (collator) {
                return deserializeSpilledRecordWithCollation(record, *collator);
            }
            return deserializeSpilledRecord(record, _gbs.size(), keyBuffer);
        };

    tassert(10226700, "Expecting _rsCursor to be populated", _rsCursor);
    if (_stashedNextRow.first.isEmpty()) {
        auto nextRecord = _rsCursor->next();
        if (!nextRecord) {
            return trackPlanState(PlanState::IS_EOF);
        }

        // We are just starting the process of merging the spilled file segments.
        auto recoveredRow = recoverSpilledRecord(*nextRecord, _outKeyRowRSBuffer, collator);

        _outKeyRowRecordStore = std::move(recoveredRow.first);
        _outAggRowRecordStore = std::move(recoveredRow.second);
    } else {
        // We peeked at the next key last time around.
        _outKeyRowRSBuffer = std::move(_stashedKeyBuffer);
        _outKeyRowRecordStore = std::move(_stashedNextRow.first);
        _outAggRowRecordStore = std::move(_stashedNextRow.second);
        // Clear the stashed row.
        _stashedNextRow = {0, 0};
    }

    // Find additional partial aggregates for the same key and merge them in order to compute the
    // final output.
    for (auto nextRecord = _rsCursor->next(); nextRecord; nextRecord = _rsCursor->next()) {
        auto recoveredRow = recoverSpilledRecord(*nextRecord, _stashedKeyBuffer, collator);
        if (!_keyEq(recoveredRow.first, _outKeyRowRecordStore)) {
            // The newly recovered spilled row belongs to a new key, so we're done merging partial
            // aggregates for the old key. Save the new row for later and return advanced.
            _stashedNextRow = std::move(recoveredRow);
            return trackPlanState(PlanState::ADVANCED);
        }

        // Merge in the new partial aggregate values.
        _spilledAggRow = std::move(recoveredRow.second);
        for (size_t idx = 0; idx < _mergingExprCodes.size(); ++idx) {
            auto [owned, tag, val] = _bytecode.run(_mergingExprCodes[idx].get());
            _outRecordStoreAggAccessors[idx]->reset(owned, tag, val);
        }
    }

    return trackPlanState(PlanState::ADVANCED);
}

PlanState HashAggStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

    // If we've spilled, then we need to produce the output by merging the spilled segments from the
    // spill file.
    if (_recordStore) {
        return getNextSpilled();
    }

    // We didn't spill. Obtain the next output row from the hash table.
    if (_htIt == _ht->end()) {
        // First invocation of getNext() after open().
        if (!_seekKeysAccessors.empty()) {
            _htIt = _ht->find(_seekKeys);
        } else {
            _htIt = _ht->begin();
        }
    } else if (!_seekKeysAccessors.empty()) {
        // Subsequent invocation with seek keys. Return only 1 single row (if any).
        _htIt = _ht->end();
    } else {
        ++_htIt;
    }

    if (_htIt == _ht->end()) {
        // The hash table has been drained (and we never spilled to disk) so we're done.
        return trackPlanState(PlanState::IS_EOF);
    } else {
        return trackPlanState(PlanState::ADVANCED);
    }
}

std::unique_ptr<PlanStageStats> HashAggStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<HashAggStats>(_specificStats);

    if (includeDebugInfo) {
        DebugPrinter printer;
        BSONObjBuilder bob;
        bob.append("groupBySlots", _gbs.begin(), _gbs.end());
        if (!_aggs.empty()) {
            BSONObjBuilder exprBuilder(bob.subobjStart("expressions"));
            for (auto&& [slot, expr] : _aggs) {
                exprBuilder.append(str::stream() << slot, printer.print(expr.agg->debugPrint()));
            }

            BSONObjBuilder initExprBuilder(bob.subobjStart("initExprs"));
            for (auto&& [slot, expr] : _aggs) {
                if (expr.init) {
                    initExprBuilder.append(str::stream() << slot,
                                           printer.print(expr.init->debugPrint()));
                } else {
                    initExprBuilder.appendNull(str::stream() << slot);
                }
            }
        }

        if (!_mergingExprs.empty()) {
            BSONObjBuilder nestedBuilder{bob.subobjStart("mergingExprs")};
            for (auto&& [slot, expr] : _mergingExprs) {
                nestedBuilder.append(str::stream() << slot, printer.print(expr->debugPrint()));
            }
        }

        // Spilling stats.
        bob.appendBool("usedDisk", _specificStats.usedDisk);
        bob.appendNumber("spills", _specificStats.spills);
        bob.appendNumber("spilledBytes", _specificStats.spilledBytes);
        bob.appendNumber("spilledRecords", _specificStats.spilledRecords);
        bob.appendNumber("spilledDataStorageSize", _specificStats.spilledDataStorageSize);

        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* HashAggStage::getSpecificStats() const {
    return &_specificStats;
}

HashAggStats* HashAggStage::getHashAggStats() {
    return &_specificStats;
}

void HashAggStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _ht = boost::none;
    if (_recordStore && _opCtx) {
        _recordStore->resetCursor(_opCtx, _rsCursor);
    }
    _rsCursor.reset();
    _recordStore.reset();
    _outKeyRowRecordStore = {0};
    _outAggRowRecordStore = {0};
    _spilledAggRow = {0};
    _stashedNextRow = {0, 0};

    if (_childOpened) {
        _children[0]->close();
        _childOpened = false;
    }
}

std::vector<DebugPrinter::Block> HashAggStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _gbs.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _gbs[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    bool first = true;
    for (auto&& [slot, expr] : _aggs) {
        if (!first) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, slot);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, expr.agg->debugPrint());
        if (expr.init) {
            ret.emplace_back(DebugPrinter::Block("init{`"));
            DebugPrinter::addBlocks(ret, expr.init->debugPrint());
            ret.emplace_back(DebugPrinter::Block("`}"));
        }
        first = false;
    }
    ret.emplace_back("`]");

    if (!_seekKeysSlots.empty()) {
        ret.emplace_back("[`");
        for (size_t idx = 0; idx < _seekKeysSlots.size(); ++idx) {
            if (idx) {
                ret.emplace_back("`,");
            }

            DebugPrinter::addIdentifier(ret, _seekKeysSlots[idx]);
        }
        ret.emplace_back("`]");
    }

    if (!_mergingExprs.empty()) {
        ret.emplace_back("spillSlots[`");
        for (size_t idx = 0; idx < _mergingExprs.size(); ++idx) {
            if (idx) {
                ret.emplace_back("`,");
            }

            DebugPrinter::addIdentifier(ret, _mergingExprs[idx].first);
        }
        ret.emplace_back("`]");

        ret.emplace_back("mergingExprs[`");
        for (size_t idx = 0; idx < _mergingExprs.size(); ++idx) {
            if (idx) {
                ret.emplace_back("`,");
            }

            DebugPrinter::addBlocks(ret, _mergingExprs[idx].second->debugPrint());
        }
        ret.emplace_back("`]");
    }

    if (!_optimizedClose) {
        ret.emplace_back("reopen");
    }

    if (_collatorSlot) {
        DebugPrinter::addIdentifier(ret, *_collatorSlot);
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

size_t HashAggStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_gbs);
    size += size_estimator::estimate(_aggs);
    size += size_estimator::estimate(_seekKeysSlots);
    size += size_estimator::estimate(_mergingExprs);
    return size;
}
}  // namespace sbe
}  // namespace mongo
