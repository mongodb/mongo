// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::diagnostic_printers {
using namespace std::literals::string_view_literals;

/**
 * Diagnostic printer for the ExpressionContext class. Take care when extending this to redact any
 * sensitive information.
 */
struct ExpressionContextPrinter {

    auto format(auto& fc) const {
        auto out = fc.out();

        if (!expCtx) {
            out = fmt::format_to(out, "expCtx is null");
            return out;
        }

        out = fmt::format_to(out, "{{");
        auto field = [&, sep = ""sv](std::string_view name, auto&& value) mutable {
            out = fmt::format_to(out, "{}{}: {}", std::exchange(sep, ", "sv), name, value);
        };
        field("collator", expCtx->getCollatorBSON().toString());
        field("uuid", expCtx->getUUID() ? redact(expCtx->getUUID()->toString()) : "none");
        field("needsMerge", expCtx->getNeedsMerge());
        field("allowDiskUse", expCtx->getAllowDiskUse());
        field("isMapReduceCommand", expCtx->isMapReduceCommand());
        field("inLookup", expCtx->getInLookup());
        field("inUnionWith", expCtx->getInUnionWith());
        field("forcePlanCache", expCtx->getForcePlanCache());
        field("sbeCompatibility", fmt::underlying(expCtx->getSbeCompatibility()));
        field("sbeGroupCompatibility", fmt::underlying(expCtx->getSbeGroupCompatibility()));
        field("sbeWindowCompatibility", fmt::underlying(expCtx->getSbeWindowCompatibility()));
        field("sbePipelineCompatibility", fmt::underlying(expCtx->getSbePipelineCompatibility()));
        field("subPipelineDepth", expCtx->getSubPipelineDepth());
        out = fmt::format_to(out, "}}");

        return out;
    }

    // This pointer must outlive this class.
    boost::intrusive_ptr<ExpressionContext> expCtx;
};

}  // namespace mongo::diagnostic_printers

namespace fmt {

template <>
struct formatter<mongo::diagnostic_printers::ExpressionContextPrinter> {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }

    auto format(const mongo::diagnostic_printers::ExpressionContextPrinter& obj, auto& ctx) const {
        return obj.format(ctx);
    }
};

}  // namespace fmt
