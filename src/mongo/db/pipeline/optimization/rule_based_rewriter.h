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
#define REGISTER_RULES(DS, ...)                                                                  \
    const ServiceContext::ConstructorActionRegisterer documentSourcePrereqsRegisterer_##DS{      \
        "PipelineOptimizationContext" #DS, [](ServiceContext* service) {                         \
            registration_detail::enforceUniqueRuleNames(service, {__VA_ARGS__});                 \
            auto& registry = getDocumentSourceVisitorRegistry(service);                          \
            registry.registerVisitorFunc<registration_detail::RuleRegisteringVisitorCtx, DS>(    \
                [](DocumentSourceVisitorContextBase* ctx, const DocumentSource&) {               \
                    static_cast<registration_detail::RuleRegisteringVisitorCtx*>(ctx)->addRules( \
                        {__VA_ARGS__});                                                          \
                });                                                                              \
        }};

/**
 * Provides methods for walking and modifying a pipeline. Treats the pipeline as a linked list. Uses
 * the document source visitor registry to track which rules can apply to which document sources.
 */
class PipelineRewriteContext : public RewriteContext<PipelineRewriteContext, DocumentSource> {
public:
    PipelineRewriteContext(Pipeline& pipeline)
        : _container(pipeline.getSources()),
          _itr(_container.begin()),
          _oldItr(_itr),
          _oldDocSource(_itr->get()),
          _registry(getDocumentSourceVisitorRegistry(
              pipeline.getContext()->getOperationContext()->getServiceContext())) {}

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

    /**
     * Returns true if the current stage has changed position or been replaced by another stage.
     * Used to decide if previously applied rules could be reapplied.
     */
    bool didChangePosition() const {
        return !hasMore() || _itr != _oldItr || _oldDocSource != _itr->get();
    }

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
    DocumentSourceContainer::iterator _oldItr;
    DocumentSource* _oldDocSource;

    const DocumentSourceVisitorRegistry& _registry;

    friend struct CommonTransforms;
};

using PipelineRewriteRule = Rule<PipelineRewriteContext>;
using PipelineRewriteEngine = RewriteEngine<PipelineRewriteContext>;

/**
 * Provides a set of common transformations that can be used either directly as transforms or inside
 * transforms to manipulate the pipeline.
 */
struct CommonTransforms {
    static bool swapStageWithPrev(PipelineRewriteContext& ctx);
    static bool swapStageWithNext(PipelineRewriteContext& ctx);
    static bool insertBefore(PipelineRewriteContext& ctx, DocumentSource& d);
    static bool insertAfter(PipelineRewriteContext& ctx, DocumentSource& d);
    static bool erase(PipelineRewriteContext& ctx);
    static bool eraseNext(PipelineRewriteContext& ctx);
    /**
     * Convenience for "sentinel" rules that detect conditions and queue other rules, but may not
     * result in other transformations.
     */
    static inline bool noop(PipelineRewriteContext&) {
        return false;
    }
};

inline bool alwaysTrue(PipelineRewriteContext&) {
    return true;
}

// TODO(SERVER-110107): Remove maybe_unused once these have real usages.
[[maybe_unused]] static auto swapStageWithPrev = CommonTransforms::swapStageWithPrev;
[[maybe_unused]] static auto swapStageWithNext = CommonTransforms::swapStageWithNext;
[[maybe_unused]] static auto insertBefore = CommonTransforms::insertBefore;
[[maybe_unused]] static auto insertAfter = CommonTransforms::insertAfter;
[[maybe_unused]] static auto erase = CommonTransforms::erase;
[[maybe_unused]] static auto eraseNext = CommonTransforms::eraseNext;
[[maybe_unused]] static auto noop = CommonTransforms::noop;

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
