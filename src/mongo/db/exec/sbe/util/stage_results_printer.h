// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace mongo::sbe {

template <typename T>
class StageResultsPrinter;

/**
 * Companion static class to StageResultsPrinter template.
 */
class StageResultsPrinters {
    StageResultsPrinters() = delete;
    StageResultsPrinters(const StageResultsPrinters&) = delete;

public:
    using SlotNames = std::vector<std::pair<value::SlotId, std::string>>;

    static StageResultsPrinter<std::ostream> make(std::ostream& stream,
                                                  const PrintOptions& options);
    static StageResultsPrinter<str::stream> make(str::stream& stream, const PrintOptions& options);
};

template <typename T>
class StageResultsPrinter {
    StageResultsPrinter() = delete;
    StageResultsPrinter(T& stream, const PrintOptions& options);

    friend class StageResultsPrinters;

public:
    using SlotNames = StageResultsPrinters::SlotNames;

    void printStageResults(CompileCtx* ctx,
                           const value::SlotVector& slots,
                           const std::vector<std::string>& names,
                           PlanStage* stage);

    void printStageResults(CompileCtx* ctx, const SlotNames& slotNames, PlanStage* stage);

    void printSlotNames(const SlotNames& slotNames);

    void printSpecificStats(const PlanStage* stage);

    void printSpecificStats(const SpecificStats* stats);

private:
    T& _stream;
    const PrintOptions& _options;
    value::ValuePrinter<T> _valuePrinter;
};

}  // namespace mongo::sbe
