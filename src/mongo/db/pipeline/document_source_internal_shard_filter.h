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

#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/pipeline/document_source.h"

namespace mongo {

/**
 * Filters out documents which are physically present on this shard but not logically owned
 * according to this operation's shard version.
 */
class DocumentSourceInternalShardFilter final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalShardFilter"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceInternalShardFilter(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                      std::unique_ptr<ShardFilterer> shardFilterer);

    const char* getSourceName() const override {
        return kStageName.rawData();
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        return StageConstraints(StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kAnyShard,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kNotAllowed,
                                TransactionRequirement::kNotAllowed,
                                LookupRequirement::kNotAllowed,
                                ChangeStreamRequirement::kBlacklist);
    }

    GetNextResult getNext() override;

    Value serialize(
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const override;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) override;

private:
    std::unique_ptr<ShardFilterer> _shardFilterer;
};

}  // namespace mongo
