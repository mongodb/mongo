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

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

/**
 * An internal stage available for testing. Acts as a simple passthrough of intermediate results
 * from the source stage, but forces the pipeline to split at the point where this stage appears
 * (assuming that no earlier splitpoints exist). Takes a single parameter, 'mergeType', which can be
 * one of 'anyShard' or 'mongos' to control where the merge may occur. Omitting this parameter or
 * specifying 'mongos' produces the default merging behaviour; the merge half of the  pipeline will
 * be executed on mongoS if all other stages are eligible, and will be sent to a random
 * participating shard otherwise.
 */
class DocumentSourceInternalSplitPipeline final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalSplitPipeline"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement, const boost::intrusive_ptr<ExpressionContext>&);

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        HostTypeRequirement mergeType,
        boost::optional<ShardId> mergeShardId = boost::none) {
        return new DocumentSourceInternalSplitPipeline(expCtx, mergeType, mergeShardId);
    }

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     _mergeType,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kAllowed,
                                     TransactionRequirement::kAllowed,
                                     _mergeType == HostTypeRequirement::kMongoS
                                         ? LookupRequirement::kNotAllowed
                                         : LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed};
        if (_mergeShardId) {
            constraints.mergeShardId = _mergeShardId;
        }
        return constraints;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    DocumentSourceInternalSplitPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        HostTypeRequirement mergeType,
                                        boost::optional<ShardId> mergeShardId)
        : DocumentSource(kStageName, expCtx), _mergeType(mergeType), _mergeShardId(mergeShardId) {}

    GetNextResult doGetNext() final;
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final override;
    HostTypeRequirement _mergeType = HostTypeRequirement::kNone;

    // Populated with a valid ShardId if this stage was constructed with
    boost::optional<ShardId> _mergeShardId = boost::none;
};

}  // namespace mongo
