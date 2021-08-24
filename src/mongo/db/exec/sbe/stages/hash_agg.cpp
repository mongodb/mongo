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

#include "mongo/db/exec/sbe/stages/hash_agg.h"

#include "mongo/util/str.h"

namespace mongo {
namespace sbe {
HashAggStage::HashAggStage(std::unique_ptr<PlanStage> input,
                           value::SlotVector gbs,
                           value::SlotMap<std::unique_ptr<EExpression>> aggs,
                           value::SlotVector seekKeysSlots,
                           bool optimizedClose,
                           boost::optional<value::SlotId> collatorSlot,
                           PlanNodeId planNodeId)
    : PlanStage("group"_sd, planNodeId),
      _gbs(std::move(gbs)),
      _aggs(std::move(aggs)),
      _collatorSlot(collatorSlot),
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
                                          _commonStats.nodeId);
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
        _outKeyAccessors.emplace_back(std::make_unique<HashKeyAccessor>(_htIt, counter++));
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

        _outAggAccessors.emplace_back(std::make_unique<HashAggAccessor>(_htIt, counter++));
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

        while (_children[0]->getNext() == PlanState::ADVANCED) {
            value::MaterializedRow key{_inKeyAccessors.size()};
            // Copy keys in order to do the lookup.
            size_t idx = 0;
            for (auto& p : _inKeyAccessors) {
                auto [tag, val] = p->getViewOfValue();
                key.reset(idx++, false, tag, val);
            }

            auto [it, inserted] = _ht->try_emplace(std::move(key), value::MaterializedRow{0});
            if (inserted) {
                // Copy keys.
                const_cast<value::MaterializedRow&>(it->first).makeOwned();
                // Initialize accumulators.
                it->second.resize(_outAggAccessors.size());
            }

            // Accumulate.
            _htIt = it;
            for (size_t idx = 0; idx < _outAggAccessors.size(); ++idx) {
                auto [owned, tag, val] = _bytecode.run(_aggCodes[idx].get());
                _outAggAccessors[idx]->reset(owned, tag, val);
            }

            // Track memory usage.
            auto shouldCalculateEstimatedSize =
                _pseudoRandom.nextCanonicalDouble() < _memoryUseSampleRate;
            if (shouldCalculateEstimatedSize) {
                long estimatedSizeForOneRow =
                    it->first.memUsageForSorter() + it->second.memUsageForSorter();
                long long estimatedTotalSize = _ht->size() * estimatedSizeForOneRow;
                uassert(5859000,
                        "Need to spill to disk",
                        estimatedTotalSize < _approxMemoryUseInBytesBeforeSpill);
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
}

PlanState HashAggStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

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
        // Returning the results of the entire hash table.
        ++_htIt;
    }

    if (_htIt == _ht->end()) {
        return trackPlanState(PlanState::IS_EOF);
    }

    return trackPlanState(PlanState::ADVANCED);
}

std::unique_ptr<PlanStageStats> HashAggStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

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
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* HashAggStage::getSpecificStats() const {
    return nullptr;
}

void HashAggStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _ht = boost::none;

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
}  // namespace sbe
}  // namespace mongo
