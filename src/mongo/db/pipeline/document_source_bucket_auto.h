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
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/granularity_rounder.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * The $bucketAuto stage takes a user-specified number of buckets and automatically determines
 * boundaries such that the values are approximately equally distributed between those buckets.
 */
class DocumentSourceBucketAuto final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$bucketAuto"_sd;
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    const char* getSourceName() const final;
    boost::intrusive_ptr<DocumentSource> optimize() final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return {StreamType::kBlocking,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kWritesTmpData,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    /**
     * The $bucketAuto stage must be run on the merging shard.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    /**
     * Convenience method to create a $bucketAuto stage.
     *
     * If 'accumulationStatements' is the empty vector, it will be filled in with the statement
     * 'count: {$sum: 1}'.
     */
    static boost::intrusive_ptr<DocumentSourceBucketAuto> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        int numBuckets,
        std::vector<AccumulationStatement> accumulationStatements = {},
        const boost::intrusive_ptr<GranularityRounder>& granularityRounder = nullptr);

    /**
     * Parses a $bucketAuto stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    /**
     * Returns the groupBy expression. The mutable getter can be used to alter
     * the expression, but should not be used after execution has begun.
     */
    boost::intrusive_ptr<Expression> getGroupByExpression() const;
    boost::intrusive_ptr<Expression>& getMutableGroupByExpression();
    /**
     * Returns the AccumulationStatements. The mutable getter can be used to alter
     * the expression, but should not be used after execution has begun.
     */
    const std::vector<AccumulationStatement>& getAccumulationStatements() const;
    std::vector<AccumulationStatement>& getMutableAccumulationStatements();

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceBucketAutoToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    DocumentSourceBucketAuto(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                             const boost::intrusive_ptr<Expression>& groupByExpression,
                             int numBuckets,
                             std::vector<AccumulationStatement> accumulationStatements,
                             const boost::intrusive_ptr<GranularityRounder>& granularityRounder);

    std::shared_ptr<std::vector<AccumulationStatement>> _accumulatedFields;

    std::shared_ptr<bool> _populated;
    boost::intrusive_ptr<Expression> _groupByExpression;
    boost::intrusive_ptr<GranularityRounder> _granularityRounder;
    int _nBuckets;
};

}  // namespace mongo
