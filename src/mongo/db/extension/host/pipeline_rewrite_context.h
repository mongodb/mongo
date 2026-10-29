// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"
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

    // Returns the docs-needed bounds computed from all stages after this one in the pipeline.
    DocsNeededBounds getPipelineSuffixBounds() const {
        auto suffix = _ctx->getSuffixSources();
        return extractDocsNeededBounds(suffix, _ctx->getExpCtx());
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
