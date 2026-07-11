// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/util/stage_results_printer.h"

#include "mongo/db/exec/plan_stats_visitor.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/util/assert_util.h"

#include <cstddef>

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
    slotNames.reserve(slots.size());
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
    accessors.reserve(slotNames.size());
    for (const auto& slot : slotNames) {
        accessors.push_back(stage->getAccessor(*ctx, slot.first));
    }

    printSlotNames(slotNames);
    _stream << ":\n";

    size_t iter = 0;
    for (auto st = stage->getNext(); st == PlanState::ADVANCED; st = stage->getNext(), iter++) {
        if (iter >= _options.arrayObjectOrNestingMaxDepth()) {
            _stream << "...\n";
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
