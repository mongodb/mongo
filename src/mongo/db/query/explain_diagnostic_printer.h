// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/curop_diagnostic_printer.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::diagnostic_printers {
using namespace std::literals::string_view_literals;

/*
 * Computes a diagnostic string containing explain information about the current operation for use
 * during a tassert, invariant, or segfault. This structs captures some information about an
 * operation and implements a format() function which will be invoked only in the case of a failure.
 * When used in conjunction with ScopedDebugInfo, no work is done on the hot-path; all computation
 * of these diagnostics is done lazily during failure handling.
 */
struct ExplainDiagnosticPrinter {
    static constexpr std::string_view kNoExecOpMsg = "omitted: no executor"sv;

    auto format(auto& fc) const {
        auto out = fc.out();
        if (!exec) {
            return fmt::format_to(out, "{}", kNoExecOpMsg);
        }

        auto opCtx = exec->getOpCtx();
        if (auto msg = isIneligibleForDiagnosticPrinting(opCtx)) {
            return fmt::format_to(out, "{}", msg.get());
        }

        auto&& explainer = exec->getPlanExplainer();
        auto&& [winningPlan, _] =
            explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
        auto&& [execStats, __] =
            explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
        out = fmt::format_to(out,
                             "{{'winningPlan': {}, 'executionStats': {}}}",
                             redact(winningPlan).toString(),
                             redact(execStats).toString());

        return out;
    }

    // This pointer must outlive this class.
    PlanExecutor* exec;
};

}  // namespace mongo::diagnostic_printers

namespace fmt {

template <>
struct formatter<mongo::diagnostic_printers::ExplainDiagnosticPrinter> {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }

    auto format(const mongo::diagnostic_printers::ExplainDiagnosticPrinter& obj, auto& ctx) const {
        return obj.format(ctx);
    }
};
}  // namespace fmt
