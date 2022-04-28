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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/str.h"

#include "mongo/db/exec/sbe/size_estimator.h"

namespace mongo {
namespace sbe {
HashAggStage::HashAggStage(std::unique_ptr<PlanStage> input,
                           value::SlotVector gbs,
                           value::SlotMap<std::unique_ptr<EExpression>> aggs,
                           value::SlotVector seekKeysSlots,
                           bool optimizedClose,
                           boost::optional<value::SlotId> collatorSlot,
                           bool allowDiskUse,
                           PlanNodeId planNodeId)
    : PlanStage("group"_sd, planNodeId),
      _gbs(std::move(gbs)),
      _aggs(std::move(aggs)),
      _collatorSlot(collatorSlot),
      _allowDiskUse(allowDiskUse),
      _seekKeysSlots(std::move(seekKeysSlots)),
      _optimizedClose(optimizedClose) {
    _children.emplace_back(std::move(input));
    invariant(_seekKeysSlots.empty() || _seekKeysSlots.size() == _gbs.size());
    tassert(5843100,
            "HashAgg stage was given optimizedClose=false and seek keys",
            _seekKeysSlots.empty() || _optimizedClose);
}

std::unique_ptr<PlanStage> HashAggStage::clone() const {
    value::SlotMap<std::unique_ptr<EExpression>> aggs;
    for (auto& [k, v] : _aggs) {
        aggs.emplace(k, v->clone());
    }
    return std::make_unique<HashAggStage>(_children[0]->clone(),
                                          _gbs,
                                          std::move(aggs),
                                          _seekKeysSlots,
                                          _optimizedClose,
                                          _collatorSlot,
                                          _allowDiskUse,
                                          _commonStats.nodeId);
}

void HashAggStage::doSaveState(bool relinquishCursor) {
    if (relinquishCursor) {
        if (_rsCursor) {
            _rsCursor->save();
        }
    }
    if (_rsCursor) {
        _rsCursor->setSaveStorageCursorOnDetachFromOperationContext(!relinquishCursor);
    }
}

void HashAggStage::doRestoreState(bool relinquishCursor) {
    invariant(_opCtx);
    if (_rsCursor && relinquishCursor) {
        auto couldRestore = _rsCursor->restore();
        uassert(6196500, "HashAggStage could not restore cursor", couldRestore);
    }
}

void HashAggStage::doDetachFromOperationContext() {
    if (_rsCursor) {
        _rsCursor->detachFromOperationContext();
    }
}

void HashAggStage::doAttachToOperationContext(OperationContext* opCtx) {
    if (_rsCursor) {
        _rsCursor->reattachToOperationContext(opCtx);
    }
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
    size_t counter = 0;
    // Process group by columns.
    for (auto& slot : _gbs) {
        auto [it, inserted] = dupCheck.emplace(slot);
        uassert(4822827, str::stream() << "duplicate field: " << slot, inserted);

        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));

        // Construct accessors for the key to be processed from either the '_ht' or the
        // '_recordStore'. Before the memory limit is reached the '_outHashKeyAccessors' will carry
        // the group-by keys, otherwise the '_outRecordStoreKeyAccessors' will carry the group-by
        // keys.
        _outHashKeyAccessors.emplace_back(std::make_unique<HashKeyAccessor>(_htIt, counter));
        _outRecordStoreKeyAccessors.emplace_back(
            std::make_unique<value::MaterializedSingleRowAccessor>(_aggKeyRecordStore, counter));

        counter++;

        // A SwitchAccessor is used to point the '_outKeyAccessors' to the key coming from the '_ht'
        // or the '_recordStore' when draining the HashAgg stage in getNext. The group-by key will
        // either be in the '_ht' or the '_recordStore' if the key lives in memory, or if the key
        // has been spilled to disk, respectively. The SwitchAccessor allows toggling between the
        // two so the parent stage can read it through the '_outAccessors'.
        _outKeyAccessors.emplace_back(
            std::make_unique<value::SwitchAccessor>(std::vector<value::SlotAccessor*>{
                _outHashKeyAccessors.back().get(), _outRecordStoreKeyAccessors.back().get()}));

        _outAccessors[slot] = _outKeyAccessors.back().get();
    }

    // Process seek keys (if any). The keys must come from outside of the subtree (by definition) so
    // we go directly to the compilation context.
    for (auto& slot : _seekKeysSlots) {
        _seekKeysAccessors.emplace_back(ctx.getAccessor(slot));
    }

    counter = 0;
    for (auto& [slot, expr] : _aggs) {
        auto [it, inserted] = dupCheck.emplace(slot);
        // Some compilers do not allow to capture local bindings by lambda functions (the one
        // is used implicitly in uassert below), so we need a local variable to construct an
        // error message.
        const auto slotId = slot;
        uassert(4822828, str::stream() << "duplicate field: " << slotId, inserted);

        // Construct accessors for the agg state to be processed from either the '_ht' or the
        // '_recordStore' by the SwitchAccessor owned by '_outAggAccessors' below.
        _outRecordStoreAggAccessors.emplace_back(
            std::make_unique<value::MaterializedSingleRowAccessor>(_aggValueRecordStore, counter));
        _outHashAggAccessors.emplace_back(std::make_unique<HashAggAccessor>(_htIt, counter));
        counter++;

        // A SwitchAccessor is used to toggle the '_outAggAccessors' between the '_ht' and the
        // '_recordStore' when updating the agg state via the bytecode. By compiling the agg
        // EExpressions with a SwitchAccessor we can load the agg value into the memory of
        // '_aggValueRecordStore' if the value comes from the '_recordStore' or we can use the
        // agg value referenced through '_htIt' and run the bytecode to mutate the value through the
        // SwitchAccessor.
        _outAggAccessors.emplace_back(
            std::make_unique<value::SwitchAccessor>(std::vector<value::SlotAccessor*>{
                _outHashAggAccessors.back().get(), _outRecordStoreAggAccessors.back().get()}));

        _outAccessors[slot] = _outAggAccessors.back().get();

        ctx.root = this;
        ctx.aggExpression = true;
        ctx.accumulator = _outAggAccessors.back().get();

        _aggCodes.emplace_back(expr->compile(ctx));
        ctx.aggExpression = false;
    }
    _compiled = true;
}

value::SlotAccessor* HashAggStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compiled) {
        if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
            return it->second;
        }
    } else {
        return _children[0]->getAccessor(ctx, slot);
    }

    return ctx.getAccessor(slot);
}

void HashAggStage::makeTemporaryRecordStore() {
    tassert(
        5907500,
        "HashAggStage attempted to write to disk in an environment which is not prepared to do so",
        _opCtx->getServiceContext());
    tassert(5907501,
            "No storage engine so HashAggStage cannot spill to disk",
            _opCtx->getServiceContext()->getStorageEngine());
    assertIgnorePrepareConflictsBehavior(_opCtx);
    _recordStore = _opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(
        _opCtx, KeyFormat::String);

    _specificStats.usedDisk = true;
}

void HashAggStage::spillRowToDisk(const value::MaterializedRow& key,
                                  const value::MaterializedRow& val) {
    KeyString::Builder kb{KeyString::Version::kLatestVersion};
    key.serializeIntoKeyString(kb);
    auto typeBits = kb.getTypeBits();

    auto rid = RecordId(kb.getBuffer(), kb.getSize());
    boost::optional<value::MaterializedRow> valFromRs =
        readFromRecordStore(_opCtx, _recordStore->rs(), rid);
    tassert(6031100, "Spilling a row doesn't support updating it in the store.", !valFromRs);

    spillValueToDisk(rid, val, typeBits, false /*update*/);
}

void HashAggStage::spillValueToDisk(const RecordId& key,
                                    const value::MaterializedRow& val,
                                    const KeyString::TypeBits& typeBits,
                                    bool update) {
    auto nBytes = upsertToRecordStore(_opCtx, _recordStore->rs(), key, val, typeBits, update);
    if (!update) {
        _specificStats.spilledRecords++;
    }
    _specificStats.lastSpilledRecordSize = nBytes;
}

// Checks memory usage. Ideally, we'd want to know the exact size of already accumulated data, but
// we cannot, so we estimate it based on the last updated/inserted row, if we have one, or the first
// row in the '_ht' table. If the estimated memory usage exceeds the allowed, this method initiates
// spilling (if haven't been done yet) and evicts some records from the '_ht' table into the temp
// store to keep the memory usage under the limit.
void HashAggStage::checkMemoryUsageAndSpillIfNecessary(MemoryCheckData& mcd) {
    // The '_ht' table might become empty in the degenerate case when all rows had to be evicted to
    // meet the memory constraint during a previous check -- we don't need to keep checking memory
    // usage in this case because the table will never get new rows.
    if (_ht->empty()) {
        return;
    }

    // If the group-by key is empty we will only ever aggregate into a single row so no sense in
    // spilling since we will just be moving a single row back and forth from disk to main memory.
    if (_inKeyAccessors.size() == 0) {
        return;
    }

    mcd.memoryCheckpointCounter++;
    if (mcd.memoryCheckpointCounter >= mcd.nextMemoryCheckpoint) {
        if (_htIt == _ht->end()) {
            _htIt = _ht->begin();
        }
        const long estimatedRowSize =
            _htIt->first.memUsageForSorter() + _htIt->second.memUsageForSorter();
        long long estimatedTotalSize = _ht->size() * estimatedRowSize;
        const double estimatedGainPerChildAdvance =
            (static_cast<double>(estimatedTotalSize - mcd.lastEstimatedMemoryUsage) /
             mcd.memoryCheckpointCounter);

        if (estimatedTotalSize >= _approxMemoryUseInBytesBeforeSpill) {
            uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
                    "Exceeded memory limit for $group, but didn't allow external spilling."
                    " Pass allowDiskUse:true to opt in.",
                    _allowDiskUse);
            if (!_recordStore) {
                makeTemporaryRecordStore();
            }

            // Evict enough rows into the temporary store to drop below the memory constraint.
            const long rowsToEvictCount =
                1 + (estimatedTotalSize - _approxMemoryUseInBytesBeforeSpill) / estimatedRowSize;
            for (long i = 0; !_ht->empty() && i < rowsToEvictCount; i++) {
                spillRowToDisk(_htIt->first, _htIt->second);
                _ht->erase(_htIt);
                _htIt = _ht->begin();
            }
            estimatedTotalSize = _ht->size() * estimatedRowSize;
        }

        // Calculate the next memory checkpoint. We estimate it based on the prior growth of the
        // '_ht' and the remaining available memory. We have to keep doing this even after starting
        // to spill because some accumulators can grow in size inside '_ht' (with no bounds).
        // Value of 'estimatedGainPerChildAdvance' can be negative if the previous checkpoint
        // evicted any records. And a value close to zero indicates a stable size of '_ht' so can
        // delay the next check progressively.
        const long nextCheckpointCandidate = (estimatedGainPerChildAdvance > 0.1)
            ? mcd.checkpointMargin * (_approxMemoryUseInBytesBeforeSpill - estimatedTotalSize) /
                estimatedGainPerChildAdvance
            : (estimatedGainPerChildAdvance < -0.1) ? mcd.atMostCheckFrequency
                                                    : mcd.nextMemoryCheckpoint * 2;
        mcd.nextMemoryCheckpoint =
            std::min<long>(mcd.memoryCheckFrequency,
                           std::max<long>(mcd.atMostCheckFrequency, nextCheckpointCandidate));

        mcd.lastEstimatedMemoryUsage = estimatedTotalSize;
        mcd.memoryCheckpointCounter = 0;
        mcd.memoryCheckFrequency =
            std::min<long>(mcd.memoryCheckFrequency * 2, mcd.atLeastMemoryCheckFrequency);
    }
}

void HashAggStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;

    if (!reOpen || _seekKeysAccessors.empty()) {
        _children[0]->open(_childOpened);
        _childOpened = true;
        if (_collatorAccessor) {
            auto [tag, collatorVal] = _collatorAccessor->getViewOfValue();
            uassert(
                5402503, "collatorSlot must be of collator type", tag == value::TypeTags::collator);
            auto collatorView = value::getCollatorView(collatorVal);
            const value::MaterializedRowHasher hasher(collatorView);
            const value::MaterializedRowEq equator(collatorView);
            _ht.emplace(0, hasher, equator);
        } else {
            _ht.emplace();
        }

        _seekKeys.resize(_seekKeysAccessors.size());

        // A default value for spilling a key to the record store.
        value::MaterializedRow defaultVal{_outAggAccessors.size()};
        bool updateAggStateHt = false;
        MemoryCheckData memoryCheckData;

        while (_children[0]->getNext() == PlanState::ADVANCED) {
            value::MaterializedRow key{_inKeyAccessors.size()};
            // Copy keys in order to do the lookup.
            size_t idx = 0;
            for (auto& p : _inKeyAccessors) {
                auto [tag, val] = p->getViewOfValue();
                key.reset(idx++, false, tag, val);
            }

            if (!_recordStore) {
                // The memory limit hasn't been reached yet, accumulate state in '_ht'.
                auto [it, inserted] = _ht->try_emplace(std::move(key), value::MaterializedRow{0});
                if (inserted) {
                    // Copy keys.
                    const_cast<value::MaterializedRow&>(it->first).makeOwned();
                    // Initialize accumulators.
                    it->second.resize(_outAggAccessors.size());
                }
                // Always update the state in the '_ht' for the branch when data hasn't been
                // spilled to disk.
                _htIt = it;
                updateAggStateHt = true;
            } else {
                // The memory limit has been reached, accumulate state in '_ht' only if we
                // find the key in '_ht'.
                _htIt = _ht->find(key);
                updateAggStateHt = _htIt != _ht->end();
            }

            if (updateAggStateHt) {
                // Accumulate state in '_ht' by pointing the '_outAggAccessors' the
                // '_outHashAggAccessors'.
                for (size_t idx = 0; idx < _outAggAccessors.size(); ++idx) {
                    _outAggAccessors[idx]->setIndex(0);
                    auto [owned, tag, val] = _bytecode.run(_aggCodes[idx].get());
                    _outHashAggAccessors[idx]->reset(owned, tag, val);
                }
            } else {
                // The memory limit has been reached and the key wasn't in the '_ht' so we need
                // to spill it to the '_recordStore'.
                KeyString::Builder kb{KeyString::Version::kLatestVersion};
                // 'key' is moved only when 'updateAggStateHt' ends up "true", so it's safe to
                // ignore the warning.
                key.serializeIntoKeyString(kb);  // NOLINT(bugprone-use-after-move)
                auto typeBits = kb.getTypeBits();

                auto rid = RecordId(kb.getBuffer(), kb.getSize());

                boost::optional<value::MaterializedRow> valFromRs =
                    readFromRecordStore(_opCtx, _recordStore->rs(), rid);
                if (!valFromRs) {
                    _aggValueRecordStore = defaultVal;
                } else {
                    _aggValueRecordStore = *valFromRs;
                }

                for (size_t idx = 0; idx < _outAggAccessors.size(); ++idx) {
                    _outAggAccessors[idx]->setIndex(1);
                    auto [owned, tag, val] = _bytecode.run(_aggCodes[idx].get());
                    _aggValueRecordStore.reset(idx, owned, tag, val);
                }
                spillValueToDisk(rid, _aggValueRecordStore, typeBits, valFromRs ? true : false);
            }

            // Estimates how much memory is being used and might start spilling.
            checkMemoryUsageAndSpillIfNecessary(memoryCheckData);

            if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumResults>(1)) {
                // During trial runs, we want to limit the amount of work done by opening a blocking
                // stage, like this one. The blocking stage tracks the number of documents it has
                // read from its child, and if the TrialRunTracker ends the trial, a special
                // exception returns control back to the planner.
                _tracker = nullptr;
                _children[0]->close();
                uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit in group");
            }
        }

        if (_optimizedClose) {
            _children[0]->close();
            _childOpened = false;
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

    // Set the SwitchAccessors to point to the '_ht' so we can drain it first before draining the
    // '_recordStore' in getNext().
    for (size_t idx = 0; idx < _outAggAccessors.size(); ++idx) {
        _outAggAccessors[idx]->setIndex(0);
    }
    _drainingRecordStore = false;
}

PlanState HashAggStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (_htIt == _ht->end() && !_drainingRecordStore) {
        // First invocation of getNext() after open() when not draining the '_recordStore'.
        if (!_seekKeysAccessors.empty()) {
            _htIt = _ht->find(_seekKeys);
        } else {
            _htIt = _ht->begin();
        }
    } else if (!_seekKeysAccessors.empty()) {
        // Subsequent invocation with seek keys. Return only 1 single row (if any).
        _htIt = _ht->end();
    } else if (!_drainingRecordStore) {
        // Returning the results of the entire hash table first before draining the '_recordStore'.
        ++_htIt;
    }

    if (_htIt == _ht->end() && !_recordStore) {
        // The hash table has been drained and nothing was spilled to disk.
        return trackPlanState(PlanState::IS_EOF);
    } else if (_htIt != _ht->end()) {
        // Drain the '_ht' on the next 'getNext()' call.
        return trackPlanState(PlanState::ADVANCED);
    } else if (_seekKeysAccessors.empty()) {
        // A record store was created to spill to disk. Drain it then clean it up.
        if (!_rsCursor) {
            _rsCursor = _recordStore->rs()->getCursor(_opCtx);
        }
        auto nextRecord = _rsCursor->next();
        if (nextRecord) {
            // Point the out accessors to the recordStore accessors to allow parent stages to read
            // the agg state from the '_recordStore'.
            if (!_drainingRecordStore) {
                for (size_t i = 0; i < _outKeyAccessors.size(); ++i) {
                    _outKeyAccessors[i]->setIndex(1);
                }
                for (size_t i = 0; i < _outAggAccessors.size(); ++i) {
                    _outAggAccessors[i]->setIndex(1);
                }
            }
            _drainingRecordStore = true;

            // Read the agg state value from the '_recordStore' and Reconstruct the key from the
            // typeBits stored along side of the value.
            BufReader valReader(nextRecord->data.data(), nextRecord->data.size());
            auto val = value::MaterializedRow::deserializeForSorter(valReader, {});
            auto typeBits =
                KeyString::TypeBits::fromBuffer(KeyString::Version::kLatestVersion, &valReader);
            _aggValueRecordStore = val;

            BufBuilder buf;
            _aggKeyRecordStore = value::MaterializedRow::deserializeFromKeyString(
                decodeKeyString(nextRecord->id, typeBits), &buf);
            return trackPlanState(PlanState::ADVANCED);
        } else {
            _rsCursor.reset();
            _recordStore.reset();
            return trackPlanState(PlanState::IS_EOF);
        }
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
            BSONObjBuilder childrenBob(bob.subobjStart("expressions"));
            for (auto&& [slot, expr] : _aggs) {
                childrenBob.append(str::stream() << slot, printer.print(expr->debugPrint()));
            }
        }
        // Spilling stats.
        bob.appendBool("usedDisk", _specificStats.usedDisk);
        bob.appendNumber("spilledRecords", _specificStats.spilledRecords);
        bob.appendNumber("spilledBytesApprox",
                         _specificStats.lastSpilledRecordSize * _specificStats.spilledRecords);

        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* HashAggStage::getSpecificStats() const {
    return &_specificStats;
}

void HashAggStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _ht = boost::none;
    if (_recordStore) {
        // A record store was created to spill to disk. Clean it up.
        _recordStore.reset();
        _drainingRecordStore = false;
    }

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
    value::orderedSlotMapTraverse(_aggs, [&](auto slot, auto&& expr) {
        if (!first) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, slot);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, expr->debugPrint());
        first = false;
    });
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
    return size;
}

void HashAggStage::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

PlanStage::TrialRunTrackerAttachResultMask HashAggStage::doAttachToTrialRunTracker(
    TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
    // The HashAggStage only tracks the "numResults" metric when it is the most deeply nested
    // blocking stage.
    if (!(childrenAttachResult & TrialRunTrackerAttachResultFlags::AttachedToBlockingStage)) {
        _tracker = tracker;
    }

    // Return true to indicate that the tracker is attached to a blocking stage: either this stage
    // or one of its descendent stages.
    return childrenAttachResult | TrialRunTrackerAttachResultFlags::AttachedToBlockingStage;
}

}  // namespace sbe
}  // namespace mongo
