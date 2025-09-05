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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/intrusive_counter.h"

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class DocumentSourceLimit final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$limit"_sd;

    /**
     * Create a new $limit stage.
     */
    static boost::intrusive_ptr<DocumentSourceLimit> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    /**
     * Parse a $limit stage from a BSON stage specification. 'elem's field name must be "$limit".
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return StageConstraints{StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed};
    }

    const char* getSourceName() const final {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    /**
     * Attempts to combine with a subsequent $limit stage, setting 'limit' appropriately.
     */
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::SEE_NEXT;  // This doesn't affect needed fields
    }

    /**
     * Returns a DistributedPlanLogic with two identical $limit stages; one for the shards pipeline
     * and one for the merging pipeline.
     */
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        // Running this stage on the shards is an optimization, but is not strictly necessary in
        // order to produce correct pipeline output.
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{
            this, DocumentSourceLimit::create(getExpCtx(), _limit), boost::none};
    }

    long long getLimit() const {
        return _limit;
    }
    void setLimit(long long newLimit) {
        _limit = newLimit;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    DocumentSourceLimit(const boost::intrusive_ptr<ExpressionContext>& pExpCtx, long long limit);

    long long _limit;
};

}  // namespace mongo
