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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/db/query/compiler/rewrites/rule_based_rewriter.h"
#include "mongo/util/modules.h"

#include <type_traits>

namespace mongo::rule_based_rewrites::pipeline {

/**
 * Macro for registering rules for a given document source. Example usage:
 *
 * REGISTER_RULES(DocumentSourceMatch,
 *                OPTIMIZE_AT_RULE(DocumentSourceMatch),
 *                OPTIMIZE_RULE(DocumentSourceMatch),
 *                {"SOME_OTHER_RULE", precondition, transform, 1.0});
 */
#define REGISTER_RULES(DS, ...)                                                                    \
    const ServiceContext::ConstructorActionRegisterer documentSourcePrereqsRegisterer_##DS {       \
        "PipelineOptimizationContext" #DS, [](ServiceContext* service) {                           \
            namespace rbr = rule_based_rewrites::pipeline;                                         \
            rbr::registration_detail::enforceUniqueRuleNames(service, {__VA_ARGS__});              \
            auto& registry = getDocumentSourceVisitorRegistry(service);                            \
            registry.registerVisitorFunc<rbr::registration_detail::RuleRegisteringVisitorCtx, DS>( \
                [](DocumentSourceVisitorContextBase* ctx, const DocumentSource&) {                 \
                    static_cast<rbr::registration_detail::RuleRegisteringVisitorCtx*>(ctx)         \
                        ->addRules({__VA_ARGS__});                                                 \
                });                                                                                \
        }                                                                                          \
    }

/**
 * Helper for defining a rule that calls optimizeAt() for a given document source.
 */
#define OPTIMIZE_AT_RULE(DS)                                   \
    {                                                          \
        .name = "OPTIMIZE_AT_" #DS,                            \
        .precondition = rbr::alwaysTrue,                       \
        .transform = rbr::Transforms::optimizeAtWrapper<DS>,   \
        .priority = rbr::kDefaultOptimizeAtPriority,           \
        .tags = rbr::PipelineRewriteContext::Tags::Reordering, \
    }

/**
 * Helper for defining a rule that calls optimize() for a given document source.
 */
#define OPTIMIZE_IN_PLACE_RULE(DS)                          \
    {                                                       \
        .name = "OPTIMIZE_IN_PLACE_" #DS,                   \
        .precondition = rbr::alwaysTrue,                    \
        .transform = rbr::Transforms::optimizeWrapper<DS>,  \
        .priority = rbr::kDefaultOptimizeInPlacePriority,   \
        .tags = rbr::PipelineRewriteContext::Tags::InPlace, \
    }

// For high priority rules that e.g. attempt to push a $match as early as possible.
constexpr double kDefaultPushdownPriority = 100.0;
// For rules that e.g. attempt to swap with or absorb an adjacent stage.
constexpr double kDefaultOptimizeAtPriority = 10.0;
// For rules that optimize a stage in place.
constexpr double kDefaultOptimizeInPlacePriority = 1.0;

/**
 * Provides methods for walking and modifying a pipeline. Treats the pipeline as a linked list. Uses
 * the document source visitor registry to track which rules can apply to which document sources.
 */
class PipelineRewriteContext : public RewriteContext<PipelineRewriteContext, DocumentSource> {
public:
    enum Tags : TagSet {
        None = 0,
        // Rules that optimize the internals of a stage in place but never touch adjacent stages.
        InPlace = 1 << 0,
        // Rules that may e.g. reorder, combine or remove stages.
        Reordering = 1 << 1,
    };

    PipelineRewriteContext(Pipeline& pipeline)
        : PipelineRewriteContext(*pipeline.getContext(), pipeline.getSources()) {}

    PipelineRewriteContext(const ExpressionContext& expCtx,
                           DocumentSourceContainer& container,
                           boost::optional<DocumentSourceContainer::iterator> startingPos = {})
        : PipelineRewriteContext(
              getDocumentSourceVisitorRegistry(expCtx.getOperationContext()->getServiceContext()),
              container,
              startingPos) {}

    PipelineRewriteContext(const DocumentSourceVisitorRegistry& registry,
                           DocumentSourceContainer& container,
                           boost::optional<DocumentSourceContainer::iterator> startingPos = {})
        : _container(container),
          _itr(startingPos.value_or(_container.begin())),
          _registry(registry) {}

    bool hasMore() const final {
        return _itr != _container.end();
    }

    DocumentSource& current() final {
        return **_itr;
    }
    const DocumentSource& current() const final {
        return **_itr;
    }

    void advance() final;
    void enqueueRules() final;

    template <size_t N>
    bool hasAtLeastNPrevStages() const {
        return std::distance(_container.begin(), _itr) >= static_cast<std::ptrdiff_t>(N);
    }

    template <size_t N>
    boost::intrusive_ptr<DocumentSource> nthPrevStage() const {
        tassert(11010007,
                str::stream() << "Expected to have " << N << " previous stages",
                hasAtLeastNPrevStages<N>());

        auto itr = _itr;
        std::advance(itr, -std::make_signed_t<size_t>(N));
        return *itr;
    }

    boost::intrusive_ptr<DocumentSource> prevStage() const {
        return nthPrevStage<1>();
    }

    boost::intrusive_ptr<DocumentSource> nextStage() const {
        tassert(11010005, "Already at last stage", !atLastStage());
        return *std::next(_itr);
    }

    bool atFirstStage() const {
        return _itr == _container.begin();
    }

    bool atLastStage() const {
        return std::next(_itr) == _container.end();
    }

    std::string debugString() const;

private:
    DocumentSourceContainer& _container;
    DocumentSourceContainer::iterator _itr;

    const DocumentSourceVisitorRegistry& _registry;

    friend struct Transforms;
};

using PipelineRewriteRule = Rule<PipelineRewriteContext>;
using PipelineRewriteEngine = RewriteEngine<PipelineRewriteContext>;

/**
 * Provides a set of common transformations that can be used either directly as transforms or inside
 * transforms to manipulate the pipeline.
 */
struct Transforms {
    static bool swapStageWithPrev(PipelineRewriteContext& ctx);
    static bool swapStageWithNext(PipelineRewriteContext& ctx);
    static bool insertBefore(PipelineRewriteContext& ctx, DocumentSource& d);
    static bool insertAfter(PipelineRewriteContext& ctx, DocumentSource& d);
    static bool eraseCurrent(PipelineRewriteContext& ctx);
    static bool eraseNext(PipelineRewriteContext& ctx);

    /**
     * Pushes 'pushdownPart' before the previous stage. Assumes that 'ctx.current()' is the match
     * we're pushing down.
     */
    static bool partialPushdown(PipelineRewriteContext& ctx,
                                boost::intrusive_ptr<DocumentSource> pushdownPart,
                                boost::intrusive_ptr<DocumentSource> remainingPart);
    /**
     * Convenience for "sentinel" rules that detect conditions and queue other rules, but may not
     * result in other transformations.
     */
    static inline bool noop(PipelineRewriteContext&) {
        return false;
    }

    template <typename DS>
    static bool optimizeWrapper(PipelineRewriteContext& ctx) {
        if (auto result = ctx.currentAs<DS>().optimize()) {
            *ctx._itr = std::move(result);
            return false;
        }

        // If the current stage optimized to null, remove it and move on to the next one. Note that
        // we can advance here only because we know that in-place optimizations are the last rules
        // to be applied. If that changes in the future, we must not advance here. The advantage of
        // advancing is that we don't end up redundantly re-attempting rules that have already been
        // applied to the previous stage.
        eraseCurrent(ctx);
        if (ctx.hasMore()) {
            ctx.advance();
        }
        return true;
    }

    template <typename DS>
    static bool optimizeAtWrapper(PipelineRewriteContext& ctx) {
        const auto getAdjacentStages = [&](DocumentSourceContainer::iterator itr) {
            auto prev = itr == ctx._container.begin() ? nullptr : *std::prev(itr);
            auto curr = itr == ctx._container.end() ? nullptr : *itr;
            auto next = !curr || std::next(itr) == ctx._container.end() ? nullptr : *std::next(itr);
            return std::make_tuple(std::move(prev), std::move(curr), std::move(next));
        };

        auto stagesBefore = getAdjacentStages(ctx._itr);
        auto resultItr = ctx.currentAs<DS>().optimizeAt(ctx._itr, &ctx._container);
        // If nothing changed, resultItr points to the next position.
        auto stagesAfter = getAdjacentStages(
            resultItr == ctx._container.begin() ? resultItr : std::prev(resultItr));

        // Try to detect if optimizeAt() did anything. Normally, std::next() indicates that no
        // optimizations were performed. However, it's also possible that the current (or some
        // other) stage was completely erased, which means comparisons involving the erased
        // iterators would be undefined behavior.
        if (stagesBefore == stagesAfter &&
            (resultItr == ctx._container.end() || resultItr == std::next(ctx._itr))) {
            // We know that optimizeAt() didn't do anything. Current position may still have been
            // erased (and re-inserted) by optimizeAt(), so we need to re-set it just in case.
            ctx._itr = std::prev(resultItr);
            return false;
        }

        ctx._itr = resultItr;
        return true;
    }
};

inline bool alwaysTrue(PipelineRewriteContext&) {
    return true;
}

namespace registration_detail {
/**
 * Helper for queueing rules using the document source visitor registry.
 */
class RuleRegisteringVisitorCtx : public DocumentSourceVisitorContextBase {
public:
    RuleRegisteringVisitorCtx(PipelineRewriteContext& ctx) : _ctx(ctx) {}

    void addRules(std::vector<Rule<PipelineRewriteContext>> rules) {
        _ctx.addRules(std::move(rules));
    }

private:
    PipelineRewriteContext& _ctx;
};

/**
 * Enforces that pipeline rewrite rule names registered under the same service context are unique.
 */
void enforceUniqueRuleNames(ServiceContext* service,
                            std::vector<Rule<PipelineRewriteContext>> rules);
}  // namespace registration_detail
}  // namespace mongo::rule_based_rewrites::pipeline
