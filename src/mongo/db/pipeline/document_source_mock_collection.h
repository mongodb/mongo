/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_queue.h"

namespace mongo {

/**
 * This is pretty much funcitonally equivalent to $documents or $queue, but it pretends to be a
 * collection instead so it is usable on a normal namespace rather than a "collectionless" one.
 *
 * TODO SERVER-65534 this stage also works around the restriction that $documents must run on
 * mongos.
 */
class DocumentSourceMockCollection : public DocumentSourceQueue {
public:
    static constexpr StringData kStageName = "$mockCollection"_sd;

    DocumentSourceMockCollection(std::deque<GetNextResult> results,
                                 const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceQueue(std::move(results), expCtx, kStageName) {}

    ~DocumentSourceMockCollection() override = default;

    const char* getSourceName() const override {
        return kStageName.rawData();
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const override;

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        auto queueConstraints = DocumentSourceQueue::constraints(pipeState);
        // Technically this stage is independent in that it doesn't read any data. But the point of
        // this stage is to replace a real collection and pretend that it is the collection. So in
        // order to permit using this stage with a "real" (non-collectionless) namespace, we need to
        // set this to false.
        queueConstraints.isIndependentOfAnyCollection = false;
        // TODO SERVER-65534 This is a hacky workaround. In order to get this stage to work in a
        // pipeline followed by another stage which must run on a shard, we should be able to
        // forward the entire pipeline to a shard. That kind of 'host type requirement' doesn't
        // exist yet.
        queueConstraints.hostRequirement = StageConstraints::HostTypeRequirement::kNone;
        queueConstraints.requiredPosition = StageConstraints::PositionRequirement::kNone;
        return queueConstraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        // TODO SERVER-65534 This is a hacky workaround. In order to get this stage to work in a
        // pipeline with another stage which must run on a shard, we should be able to forward the
        // entire pipeline to a shard. Until we know how to do that though, it is correct but not
        // performant to do this instead:

        // {shards stage, merging stage, merge sort}.
        return DistributedPlanLogic{
            // We will ignore all results, so add a limit to reduce perf impact.
            DocumentSourceLimit::create(pExpCtx, 1),
            // This needs to happen in the merging half so that we don't repeat the data N
            // times where N is the number of shards with at least one chunk for this
            // collection.
            this,
            boost::none};
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
};

}  // namespace mongo
