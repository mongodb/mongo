/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group_base.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"

#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This class represents hash based group implementation that stores all groups until source is
 * depleted and only then starts outputing documents.
 */
class DocumentSourceGroup final : public DocumentSourceGroupBase {
public:
    static constexpr StringData kStageName = "$group"_sd;

    const char* getSourceName() const final;

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

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) override;

    // The $sort/$group with $first/$last is rewritten to use $top/$bottom in $group so that $sort
    // is absorbed into $group. Currently this rewrite is only invoked from time-series.
    //
    // TODO SERVER-28980 will lift the restriction.
    bool tryToAbsorbTopKSort(DocumentSourceSort* prospectiveSort,
                             DocumentSourceContainer::iterator prospectiveSortItr,
                             DocumentSourceContainer* container);

protected:
    bool isSpecFieldReserved(StringData) final {
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
