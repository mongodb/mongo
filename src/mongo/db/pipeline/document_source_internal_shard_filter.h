/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/exec_shard_filter_policy.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <memory>
#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Filters out documents which are physically present on this shard but not logically owned
 * according to this operation's shard version.
 */
class DocumentSourceInternalShardFilter final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalShardFilter"_sd;

    /**
     * Examines the state of the OperationContext (attached to 'expCtx') to determine if this
     * operation is expected to be shard versioned. If so, builds and returns a
     * DocumentSourceInternalShardFilter. If not, returns nullptr.
     */
    static boost::intrusive_ptr<DocumentSourceInternalShardFilter> buildIfNecessary(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceInternalShardFilter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      std::unique_ptr<ShardFilterer> shardFilterer);

    const char* getSourceName() const override {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        return StageConstraints(StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kAnyShard,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kNotAllowed,
                                TransactionRequirement::kNotAllowed,
                                LookupRequirement::kNotAllowed,
                                UnionRequirement::kNotAllowed,
                                ChangeStreamRequirement::kDenylist);
    }


    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) override;

    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        // This stage doesn't use any variables.
        if (_shardFilterer->isCollectionSharded()) {
            const BSONObj& keyPattern = _shardFilterer->getKeyPattern().toBSON();
            for (BSONElement elem : keyPattern) {
                deps->fields.insert(elem.fieldName());
            }
        }
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    const auto& shardFilterer() {
        return *_shardFilterer;
    }

private:
    friend boost::intrusive_ptr<mongo::exec::agg::Stage> internalShardFilterToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    std::shared_ptr<ShardFilterer> _shardFilterer;
};

inline ExecShardFilterPolicy buildExecShardFilterPolicy(
    const boost::intrusive_ptr<DocumentSourceInternalShardFilter>& maybeShardFilterer) {
    if (maybeShardFilterer) {
        return ProofOfUpstreamFiltering{maybeShardFilterer->shardFilterer()};
    }
    return AutomaticShardFiltering{};
}

}  // namespace mongo
