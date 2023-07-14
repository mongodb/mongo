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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/group_from_first_document_transformation.h"
#include "mongo/db/pipeline/group_processor.h"
#include "mongo/db/pipeline/memory_usage_tracker.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/string_map.h"

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

    Value serialize(SerializationOptions opts = SerializationOptions()) const final override;
    boost::intrusive_ptr<DocumentSource> optimize() final;
    DepsTracker::State getDependencies(DepsTracker* deps) const final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final;
    GetModPathsReturn getModifiedPaths() const final;
    StringMap<boost::intrusive_ptr<Expression>> getIdFields() const;

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

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
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
        return &_groupProcessor;
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
        return _groupProcessor.doingMerge();
    }

    const SpecificStats* getSpecificStats() const final {
        return &_groupProcessor.getStats();
    }

    /**
     * Returns true if this $group stage used disk during execution and false otherwise.
     */
    bool usedDisk() final {
        return _groupProcessor.usedDisk();
    }

    /**
     * Returns maximum allowed memory footprint.
     */
    size_t getMaxMemoryUsageBytes() const {
        return _groupProcessor.getMemoryTracker()._maxAllowedMemoryUsageBytes;
    }

    bool canRunInParallelBeforeWriteStage(
        const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const final;

    /**
     * When possible, creates a document transformer that transforms the first document in a group
     * into one of the output documents of the $group stage. This is possible when we are grouping
     * on a single field and all accumulators are $first (or there are no accumluators).
     *
     * It is sometimes possible to use a DISTINCT_SCAN to scan the first document of each group,
     * in which case this transformation can replace the actual $group stage in the pipeline
     * (SERVER-9507).
     */
    std::unique_ptr<GroupFromFirstDocumentTransformation> rewriteGroupAsTransformOnFirstDocument()
        const;

    // True if this $group can be pushed down to SBE.
    SbeCompatibility sbeCompatibility() const {
        return _sbeCompatibility;
    }

protected:
    DocumentSourceGroupBase(StringData stageName,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            boost::optional<size_t> maxMemoryUsageBytes = boost::none);

    virtual ~DocumentSourceGroupBase();

    void initializeFromBson(BSONElement elem);
    virtual bool isSpecFieldReserved(StringData fieldName) = 0;

    void doDispose() final;

    virtual void serializeAdditionalFields(
        MutableDocument& out, SerializationOptions opts = SerializationOptions()) const {};

    GroupProcessor _groupProcessor;

private:
    /**
     * Returns true if 'dottedPath' is one of the group keys present in '_idExpressions'.
     */
    bool pathIncludedInGroupKeys(const std::string& dottedPath) const;

    SbeCompatibility _sbeCompatibility = SbeCompatibility::notCompatible;
};

}  // namespace mongo
