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

#include "mongo/db/exec/sbe/util/stage_results_printer.h"

#include "mongo/db/exec/plan_stats_visitor.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/assert_util.h"

#include <cstddef>

#include <absl/container/inlined_vector.h>

namespace mongo::sbe {

template <typename T>
StageResultsPrinter<T>::StageResultsPrinter(T& stream, const PrintOptions& options)
    : _stream(stream),
      _options(options),
      _valuePrinter(value::ValuePrinters::make(stream, options)) {}

template <typename T>
void StageResultsPrinter<T>::printStageResults(CompileCtx* ctx,
                                               const value::SlotVector& slots,
                                               const std::vector<std::string>& names,
                                               PlanStage* stage) {
    tassert(6441701, "slots and names sizes must match", slots.size() == names.size());
    SlotNames slotNames;
    size_t idx = 0;
    for (auto slot : slots) {
        slotNames.emplace_back(slot, names[idx++]);
    }

    printStageResults(ctx, slotNames, stage);
}

template <typename T>
void StageResultsPrinter<T>::printStageResults(CompileCtx* ctx,
                                               const SlotNames& slotNames,
                                               PlanStage* stage) {
    std::vector<value::SlotAccessor*> accessors;
    for (const auto& slot : slotNames) {
        accessors.push_back(stage->getAccessor(*ctx, slot.first));
    }

    printSlotNames(slotNames);
    _stream << ":"
            << "\n";

    size_t iter = 0;
    for (auto st = stage->getNext(); st == PlanState::ADVANCED; st = stage->getNext(), iter++) {
        if (iter >= _options.arrayObjectOrNestingMaxDepth()) {
            _stream << "..."
                    << "\n";
            break;
        }

        bool first = true;
        for (auto accessor : accessors) {
            if (!first) {
                _stream << ", ";
            } else {
                first = false;
            }
            auto [tag, val] = accessor->getViewOfValue();
            _valuePrinter.writeValueToStream(tag, val);
        }
        _stream << "\n";
    }
}

template <typename T>
void StageResultsPrinter<T>::printSlotNames(const SlotNames& slotNames) {
    _stream << "[";
    bool first = true;
    for (const auto& slot : slotNames) {
        if (!first) {
            _stream << ", ";
        } else {
            first = false;
        }
        _stream << slot.second;
    }
    _stream << "]";
}

/**
 * Visitor for printing the specific stats of a query stage into a stream.
 */
template <typename T>
struct PlanStatsSpecificStatsPrinter : PlanStatsVisitorBase<true> {
    PlanStatsSpecificStatsPrinter() = delete;
    PlanStatsSpecificStatsPrinter(T& stream) : _stream(stream) {}

    // To avoid overloaded-virtual warnings.
    using PlanStatsConstVisitor::visit;

    void visit(tree_walker::MaybeConstPtr<true, sbe::HashLookupStats> stats) final {
        _stream << "dsk:" << stats->usedDisk << "\n";
        _stream << "htRecs:" << stats->spillingHtStats.getSpilledRecords() << "\n";
        _stream << "htIndices:" << stats->spillingHtStats.getSpilledBytes() << "\n";
        _stream << "buffRecs:" << stats->spillingBuffStats.getSpilledRecords() << "\n";
        _stream << "buffBytes:" << stats->spillingBuffStats.getSpilledBytes() << "\n";
    }

    // TODO: Add an overload for the specific stats of other stages by overriding their
    // corresponding `visit` method if needed.

private:
    T& _stream;
};

template <typename T>
void StageResultsPrinter<T>::printSpecificStats(const SpecificStats* stats) {
    auto visitor = PlanStatsSpecificStatsPrinter<T>(_stream);
    stats->acceptVisitor(&visitor);
}

template <typename T>
void StageResultsPrinter<T>::printSpecificStats(const PlanStage* stage) {
    printSpecificStats(stage->getSpecificStats());
}

template class StageResultsPrinter<std::ostream>;
template class StageResultsPrinter<str::stream>;

StageResultsPrinter<std::ostream> StageResultsPrinters::make(std::ostream& stream,
                                                             const PrintOptions& options) {
    return StageResultsPrinter(stream, options);
}

StageResultsPrinter<str::stream> StageResultsPrinters::make(str::stream& stream,
                                                            const PrintOptions& options) {
    return StageResultsPrinter(stream, options);
}

}  // namespace mongo::sbe
