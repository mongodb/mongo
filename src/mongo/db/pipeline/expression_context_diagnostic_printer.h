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

#include <fmt/format.h>

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/logv2/redaction.h"

namespace mongo::command_diagnostics {

/**
 * Diagnostic printer for the ExpressionContext class. Take care when extending this to redact any
 * sensitive information.
 */
struct ExpressionContextPrinter {

    auto format(auto& fc) const {
        auto out = fc.out();

        if (!expCtx) {
            out = format_to(out, FMT_STRING("expCtx is null"));
            return out;
        }

        out = format_to(
            out,
            FMT_STRING("{{collator: {}, uuid: {}, needsMerge: {}, allowDiskUse: {}, "
                       "isMapReduceCommand: {}, "
                       "inLookup: {}, inUnionWith: {}, forcePlanCache: {}, sbeCompatibility: {}, "
                       "sbeGroupCompatibility: {}, sbeWindowCompatibility: {}, "
                       "sbePipelineCompatibility: {}, subPipelineDepth: {}}}"),
            expCtx->getCollatorBSON().toString(),
            expCtx->getUUID() ? redact(expCtx->getUUID()->toString()) : "none",
            expCtx->getNeedsMerge(),
            expCtx->getAllowDiskUse(),
            expCtx->isMapReduceCommand(),
            expCtx->getInLookup(),
            expCtx->getInUnionWith(),
            expCtx->getForcePlanCache(),
            expCtx->getSbeCompatibility(),
            expCtx->getSbeGroupCompatibility(),
            expCtx->getSbeWindowCompatibility(),
            expCtx->getSbePipelineCompatibility(),
            expCtx->getSubPipelineDepth());

        return out;
    }

    // This pointer must outlive this class.
    boost::intrusive_ptr<ExpressionContext> expCtx;
};

}  // namespace mongo::command_diagnostics

namespace fmt {

template <>
struct formatter<mongo::command_diagnostics::ExpressionContextPrinter> {
    constexpr auto parse(auto& ctx) {
        return ctx.begin();
    }

    auto format(const mongo::command_diagnostics::ExpressionContextPrinter& obj, auto& ctx) {
        return obj.format(ctx);
    }
};

}  // namespace fmt
