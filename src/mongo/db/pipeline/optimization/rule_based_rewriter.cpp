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

#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"

#include "mongo/db/pipeline/document_source.h"

namespace mongo::rule_based_rewrites::pipeline {
namespace {
inline auto prevOrFirstItr(DocumentSourceContainer& container,
                           DocumentSourceContainer::iterator itr) {
    return itr == container.begin() ? itr : std::prev(itr);
}

/**
 * Swaps two adjacent stages in the pipeline. The first iterator must precede the second one.
 */
inline auto swapStages(DocumentSourceContainer& container,
                       DocumentSourceContainer::iterator itr1,
                       DocumentSourceContainer::iterator itr2) {
    tassert(11010012, "Attempted to swap non-adjacent stages", std::next(itr1) == itr2);
    container.splice(itr1, container, itr2);
    // The stage before the pushed before stage may be able to optimize further, if there is
    // such a stage.
    return prevOrFirstItr(container, itr2);
}
}  // namespace

void PipelineRewriteContext::advance() {
    tassert(11010008, "Already at the end of the container", hasMore());
    _itr = std::next(_itr);
}

void PipelineRewriteContext::enqueueRules() {
    auto& ds = current();
    registration_detail::RuleRegisteringVisitorCtx visitorCtx{*this};
    auto queueTransforms = _registry.getConstVisitorFunc<true /*AllowMissing*/>(visitorCtx, ds);
    // Invoke the function pointer returned from the registry. May be a noop for stages with no
    // optimizations registered.
    queueTransforms(&visitorCtx, ds);
}

std::string PipelineRewriteContext::debugString() const {
    str::stream ss;
    ss << "Container (current position " << std::distance(_container.begin(), _itr) << "):\n";
    size_t pos = 0;
    for (auto&& stage : _container) {
        ss << "\t" << pos++ << ": " << stage->serializeToBSONForDebug() << "\n";
    }
    return ss;
}

bool CommonTransforms::swapStageWithNext(PipelineRewriteContext& ctx) {
    tassert(11010009, "Already at the end of the container", ctx.hasMore());
    ctx._itr = swapStages(ctx._container, ctx._itr, std::next(ctx._itr));
    return true;
}

bool CommonTransforms::swapStageWithPrev(PipelineRewriteContext& ctx) {
    tassert(11010011, "Can't swap first stage with prev", !ctx.atFirstStage());
    ctx._itr = swapStages(ctx._container, std::prev(ctx._itr), ctx._itr);
    return true;
}

bool CommonTransforms::insertBefore(PipelineRewriteContext& ctx, DocumentSource& d) {
    ctx._container.insert(ctx._itr, &d);
    ctx._itr = std::prev(ctx._itr);
    return true;
}

bool CommonTransforms::insertAfter(PipelineRewriteContext& ctx, DocumentSource& d) {
    ctx._container.insert(std::next(ctx._itr), &d);
    return false;
}

bool CommonTransforms::erase(PipelineRewriteContext& ctx) {
    ctx._itr = ctx._container.erase(ctx._itr);
    ctx._itr = prevOrFirstItr(ctx._container, ctx._itr);
    return true;
}

bool CommonTransforms::eraseNext(PipelineRewriteContext& ctx) {
    tassert(11010020, "Already at the last stage", !ctx.atLastStage());
    ctx._container.erase(std::next(ctx._itr));
    return false;
}

namespace registration_detail {
namespace {
const auto getRegisteredRuleNames = ServiceContext::declareDecoration<std::set<std::string>>();
}

void enforceUniqueRuleNames(ServiceContext* service,
                            std::vector<Rule<PipelineRewriteContext>> rules) {
    auto& registeredRuleNames = getRegisteredRuleNames(service);
    for (auto&& rule : rules) {
        tassert(11010016,
                str::stream() << "Duplicate rule name \"" << rule.name << '\"',
                registeredRuleNames.insert(rule.name).second);
    }
}
}  // namespace registration_detail

}  // namespace mongo::rule_based_rewrites::pipeline
