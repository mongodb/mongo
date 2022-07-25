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

#include "mongo/platform/basic.h"

#include "mongo/db/curop.h"
#include "mongo/db/exec/sbe/stages/hash_lookup.h"
#include "mongo/db/exec/sbe/stages/stage_visitors.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {

HashLookupStage::HashLookupStage(std::unique_ptr<PlanStage> outer,
                                 std::unique_ptr<PlanStage> inner,
                                 value::SlotId outerCond,
                                 value::SlotId innerCond,
                                 value::SlotVector innerProjects,
                                 value::SlotMap<std::unique_ptr<EExpression>> innerAggs,
                                 boost::optional<value::SlotId> collatorSlot,
                                 PlanNodeId planNodeId,
                                 bool participateInTrialRunTracking)
    : PlanStage("hash_lookup"_sd, planNodeId, participateInTrialRunTracking),
      _outerCond(outerCond),
      _innerCond(innerCond),
      _innerProjects(innerProjects),
      _innerAggs(std::move(innerAggs)),
      _collatorSlot(collatorSlot),
      _probeKey(0) {
    _children.emplace_back(std::move(outer));
    _children.emplace_back(std::move(inner));
}

std::unique_ptr<PlanStage> HashLookupStage::clone() const {
    value::SlotMap<std::unique_ptr<EExpression>> innerAggs;
    for (auto& [k, v] : _innerAggs) {
        innerAggs.emplace(k, v->clone());
    }

    return std::make_unique<HashLookupStage>(outerChild()->clone(),
                                             innerChild()->clone(),
                                             _outerCond,
                                             _innerCond,
                                             _innerProjects,
                                             std::move(innerAggs),
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
        _collator = value::getCollatorView(collatorVal);
    }

    value::SlotSet inputSlots;
    value::SlotSet resultSlots;

    auto slot = _outerCond;
    inputSlots.emplace(slot);
    _inOuterMatchAccessor = outerChild()->getAccessor(ctx, slot);

    slot = _innerCond;
    inputSlots.emplace(slot);
    _inInnerMatchAccessor = innerChild()->getAccessor(ctx, slot);

    size_t idx = 0;
    value::SlotSet innerProjectDupCheck;
    _outInnerProjectAccessors.reserve(_innerProjects.size());
    _outInnerBufferProjectAccessors.reserve(_innerProjects.size());
    _outInnerBufValueRecordStoreAccessors.reserve(_innerProjects.size());
    for (auto slot : _innerProjects) {
        inputSlots.emplace(slot);
        auto [it, inserted] = innerProjectDupCheck.emplace(slot);
        tassert(6367802, str::stream() << "duplicate inner project field: " << slot, inserted);

        auto accessor = innerChild()->getAccessor(ctx, slot);
        _inInnerProjectAccessors.push_back(accessor);

        _outInnerBufferProjectAccessors.emplace_back(_buffer, _bufferIt, idx);
        _outInnerBufValueRecordStoreAccessors.push_back(
            value::MaterializedSingleRowAccessor(_bufValueRecordStore, idx));

        // Use a switch accessor to feed the buffered value from the '_buffer' or the value spilled
        // to '_recordStoreBuf'.
        _outInnerProjectAccessors.push_back(value::SwitchAccessor(
            std::vector<value::SlotAccessor*>{&_outInnerBufferProjectAccessors.back(),
                                              &_outInnerBufValueRecordStoreAccessors.back()}));

        // '_outInnerProjectAccessors' has been preallocated, so it's element pointers will be
        // stable.
        _outInnerProjectAccessorMap.emplace(slot, &_outInnerProjectAccessors.back());
        idx++;
    }

    idx = 0;
    _outResultAggAccessors.reserve(_innerAggs.size());
    for (auto& [slot, expr] : _innerAggs) {
        auto [it, inserted] = resultSlots.emplace(slot);
        // Some compilers do not allow to capture local bindings by lambda functions (the one
        // is used implicitly in tassert below), so we need a local variable to construct an
        // error message.
        auto& slotId = slot;
        tassert(6367803, str::stream() << "duplicate inner agg field: " << slotId, inserted);

        // Construct accessors for the agg state to be returned.
        _outResultAggAccessors.emplace_back(_resultAggRow, idx);

        // '_outResultAggAccessors' has been preallocated, so it's element pointers will be stable.
        _outAccessorMap[slot] = &_outResultAggAccessors.back();

        // Set '_compileInnerAgg' to make only '_outInnerProjectAccessorMap' visible when compiling
        // the expression.
        _compileInnerAgg = true;
        ctx.root = this;
        ctx.aggExpression = true;
        ctx.accumulator = &_outResultAggAccessors.back();
        _aggCodes.emplace_back(expr->compile(ctx));
        ctx.aggExpression = false;
        _compileInnerAgg = false;

        idx++;
    }

    for (auto slot : resultSlots) {
        tassert(6367804,
                str::stream() << "conflicting input and result field: " << slot,
                !inputSlots.contains(slot));
    }

    _resultAggRow.resize(_outResultAggAccessors.size());
    _probeKey.resize(1);
}

value::SlotAccessor* HashLookupStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compileInnerAgg) {
        if (auto it = _outInnerProjectAccessorMap.find(slot);
            it != _outInnerProjectAccessorMap.end()) {
            return it->second;
        }

        return ctx.getAccessor(slot);
    } else {
        if (auto it = _outAccessorMap.find(slot); it != _outAccessorMap.end()) {
            return it->second;
        }

        return outerChild()->getAccessor(ctx, slot);
    }
}

void HashLookupStage::reset() {
    _ht = boost::none;

    // Reset the memory threshold if the knob changes between re-open calls.
    _memoryUseInBytesBeforeSpill = internalQuerySBELookupApproxMemoryUseInBytesBeforeSpill.load();

    if (_recordStoreHt) {
        _recordStoreHt.reset(nullptr);
    }
    if (_recordStoreBuf) {
        _recordStoreBuf.reset(nullptr);
    }

    // Erase but don't change its reference. Otherwise it will invalidate the slot accessors.
    _buffer.clear();
    _valueId = 0;
    _bufferIt = 0;
}

std::pair<RecordId, KeyString::TypeBits> HashLookupStage::serializeKeyForRecordStore(
    const value::MaterializedRow& key) const {
    KeyString::Builder kb{KeyString::Version::kLatestVersion};
    return encodeKeyString(kb, key);
}

std::tuple<bool, value::TypeTags, value::Value> HashLookupStage::normalizeStringIfCollator(
    value::TypeTags tag, value::Value val) {
    if (value::isString(tag) && _collatorSlot) {
        auto [tagColl, valColl] = value::makeNewString(
            _collator->getComparisonKey(value::getStringView(tag, val)).getKeyData());
        return {true, tagColl, valColl};
    }
    return {false, tag, val};
}

void HashLookupStage::addHashTableEntry(value::SlotAccessor* keyAccessor, size_t valueIndex) {
    // Adds a new key-value entry. Will attempt to move or copy from key accessor when needed.
    // array case each elem in array we put each element into ht.
    auto [tagKeyView, valKeyView] = keyAccessor->getViewOfValue();
    _probeKey.reset(0, false, tagKeyView, valKeyView);

    // Check to see if key is already in memory. If not, we will emplace a new key or spill to disk.
    auto htIt = _ht->find(_probeKey);
    if (htIt == _ht->end()) {
        // If the key and one 'size_t' index fit into the '_ht' without reaching the memory limit
        // and we haven't spilled yet emplace into '_ht'. Otherwise, we will always spill the key to
        // the record store. The additional guard !hasSpilledHtToDisk() ensures that a key that is
        // evicted from '_ht' never ends in '_ht' again.
        const long long newMemUsage = _computedTotalMemUsage +
            size_estimator::estimate(tagKeyView, valKeyView) + sizeof(size_t);

        value::MaterializedRow key{1};
        if (!hasSpilledHtToDisk() && newMemUsage <= _memoryUseInBytesBeforeSpill) {
            // We have to insert an owned key, attempt a move, but force copy if necessary when we
            // haven't spilled to the '_recordStore' yet.
            auto [tagKey, valKey] = keyAccessor->copyOrMoveValue();
            key.reset(0, true, tagKey, valKey);

            auto [it, inserted] = _ht->try_emplace(std::move(key));
            invariant(inserted);
            htIt = it;
            htIt->second.push_back(valueIndex);
            _computedTotalMemUsage = newMemUsage;
        } else {
            // Write record to rs.
            if (!hasSpilledHtToDisk()) {
                makeTemporaryRecordStore();
            }

            auto val = std::vector<size_t>{valueIndex};
            auto [tagKey, valKey] = keyAccessor->getViewOfValue();
            spillIndicesToRecordStore(_recordStoreHt->rs(), tagKey, valKey, val);
        }
    } else {
        // The key is already present in '_ht' so the memory will only grow by one size_t. If we
        // reach the memory limit, the key/value in '_ht' will be evicted from memory and spilled to
        // '_recordStoreHt' along with the new index.
        const long long newMemUsage = _computedTotalMemUsage + sizeof(size_t);
        if (newMemUsage <= _memoryUseInBytesBeforeSpill) {
            htIt->second.push_back(valueIndex);
            _computedTotalMemUsage = newMemUsage;
        } else {
            if (!hasSpilledHtToDisk()) {
                makeTemporaryRecordStore();
            }

            value::MaterializedRow key{1};
            key.reset(0, true, tagKeyView, valKeyView);
            _computedTotalMemUsage -= size_estimator::estimate(tagKeyView, valKeyView);

            // Evict the hash table value.
            _computedTotalMemUsage -= htIt->second.size() * sizeof(size_t);
            htIt->second.push_back(valueIndex);
            spillIndicesToRecordStore(_recordStoreHt->rs(), tagKeyView, valKeyView, htIt->second);
            _ht->erase(htIt);
        }
    }
}

void HashLookupStage::makeTemporaryRecordStore() {
    tassert(6373901,
            "HashLookupStage attempted to write to disk in an environment which is not prepared to "
            "do so",
            _opCtx->getServiceContext());
    tassert(6373902,
            "No storage engine so HashLookupStage cannot spill to disk",
            _opCtx->getServiceContext()->getStorageEngine());
    assertIgnorePrepareConflictsBehavior(_opCtx);

    _recordStoreBuf = _opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(
        _opCtx, KeyFormat::Long);

    _recordStoreHt = _opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(
        _opCtx, KeyFormat::String);

    _specificStats.usedDisk = true;
}

void HashLookupStage::spillBufferedValueToDisk(OperationContext* opCtx,
                                               RecordStore* rs,
                                               size_t bufferIdx,
                                               const value::MaterializedRow& val) {
    CurOp::get(_opCtx)->debug().hashLookupSpillToDisk += 1;

    auto rid = getValueRecordId(bufferIdx);

    BufBuilder buf;
    val.serializeForSorter(buf);

    assertIgnorePrepareConflictsBehavior(opCtx);
    WriteUnitOfWork wuow(opCtx);

    auto status = rs->insertRecord(opCtx, rid, buf.buf(), buf.len(), Timestamp{});
    wuow.commit();

    tassert(6373906,
            str::stream() << "Failed to write to disk because " << status.getStatus().reason(),
            status.isOK());

    _specificStats.spilledBuffRecords++;
    // Add size of record ID + size of buffer.
    _specificStats.spilledBuffBytesOverAllRecords += sizeof(size_t) + buf.len();
    return;
}

size_t HashLookupStage::bufferValueOrSpill(value::MaterializedRow& value) {
    size_t bufferIndex = _valueId;
    const long long newMemUsage = _computedTotalMemUsage + size_estimator::estimate(value);
    if (newMemUsage <= _memoryUseInBytesBeforeSpill) {
        _buffer.emplace_back(std::move(value));
        _computedTotalMemUsage = newMemUsage;
    } else {
        if (!hasSpilledBufToDisk()) {
            makeTemporaryRecordStore();
        }
        spillBufferedValueToDisk(_opCtx, _recordStoreBuf->rs(), bufferIndex, value);
    }
    _valueId++;
    return bufferIndex;
}

void HashLookupStage::setInnerProjectSwitchAccessor(int newIdx) {
    if (newIdx != _currentSwitchIdx) {
        for (size_t idx = 0; idx < _outInnerProjectAccessors.size(); idx++) {
            _outInnerProjectAccessors[idx].setIndex(newIdx);
        }
        _currentSwitchIdx = newIdx;
    }
}

void HashLookupStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    if (reOpen) {
        reset();
    }

    _commonStats.opens++;
    if (_collatorAccessor) {
        auto [tag, collatorVal] = _collatorAccessor->getViewOfValue();
        tassert(6367810, "collatorSlot must be of collator type", tag == value::TypeTags::collator);
        auto collatorView = value::getCollatorView(collatorVal);
        const value::MaterializedRowHasher hasher(collatorView);
        const value::MaterializedRowEq equator(collatorView);
        _ht.emplace(0, hasher, equator);
    } else {
        _ht.emplace();
    }

    innerChild()->open(false);

    // Insert the inner side into the hash table.
    while (innerChild()->getNext() == PlanState::ADVANCED) {
        value::MaterializedRow value{_inInnerProjectAccessors.size()};

        // Copy all projected values.
        size_t idx = 0;
        for (auto accessor : _inInnerProjectAccessors) {
            auto [tag, val] = accessor->copyOrMoveValue();
            value.reset(idx++, true, tag, val);
        }

        // This where we put the value in here. This can grow need to spill.
        auto bufferIndex = bufferValueOrSpill(value);

        auto [tagKeyView, valKeyView] = _inInnerMatchAccessor->getViewOfValue();
        if (value::isArray(tagKeyView)) {
            value::ArrayAccessor arrayAccessor;
            arrayAccessor.reset(_inInnerMatchAccessor);

            while (!arrayAccessor.atEnd()) {
                addHashTableEntry(&arrayAccessor, bufferIndex);
                arrayAccessor.advance();
            }
        } else {
            addHashTableEntry(_inInnerMatchAccessor, bufferIndex);
        }
    }

    innerChild()->close();
    outerChild()->open(reOpen);
}

template <typename C>
void HashLookupStage::accumulateFromValueIndices(const C& bufferIndices) {
    boost::optional<size_t> prevIdx;
    for (auto bufferIdx : bufferIndices) {
        tassert(6367811, "Indices expected to be sorted", !prevIdx || prevIdx < bufferIdx);

        _bufferIt = bufferIdx;
        // Point iterator to a row to accumulate.
        if (_buffer.size() > 0 && _bufferIt < _buffer.size()) {
            setInnerProjectSwitchAccessor(0);
        } else {
            // Point the _outInnerProjectAccessors to the accessor for the value from
            // '_recordStoreBuf' since we need to read a spilled value.
            setInnerProjectSwitchAccessor(1);

            // We must shift the '_bufferIt' index by one when using it as a RecordId because a
            // RecordId of 0 is invalid.
            auto rid = getValueRecordId(_bufferIt);
            auto rsValue = readFromRecordStore(_opCtx, _recordStoreBuf->rs(), rid);
            if (!rsValue) {
                tasserted(6373900, "bufferIdx not found in record store");
            }
            _bufValueRecordStore = *rsValue;
        }

        for (size_t idx = 0; idx < _outResultAggAccessors.size(); idx++) {
            auto [owned, tag, val] = _bytecode.run(_aggCodes[idx].get());
            _resultAggRow.reset(idx, owned, tag, val);
        }

        prevIdx = bufferIdx;
    }
}

void HashLookupStage::writeIndicesToRecordStore(RecordStore* rs,
                                                value::TypeTags tagKey,
                                                value::Value valKey,
                                                const std::vector<size_t>& value,
                                                bool update) {
    BufBuilder buf;
    buf.appendNum(value.size());  // number of indices
    for (auto& idx : value) {
        buf.appendNum(static_cast<size_t>(idx));
    }

    value::MaterializedRow key{1};
    key.reset(0, false, tagKey, valKey);
    auto [rid, typeBits] = serializeKeyForRecordStore(key);

    upsertToRecordStore(_opCtx, rs, rid, buf, typeBits, update);
    if (!update) {
        _specificStats.spilledHtRecords++;
        // Add the size of key (which comprises of the memory usage for the key + its type bits),
        // as well as the size of one integer to store the length of indices vector in the value.
        _specificStats.spilledHtBytesOverAllRecords +=
            rid.memUsage() + typeBits.getSize() + sizeof(size_t);
    }
    // Add the size of indices vector used in the hash-table value to the accounting.
    _specificStats.spilledHtBytesOverAllRecords += value.size() * sizeof(size_t);
}

boost::optional<std::vector<size_t>> HashLookupStage::readIndicesFromRecordStore(
    RecordStore* rs, value::TypeTags tagKey, value::Value valKey) {
    _probeKey.reset(0, false, tagKey, valKey);

    auto [rid, _] = serializeKeyForRecordStore(_probeKey);
    RecordData record;
    if (rs->findRecord(_opCtx, rid, &record)) {
        // 'BufBuilder' writes numbers in little endian format, so must read them using the same.
        auto valueReader = BufReader(record.data(), record.size());
        auto nRecords = valueReader.read<LittleEndian<size_t>>();
        std::vector<size_t> result(nRecords);
        for (size_t i = 0; i < nRecords; ++i) {
            auto idx = valueReader.read<LittleEndian<size_t>>();
            result[i] = idx;
        }
        return result;
    }
    return boost::none;
}

void HashLookupStage::spillIndicesToRecordStore(RecordStore* rs,
                                                value::TypeTags tagKey,
                                                value::Value valKey,
                                                const std::vector<size_t>& value) {
    CurOp::get(_opCtx)->debug().hashLookupSpillToDisk += 1;

    auto [owned, tagKeyColl, valKeyColl] = normalizeStringIfCollator(tagKey, valKey);
    _probeKey.reset(0, owned, tagKeyColl, valKeyColl);

    auto valFromRs = readIndicesFromRecordStore(rs, tagKeyColl, valKeyColl);

    auto update = false;
    if (valFromRs) {
        valFromRs->insert(valFromRs->end(), value.begin(), value.end());
        update = true;
        // As we're updating these records, we'd remove the old size from the accounting. The new
        // size is added back to the accounting in the call to 'writeIndicesToRecordStore' below.
        _specificStats.spilledHtBytesOverAllRecords -= value.size();
    } else {
        valFromRs = value;
    }

    writeIndicesToRecordStore(rs, tagKeyColl, valKeyColl, *valFromRs, update);
}

PlanState HashLookupStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    auto state = outerChild()->getNext();
    if (state == PlanState::ADVANCED) {
        // Clear the result accumulators.
        for (size_t idx = 0; idx < _outResultAggAccessors.size(); idx++) {
            _resultAggRow.reset(0, false, value::TypeTags::Nothing, 0);
        }
        auto [tagKeyView, valKeyView] = _inOuterMatchAccessor->getViewOfValue();
        if (value::isArray(tagKeyView)) {
            // We are using sorted set, so that fetching by index is monotonic.
            // It also provides a deterministic execution, that is unrelated to a hash function
            // chosen. This way of constructing union is not optimal in the worst case O(INPUT *
            // log INPUT), where INPUT is total count of input indices to union. This could be
            // improved to O(OUTPUT * log OUTPUT), by using hash set and sorting later. Hovewer
            // its not obvious which approach will be acutally better in real world scenarios.
            std::set<size_t> indices;
            value::ArrayEnumerator enumerator(tagKeyView, valKeyView);
            while (!enumerator.atEnd()) {
                auto [tagElemView, valElemView] = enumerator.getViewOfValue();
                _probeKey.reset(0, false, tagElemView, valElemView);
                auto htIt = _ht->find(_probeKey);
                if (htIt != _ht->end()) {
                    indices.insert(htIt->second.begin(), htIt->second.end());
                } else if (_recordStoreHt) {
                    // The key wasn't in memory and we have spilled to a '_recordStoreHt', fetch it
                    // if it exists.
                    auto [_, tagElemCollView, valElemCollView] =
                        normalizeStringIfCollator(tagElemView, valElemView);

                    auto indicesFromRS = readIndicesFromRecordStore(
                        _recordStoreHt->rs(), tagElemCollView, valElemCollView);
                    if (indicesFromRS) {
                        indices.insert(indicesFromRS->begin(), indicesFromRS->end());
                    }
                }
                enumerator.advance();
            }
            accumulateFromValueIndices(indices);
        } else {
            _probeKey.reset(0, false, tagKeyView, valKeyView);
            auto htIt = _ht->find(_probeKey);
            if (htIt != _ht->end()) {
                accumulateFromValueIndices(htIt->second);
            } else if (_recordStoreHt) {
                // Need to make sure we have spilled by checking if the '_recordStoreHt' is
                // non-nullptr if we don't find the '_probeKey' in the '_ht'. Otherwise, the empty
                // foreign side edge case won't fallthrough and we may hit this block and try to
                // read from a non-existent '_recordStoreHt'.
                auto [_, tagKeyCollView, valKeyCollView] =
                    normalizeStringIfCollator(tagKeyView, valKeyView);

                auto indicesFromRS = readIndicesFromRecordStore(
                    _recordStoreHt->rs(), tagKeyCollView, valKeyCollView);
                if (indicesFromRS) {
                    accumulateFromValueIndices(*indicesFromRS);
                }
            }
        }
    }

    return trackPlanState(state);
}

void HashLookupStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();

    outerChild()->close();
    reset();

    _buffer.shrink_to_fit();
}

std::unique_ptr<PlanStageStats> HashLookupStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    invariant(ret);
    ret->children.emplace_back(outerChild()->getStats(includeDebugInfo));
    ret->children.emplace_back(innerChild()->getStats(includeDebugInfo));
    ret->specific = std::make_unique<HashLookupStats>(_specificStats);
    if (includeDebugInfo) {
        BSONObjBuilder bob(StorageAccessStatsVisitor::collectStats(*this, *ret).toBSON());
        // Spilling stats.
        bob.appendBool("usedDisk", _specificStats.usedDisk)
            .appendNumber("spilledRecords", _specificStats.getSpilledRecords())
            .appendNumber("spilledBytesApprox", _specificStats.getSpilledBytesApprox());
        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* HashLookupStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> HashLookupStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("[`"));
    bool first = true;
    value::orderedSlotMapTraverse(_innerAggs, [&](auto slot, auto&& expr) {
        if (!first) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, slot);
        ret.emplace_back("=");
        DebugPrinter::addBlocks(ret, expr->debugPrint());
        first = false;
    });
    ret.emplace_back("`]");

    if (_collatorSlot) {
        DebugPrinter::addIdentifier(ret, *_collatorSlot);
    }

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);

    DebugPrinter::addKeyword(ret, "outer");
    DebugPrinter::addIdentifier(ret, _outerCond);
    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, outerChild()->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    DebugPrinter::addKeyword(ret, "inner");
    DebugPrinter::addIdentifier(ret, _innerCond);

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _innerProjects.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _innerProjects[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, innerChild()->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    return ret;
}

size_t HashLookupStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_innerProjects);
    size += size_estimator::estimate(_innerAggs);
    return size;
}
}  // namespace sbe
}  // namespace mongo
