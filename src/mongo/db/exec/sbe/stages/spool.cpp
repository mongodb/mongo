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

#include "mongo/db/exec/sbe/stages/spool.h"

namespace mongo::sbe {
SpoolEagerProducerStage::SpoolEagerProducerStage(std::unique_ptr<PlanStage> input,
                                                 SpoolId spoolId,
                                                 value::SlotVector vals)
    : PlanStage{"espool"_sd}, _spoolId{spoolId}, _vals{std::move(vals)} {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> SpoolEagerProducerStage::clone() const {
    return std::make_unique<SpoolEagerProducerStage>(_children[0]->clone(), _spoolId, _vals);
}

void SpoolEagerProducerStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    if (!_buffer) {
        _buffer = ctx.getSpoolBuffer(_spoolId);
    }

    value::SlotSet dupCheck;
    size_t counter = 0;

    for (auto slot : _vals) {
        auto [it, inserted] = dupCheck.insert(slot);
        uassert(4822810, str::stream() << "duplicate field: " << slot, inserted);

        _inAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));
        _outAccessors.emplace(
            slot, value::MaterializedRowAccessor<SpoolBuffer>{*_buffer, _bufferIt, counter++});
    }
}

value::SlotAccessor* SpoolEagerProducerStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
        return &it->second;
    }

    return ctx.getAccessor(slot);
}

void SpoolEagerProducerStage::open(bool reOpen) {
    _commonStats.opens++;
    _children[0]->open(reOpen);

    if (reOpen) {
        _buffer->clear();
    }

    while (_children[0]->getNext() == PlanState::ADVANCED) {
        value::MaterializedRow vals{_inAccessors.size()};

        size_t idx = 0;
        for (auto accessor : _inAccessors) {
            auto [tag, val] = accessor->copyOrMoveValue();
            vals.reset(idx++, true, tag, val);
        }

        _buffer->emplace_back(std::move(vals));
    }

    _children[0]->close();
    _bufferIt = _buffer->size();
}

PlanState SpoolEagerProducerStage::getNext() {
    if (_bufferIt == _buffer->size()) {
        _bufferIt = 0;
    } else {
        ++_bufferIt;
    }

    if (_bufferIt == _buffer->size()) {
        return trackPlanState(PlanState::IS_EOF);
    }

    return trackPlanState(PlanState::ADVANCED);
}

void SpoolEagerProducerStage::close() {
    _commonStats.closes++;
}

std::unique_ptr<PlanStageStats> SpoolEagerProducerStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats());
    return ret;
}

const SpecificStats* SpoolEagerProducerStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> SpoolEagerProducerStage::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "espool");

    DebugPrinter::addSpoolIdentifier(ret, _spoolId);

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _vals.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _vals[idx]);
    }
    ret.emplace_back("`]");

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    return ret;
}

SpoolLazyProducerStage::SpoolLazyProducerStage(std::unique_ptr<PlanStage> input,
                                               SpoolId spoolId,
                                               value::SlotVector vals,
                                               std::unique_ptr<EExpression> predicate)
    : PlanStage{"lspool"_sd},
      _spoolId{spoolId},
      _vals{std::move(vals)},
      _predicate{std::move(predicate)} {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> SpoolLazyProducerStage::clone() const {
    return std::make_unique<SpoolLazyProducerStage>(
        _children[0]->clone(), _spoolId, _vals, _predicate->clone());
}

void SpoolLazyProducerStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    if (!_buffer) {
        _buffer = ctx.getSpoolBuffer(_spoolId);
    }

    if (_predicate) {
        ctx.root = this;
        _predicateCode = _predicate->compile(ctx);
    }

    value::SlotSet dupCheck;

    for (auto slot : _vals) {
        auto [it, inserted] = dupCheck.insert(slot);
        uassert(4822811, str::stream() << "duplicate field: " << slot, inserted);

        _inAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));
        _outAccessors.emplace(slot, value::ViewOfValueAccessor{});
    }

    _compiled = true;
}

value::SlotAccessor* SpoolLazyProducerStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compiled) {
        if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
            return &it->second;
        }
    } else {
        return _children[0]->getAccessor(ctx, slot);
    }

    return ctx.getAccessor(slot);
}

void SpoolLazyProducerStage::open(bool reOpen) {
    _commonStats.opens++;
    _children[0]->open(reOpen);

    if (reOpen) {
        _buffer->clear();
    }
}

PlanState SpoolLazyProducerStage::getNext() {
    auto state = _children[0]->getNext();

    if (state == PlanState::ADVANCED) {
        auto pass{true};

        if (_predicateCode) {
            pass = _bytecode.runPredicate(_predicateCode.get());
        }

        if (pass) {
            // We either haven't got a predicate, or it has passed. In both cases, we need pass
            // through the input values, and store them into the buffer.
            value::MaterializedRow vals{_inAccessors.size()};

            for (size_t idx = 0; idx < _inAccessors.size(); ++idx) {
                auto [tag, val] = _inAccessors[idx]->getViewOfValue();
                _outAccessors[_vals[idx]].reset(tag, val);

                auto [copyTag, copyVal] = value::copyValue(tag, val);
                vals.reset(idx, true, copyTag, copyVal);
            }

            _buffer->emplace_back(std::move(vals));
        } else {
            // Otherwise, just pass through the input values.
            for (size_t idx = 0; idx < _inAccessors.size(); ++idx) {
                auto [tag, val] = _inAccessors[idx]->getViewOfValue();
                _outAccessors[_vals[idx]].reset(tag, val);
            }
        }
    }

    return trackPlanState(state);
}

void SpoolLazyProducerStage::close() {
    _commonStats.closes++;
}

std::unique_ptr<PlanStageStats> SpoolLazyProducerStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats());
    return ret;
}

const SpecificStats* SpoolLazyProducerStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> SpoolLazyProducerStage::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "lspool");

    DebugPrinter::addSpoolIdentifier(ret, _spoolId);

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _vals.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _vals[idx]);
    }
    ret.emplace_back("`]");

    if (_predicate) {
        ret.emplace_back("{`");
        DebugPrinter::addBlocks(ret, _predicate->debugPrint());
        ret.emplace_back("`}");
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    return ret;
}
}  // namespace mongo::sbe
