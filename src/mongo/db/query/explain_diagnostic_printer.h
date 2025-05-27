/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/curop_diagnostic_printer.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/logv2/redaction.h"

#include <fmt/format.h>

namespace mongo::diagnostic_printers {

/*
 * Computes a diagnostic string containing explain information about the current operation for use
 * during a tassert, invariant, or segfault. This structs captures some information about an
 * operation and implements a format() function which will be invoked only in the case of a failure.
 * When used in conjunction with ScopedDebugInfo, no work is done on the hot-path; all computation
 * of these diagnostics is done lazily during failure handling.
 */
struct ExplainDiagnosticPrinter {
    static constexpr StringData kNoExecOpMsg = "omitted: no executor"_sd;

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
