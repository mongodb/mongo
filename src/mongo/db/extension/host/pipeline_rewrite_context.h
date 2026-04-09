/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host {

/**
 * Wraps a rule_based_rewrites::pipeline::PipelineRewriteContext for use by the host extension
 * layer, providing access to adjacent pipeline stages without exposing internal rewriter types.
 */
class PipelineRewriteContext {
public:
    ~PipelineRewriteContext() = default;

    boost::intrusive_ptr<DocumentSource> getNthNextStage(size_t index) const {
        return _ctx->nthNextStage(index);
    }

    bool eraseNthNext(size_t index) {
        return rule_based_rewrites::pipeline::Transforms::eraseNthNext(*_ctx, index);
    }

    bool hasAtLeastNNextStages(size_t n) const {
        return _ctx->hasAtLeastNNextStages(n);
    }

    static inline std::unique_ptr<PipelineRewriteContext> make(
        rule_based_rewrites::pipeline::PipelineRewriteContext* ctx) {
        return std::unique_ptr<PipelineRewriteContext>(new PipelineRewriteContext(ctx));
    }

protected:
    PipelineRewriteContext(absl::Nonnull<rule_based_rewrites::pipeline::PipelineRewriteContext*>
                               pipelineRewriteContext)
        : _ctx(pipelineRewriteContext) {}


private:
    rule_based_rewrites::pipeline::PipelineRewriteContext* _ctx;
};

}  // namespace mongo::extension::host
