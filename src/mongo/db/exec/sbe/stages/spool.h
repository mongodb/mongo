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

#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <absl/meta/type_traits.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::sbe {
/**
 * This is a Spool PlanStage which retains a copy of all data it reads from its child in a shared
 * 'SpoolBuffer', and can later return this data without having to call its child to produce it
 * again.
 *
 * This spool operates in an 'Eager' producer mode. On the call to 'open()' it will read and store
 * the entire input from its child into the buffer. On the 'getNext' call it will return data from
 * the buffer.
 *
 * This producer spool can be connected with multiple consumer spools via a shared 'SpoolBuffer'.
 * This stage will be responsible for populating the buffer, while consumers will read from the
 * buffer once its populated, each using its own read pointer.
 *
 * Debug string representation:
 *
 *   espool spoolId [<vals>] childStage
 */
class SpoolEagerProducerStage final : public PlanStage {
public:
    SpoolEagerProducerStage(std::unique_ptr<PlanStage> input,
                            SpoolId spoolId,
                            value::SlotVector vals,
                            PlanYieldPolicy* yieldPolicy,
                            PlanNodeId planNodeId,
                            bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

private:
    std::shared_ptr<SpoolBuffer> _buffer{nullptr};
    size_t _bufferIt{0};
    const SpoolId _spoolId;

    const value::SlotVector _vals;
    std::vector<value::SlotAccessor*> _inAccessors;
    value::SlotMap<value::MaterializedRowAccessor<SpoolBuffer>> _outAccessors;
};

/**
 * This is a Spool PlanStage which retains a copy of all data it reads from its child in a shared
 * 'SpoolBuffer', and can later return this data without having to call its child to produce it
 * again.
 *
 * This spool operates in a 'Lazy' producer mode. In contrast to the 'Eager' producer spool, on the
 * call to 'open()' it will _not_ read and populate the buffer. Instead, on the call to 'getNext'
 * it will read and store the input into the buffer, and immediately return it to the caller stage.
 *
 * This producer spool can be connected with multiple consumer spools via a shared 'SpoolBuffer'.
 * This stage will be responsible for populating the buffer in a lazy fashion as described above,
 * while consumers will read from the buffer (possibly while it's still being populated), each using
 * its own read pointer.
 *
 * This spool can be parameterized with an optional predicate which can be used to filter the input
 * and store only portion of input data into the buffer. Filtered out input data is passed through
 * without being stored into the buffer.
 *
 * Debug string representation:
 *
 *   lspool spoolId [<vals>] { predicate }? childStage
 */
class SpoolLazyProducerStage final : public PlanStage {
public:
    SpoolLazyProducerStage(std::unique_ptr<PlanStage> input,
                           SpoolId spoolId,
                           value::SlotVector vals,
                           std::unique_ptr<EExpression> predicate,
                           PlanNodeId planNodeId,
                           bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;
    size_t estimateCompileTimeSize() const final;

protected:
    void doSaveState(bool relinquishCursor) final;
    bool shouldOptimizeSaveState(size_t) const final {
        return true;
    }

private:
    std::shared_ptr<SpoolBuffer> _buffer{nullptr};
    const SpoolId _spoolId;

    const value::SlotVector _vals;
    std::vector<value::SlotAccessor*> _inAccessors;
    value::SlotMap<value::OwnedValueAccessor> _outAccessors;

    std::unique_ptr<EExpression> _predicate;
    std::unique_ptr<vm::CodeFragment> _predicateCode;
    vm::ByteCode _bytecode;
    bool _compiled{false};
};

/**
 * This is Spool PlanStage which operates in read-only mode. It doesn't populate its 'SpoolBuffer'
 * with the input data (and as such, it doesn't have an input PlanStage) but reads and returns data
 * from a shared 'SpoolBuffer' that is populated by another producer spool stage.
 *
 * This consumer PlanStage can operate as a Stack Spool, in conjunction with a 'Lazy' producer
 * spool. In this mode the consumer spool on each call to 'getNext' first deletes the input from
 * buffer, remembered on the previous call to 'getNext', and then moves the read pointer to the last
 * element in the buffer and returns it.
 *
 * Since in 'Stack' mode this spool always returns the last input from the buffer, it does not read
 * data in the same order as they were added. It will always return the last added input. For
 * example, the lazy spool can add values [1,2,3], then the stack consumer spool will read and
 * delete 3, then another two values can be added to the buffer [1,2,4,5], then the consumer spool
 * will read and delete 5, and so on.
 *
 * Debug string representations:
 *
 *   cspool spoolId [<vals>]
 *   sspool spoolId [<vals>]
 */
template <bool IsStack>
class SpoolConsumerStage final : public PlanStage {
public:
    SpoolConsumerStage(SpoolId spoolId,
                       value::SlotVector vals,
                       PlanYieldPolicy* yieldPolicy,
                       PlanNodeId planNodeId,
                       bool participateInTrialRunTracking = true)
        : PlanStage{IsStack ? "sspool"_sd : "cspool"_sd,
                    yieldPolicy,
                    planNodeId,
                    participateInTrialRunTracking},
          _spoolId{spoolId},
          _vals{std::move(vals)} {}

    std::unique_ptr<PlanStage> clone() const {
        return std::make_unique<SpoolConsumerStage<IsStack>>(
            _spoolId, _vals, _yieldPolicy, _commonStats.nodeId, _participateInTrialRunTracking);
    }

    void prepare(CompileCtx& ctx) {
        if (!_buffer) {
            _buffer = ctx.getSpoolBuffer(_spoolId);
        }

        value::SlotSet dupCheck;
        size_t counter = 0;

        for (auto slot : _vals) {
            auto [it, inserted] = dupCheck.insert(slot);
            uassert(4822809, str::stream() << "duplicate field: " << slot, inserted);

            _outAccessors.emplace(
                slot, value::MaterializedRowAccessor<SpoolBuffer>{*_buffer, _bufferIt, counter++});
        }
    }

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) {
        if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
            return &it->second;
        }

        return ctx.getAccessor(slot);
    }

    void open(bool reOpen) {
        auto optTimer(getOptTimer(_opCtx));

        _commonStats.opens++;
        _bufferIt = _buffer->size();
    }

    PlanState getNext() {
        auto optTimer(getOptTimer(_opCtx));
        checkForInterruptAndYield(_opCtx);

        if constexpr (IsStack) {
            if (_bufferIt != _buffer->size()) {
                _buffer->erase(_buffer->begin() + _bufferIt);
            }

            if (_buffer->size() == 0) {
                return trackPlanState(PlanState::IS_EOF);
            }

            _bufferIt = _buffer->size() - 1;
        } else {
            if (_bufferIt == _buffer->size()) {
                _bufferIt = 0;
            } else {
                ++_bufferIt;
            }

            if (_bufferIt == _buffer->size()) {
                return trackPlanState(PlanState::IS_EOF);
            }
        }
        return trackPlanState(PlanState::ADVANCED);
    }

    void close() {
        auto optTimer(getOptTimer(_opCtx));

        trackClose();
    }

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const {
        auto ret = std::make_unique<PlanStageStats>(_commonStats);

        if (includeDebugInfo) {
            BSONObjBuilder bob;
            bob.appendNumber("spoolId", static_cast<long long>(_spoolId));
            bob.append("outputSlots", _vals.begin(), _vals.end());
            ret->debugInfo = bob.obj();
        }

        return ret;
    }

    const SpecificStats* getSpecificStats() const {
        return nullptr;
    }

    std::vector<DebugPrinter::Block> debugPrint() const {
        auto ret = PlanStage::debugPrint();

        DebugPrinter::addSpoolIdentifier(ret, _spoolId);

        ret.emplace_back(DebugPrinter::Block("[`"));
        for (size_t idx = 0; idx < _vals.size(); ++idx) {
            if (idx) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }

            DebugPrinter::addIdentifier(ret, _vals[idx]);
        }
        ret.emplace_back("`]");

        return ret;
    }

    size_t estimateCompileTimeSize() const {
        size_t size = sizeof(*this);
        size += size_estimator::estimate(_vals);
        return size;
    }

private:
    std::shared_ptr<SpoolBuffer> _buffer{nullptr};
    size_t _bufferIt{0};
    const SpoolId _spoolId;

    const value::SlotVector _vals;
    value::SlotMap<value::MaterializedRowAccessor<SpoolBuffer>> _outAccessors;
};
}  // namespace mongo::sbe
