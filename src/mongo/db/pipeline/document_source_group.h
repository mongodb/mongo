// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group_base.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(Group);

/**
 * This class represents hash based group implementation that stores all groups until source is
 * depleted and only then starts outputing documents.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceGroup final : public DocumentSourceGroupBase {
public:
    static constexpr std::string_view kStageName = "$group"sv;

    std::string_view getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    /**
     * Convenience method for creating a new $group stage. If maxMemoryUsageBytes is boost::none,
     * then it will actually use the value of internalDocumentSourceGroupMaxMemoryBytes.
     */
    static boost::intrusive_ptr<DocumentSourceGroup> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        std::vector<AccumulationStatement> accumulationStatements,
        bool willBeMerged,
        boost::optional<int64_t> maxMemoryUsageBytes = boost::none);

    /**
     * Parses 'elem' into a $group stage, or throws a AssertionException if 'elem' was an invalid
     * specification.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
    static boost::intrusive_ptr<DocumentSource> createFromBsonWithMaxMemoryUsage(
        BSONElement elem,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<int64_t> maxMemoryUsageBytes);

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

    // The $sort/$group with $first/$last is rewritten to use $top/$bottom in $group so that $sort
    // is absorbed into $group. Currently this rewrite is only invoked from time-series.
    //
    // TODO SERVER-28980 will lift the restriction.
    bool tryToAbsorbTopKSort(DocumentSourceSort* prospectiveSort,
                             DocumentSourceContainer::iterator prospectiveSortItr,
                             DocumentSourceContainer* container);

protected:
    bool isSpecFieldReserved(std::string_view) final {
        return false;
    }

    /**
     * This optimization pushes a filter over a renamed grouping field
     * before the group to improve performance.
     *
     * Specifically:
     * $group { _id: {c: $x}, c: {aggregation}},
     * $project { newVar: $_id.c }
     * $match { newVar: "value"}
     * ->
     * $match { x: "value"}
     * $group { _id: {c: $x}, c: {aggregation}},
     * $project { newVar: $_id.c }
     *
     * Note: This optimization will not push over multiple grouping stages
     * or multiple rename stages. Only the last set of group, project, match
     * is taken into account. Furthermore, the optimization addresses specifically
     * the defined sequence of operations to ensure the semantics of filters over arrays. Renaming
     * dotted paths which include arrays change the evaluation of the filter statement and may lead
     * to erroneous results.
     */
    bool pushDotRenamedMatch(DocumentSourceContainer::iterator itr,
                             DocumentSourceContainer* container);

    /**
     * This optimization combines multiple $top(N) or $bottom(N) accumulators that use the same sort
     * pattern into one so that they generate the sort key only once at run-time. For example,
     *
     * {
     *   $group: {
     *     _id: ...,
     *     tm: {$top: {sortBy: {time: -1}, output: "$m"}},
     *     ti: {$top: {sortBy: {time: -1}, output: "$i"}}
     *   }
     * }
     * ->
     * {
     *   $group: {
     *     _id: ...,
     *     ts: {
     *       $top: {
     *         sortBy: {time: -1},
     *         output: {tm: {$ifNull: ["$m", null]}, ti: {$ifNull: ["$i", null]}}
     *       }
     *     }
     *   }
     * },
     * {$project: {tm: "$ts.tm", ti: "$ts.ti"}}
     */
    bool tryToGenerateCommonSortKey(DocumentSourceContainer::iterator itr,
                                    DocumentSourceContainer* container);

    /**
     * This optimization desugars a $topN where N == 1 to a single $top followed by a $addFields to
     * wrap the output in an array. Note that the wrapping in an array needs to happen because $topN
     * is expected to return an array of results as compared to $top.  Similarly for $bottomN,
     * $firstN and $lastN. The goal is to hopefully leverage a distinct scan (if a proper index were
     * to exist). Therefore, all accumulators need to be compatible (e.g. all $first's and
     * $firstN's) for the optimization to apply.
     *
     * {
     *   $group: {
     *     _id: ...,
     *     myField: {$firstN: {input: exprA, n: 1}}
     *   }
     * }
     * ->
     * {
     *   $group: {
     *     _id: ...,
     *     myField: {$first: exprA}
     *   }
     * },
     * {$addFields: {myField: ["$myField"]}
     */
    bool tryToOptimizeAccN(DocumentSourceContainer::iterator itr,
                           DocumentSourceContainer* container);

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceGroupToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    explicit DocumentSourceGroup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 boost::optional<int64_t> maxMemoryUsageBytes = boost::none);
};

}  // namespace mongo
