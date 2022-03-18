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

#include "mongo/db/exec/sbe/stages/hash_lookup.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
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
                                 PlanNodeId planNodeId)
    : PlanStage("hash_lookup"_sd, planNodeId),
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
                                             _commonStats.nodeId);
}

void HashLookupStage::prepare(CompileCtx& ctx) {
    outerChild()->prepare(ctx);
    innerChild()->prepare(ctx);

    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(6367801,
                "collator accessor should exist if collator slot provided to HashJoinStage",
                _collatorAccessor != nullptr);
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
    for (auto slot : _innerProjects) {
        inputSlots.emplace(slot);
        auto [it, inserted] = innerProjectDupCheck.emplace(slot);
        tassert(6367802, str::stream() << "duplicate inner project field: " << slot, inserted);

        auto accessor = innerChild()->getAccessor(ctx, slot);
        _inInnerProjectAccessors.push_back(accessor);

        _outInnerProjectAccessors.emplace_back(_buffer, _bufferIt, idx);

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

    // Erase but don't change its reference. Otherwise it will invalidate the slot accessors.
    _buffer.clear();
}

void HashLookupStage::addHashTableEntry(value::SlotAccessor* keyAccessor, size_t valueIndex) {
    // Adds a new key-value entry. Will attempt to move or copy from key accessor when needed.
    auto [tagKeyView, valKeyView] = keyAccessor->getViewOfValue();
    _probeKey.reset(0, false, tagKeyView, valKeyView);

    auto htIt = _ht->find(_probeKey);
    if (htIt == _ht->end()) {
        // We have to insert an owned key, attempt a move, but force copy if necessary.
        value::MaterializedRow key{1};
        auto [tagKey, valKey] = keyAccessor->copyOrMoveValue();
        key.reset(0, true, tagKey, valKey);
        auto [it, inserted] = _ht->try_emplace(std::move(key));
        invariant(inserted);
        htIt = it;
    }

    htIt->second.push_back(valueIndex);
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

        size_t bufferIndex = _buffer.size();
        _buffer.emplace_back(std::move(value));

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

        // Point iterator to a row to accumulate.
        _bufferIt = bufferIdx;

        for (size_t idx = 0; idx < _outResultAggAccessors.size(); idx++) {
            auto [owned, tag, val] = _bytecode.run(_aggCodes[idx].get());
            _resultAggRow.reset(idx, owned, tag, val);
        }

        prevIdx = bufferIdx;
    }
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
            // chosen. This way of constructing union is not optimal in the worst case O(INPUT * log
            // INPUT), where INPUT is total count of input indices to union. This could be improved
            // to O(OUTPUT * log OUTPUT), by using hash set and sorting later. Hovewer its not
            // obvious which approach will be acutally better in real world scenarios.
            std::set<size_t> indices;
            value::ArrayEnumerator enumerator(tagKeyView, valKeyView);
            while (!enumerator.atEnd()) {
                auto [tagElemView, valElemView] = enumerator.getViewOfValue();
                _probeKey.reset(0, false, tagElemView, valElemView);
                auto htIt = _ht->find(_probeKey);
                if (htIt != _ht->end()) {
                    indices.insert(htIt->second.begin(), htIt->second.end());
                }
                enumerator.advance();
            }
            accumulateFromValueIndices(indices);
        } else {
            _probeKey.reset(0, false, tagKeyView, valKeyView);
            auto htIt = _ht->find(_probeKey);
            if (htIt != _ht->end()) {
                accumulateFromValueIndices(htIt->second);
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
    ret->children.emplace_back(outerChild()->getStats(includeDebugInfo));
    ret->children.emplace_back(innerChild()->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* HashLookupStage::getSpecificStats() const {
    return nullptr;
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
