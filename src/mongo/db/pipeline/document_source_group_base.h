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
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/group_from_first_document_transformation.h"
#include "mongo/db/pipeline/group_processor.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This class represents a $group stage generically - could be a streaming or hash based group.
 *
 * It contains some common execution code between the two algorithms, such as:
 *  - Handling spilling to disk.
 *  - Computing the group key
 *  - Accumulating values in a hash table and populating output documents.
 */
class DocumentSourceGroupBase : public DocumentSource {
public:
    using Accumulators = std::vector<boost::intrusive_ptr<AccumulatorState>>;
    using GroupsMap = ValueUnorderedMap<Accumulators>;

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;
    boost::intrusive_ptr<DocumentSource> optimize() final;
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final;
    GetModPathsReturn getModifiedPaths() const final;

    /**
     * Returns a map with the fieldPath and expression of the _id field for $group.
     * If _id is a single expression, such as {_id: "$field"}, the function will return {_id:
     * "$field"}.
     * If _id is a nested expression, such as  {_id: {c: "$field"}}, the function will
     * return {_id.c: "$field"}}.
     * Both maps are the same length, even though the original '_id' fields are different.
     */
    StringMap<boost::intrusive_ptr<Expression>> getIdFields() const;

    boost::optional<DistributedPlanLogic> pipelineDependentDistributedPlanLogic(
        const DistributedPlanContext& ctx) final;
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

    /**
     * Can be used to change or swap out individual _id fields, but should not be used
     * once execution has begun.
     */
    std::vector<boost::intrusive_ptr<Expression>>& getMutableIdFields();

    /**
     * Returns all the AccumulationStatements.
     */
    const std::vector<AccumulationStatement>& getAccumulationStatements() const;

    /**
     * Similar to above, but can be used to change or swap out individual accumulated fields.
     * Should not be used once execution has begun.
     */
    std::vector<AccumulationStatement>& getMutableAccumulationStatements();

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kBlocking,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kWritesTmpData,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);
        constraints.canSwapWithMatch = true;
        return constraints;
    }

    GroupProcessor* getGroupProcessor() {
        return _groupProcessor.get();
    }

    /**
     * Returns the expression to use to determine the group id of each document.
     */
    boost::intrusive_ptr<Expression> getIdExpression() const;

    /**
     * Returns true if this $group stage represents a 'global' $group which is merging together
     * results from earlier partial groups.
     */
    bool doingMerge() const {
        return _groupProcessor->doingMerge();
    }

    /**
     * Returns maximum allowed memory footprint.
     */
    size_t getMaxMemoryUsageBytes() const {
        return _groupProcessor->getMemoryTracker().maxAllowedMemoryUsageBytes();
    }

    /**
     * Returns a vector of the _id field names. If the id field is a single expression, this will
     * return an empty vector.
     */
    const std::vector<std::string>& getIdFieldNames() const {
        return _groupProcessor->getIdFieldNames();
    }

    /**
     * Returns a vector of the expressions in the _id field. If the id field is a single expression,
     * this will return a vector with one element.
     */
    const std::vector<boost::intrusive_ptr<Expression>>& getIdExpressions() const {
        return _groupProcessor->getIdExpressions();
    }

    /**
     * Returns a set of paths which the group _id references.
     * Only includes fields which are used unmodified - e.g.,
     *   {foo:{"$add":["$b", "$c"]}, bar:"$d"}
     * returns {"d"}, as the field "d" is used, but has not been altered
     * by any further computation.
     */
    OrderedPathSet getTriviallyReferencedPaths() const;

    bool canRunInParallelBeforeWriteStage(
        const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const final;

    /**
     * When possible, creates a document transformer that transforms the first document in a group
     * into one of the output documents of the $group stage. This is possible when we are grouping
     * on a single field and all accumulators are $first or $top (or there are no accumulators).
     *
     * It is sometimes possible to use a DISTINCT_SCAN to scan the first document of each group,
     * in which case this transformation can replace the actual $group stage in the pipeline
     * (SERVER-9507 & SERVER-84347).
     *
     * If a $group with $top/$bottom accumulator is transformed, its SortPattern is necessary to
     * create a DISTINCT_SCAN plan.
     *
     * Returns:
     * - first: the optional SortPattern of $group's $top or $bottom.
     * - second: The rewritten $group stage.
     */
    std::pair<boost::optional<SortPattern>, std::unique_ptr<GroupFromFirstDocumentTransformation>>
    rewriteGroupAsTransformOnFirstDocument() const;

    // True if this $group can be pushed down to SBE.
    SbeCompatibility sbeCompatibility() const {
        return _sbeCompatibility;
    }

    void setSbeCompatibility(SbeCompatibility sbeCompatibility) {
        _sbeCompatibility = sbeCompatibility;
    }

    bool willBeMerged() const {
        return _groupProcessor->willBeMerged();
    }

    bool groupIsOnShardKey(const Pipeline& pipeline,
                           const boost::optional<OrderedPathSet>& initialShardKeyPaths) const;

protected:
    DocumentSourceGroupBase(StringData stageName,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            boost::optional<int64_t> maxMemoryUsageBytes = boost::none);

    void initializeFromBson(BSONElement elem);
    virtual bool isSpecFieldReserved(StringData fieldName) = 0;

    virtual void serializeAdditionalFields(
        MutableDocument& out, const SerializationOptions& opts = SerializationOptions{}) const {};

    using RewriteGroupRequirements =
        std::tuple<AccumulatorDocumentsNeeded, std::string, boost::optional<SortPattern>>;

    /**
     * If $group is eligible for rewrite of group to transform on first document, this returns a
     * tuple of
     * - The same ExpectedInput across all accumulators.
     * - the id field for grouping.
     * - an optional SortPattern when the needed document is either kFirstOutputDocument or
     */
    boost::optional<RewriteGroupRequirements> getRewriteGroupRequirements() const;

    std::shared_ptr<GroupProcessor> _groupProcessor;

private:
    static constexpr StringData kDoingMergeSpecField = "$doingMerge"_sd;
    static constexpr StringData kWillBeMergedSpecField = "$willBeMerged"_sd;

    /**
     * Returns true if 'dottedPath' is one of the group keys present in '_idExpressions'.
     */
    bool pathIncludedInGroupKeys(const std::string& dottedPath) const;

    SbeCompatibility _sbeCompatibility = SbeCompatibility::notCompatible;
};

}  // namespace mongo
