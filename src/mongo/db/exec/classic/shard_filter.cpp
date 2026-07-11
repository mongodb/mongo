// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/classic/shard_filter.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

using std::unique_ptr;


ShardFilterStage::ShardFilterStage(ExpressionContext* expCtx,
                                   ScopedCollectionFilter collectionFilter,
                                   WorkingSet* ws,
                                   std::unique_ptr<PlanStage> child)
    : PlanStage(kStageType, expCtx), _ws(ws), _shardFilterer(std::move(collectionFilter)) {
    _children.emplace_back(std::move(child));
}

ShardFilterStage::~ShardFilterStage() {}

bool ShardFilterStage::isEOF() const {
    return child()->isEOF();
}

PlanStage::StageState ShardFilterStage::doWork(WorkingSetID* out) {
    // If we've returned as many results as we're limited to, isEOF will be true.
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    StageState status = child()->work(out);

    if (PlanStage::ADVANCED == status) {
        // If we're sharded make sure that we don't return data that is not owned by us,
        // including pending documents from in-progress migrations and orphaned documents from
        // aborted migrations
        if (_shardFilterer.isCollectionSharded()) {
            WorkingSetMember* member = _ws->get(*out);
            ShardFilterer::DocumentBelongsResult res = _shardFilterer.documentBelongsToMe(*member);
            if (res != ShardFilterer::DocumentBelongsResult::kBelongs) {
                if (res == ShardFilterer::DocumentBelongsResult::kNoShardKey) {
                    // We can't find a shard key for this working set member - this should never
                    // happen with a non-fetched result unless our query planning is screwed up
                    tassert(11051631,
                            "Expecting working set member to have an object",
                            member->hasObj());

                    // Skip this working set member with a warning - no shard key should not be
                    // possible unless manually inserting data into a shard
                    LOGV2_WARNING(
                        23787,
                        "No shard key found in document, it may have been inserted manually "
                        "into shard",
                        "document"_attr = redact(member->doc.value().toBson()),
                        "keyPattern"_attr = _shardFilterer.getKeyPattern());
                }

                // If the document had no shard key, or doesn't belong to us, skip it.
                _ws->free(*out);
                ++_specificStats.chunkSkips;
                return PlanStage::NEED_TIME;
            }
        }

        // If we're here either we have shard state and our doc passed, or we have no shard
        // state.  Either way, we advance.
        return status;
    }

    return status;
}

unique_ptr<PlanStageStats> ShardFilterStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_SHARDING_FILTER);
    ret->children.emplace_back(child()->getStats());
    ret->specific = std::make_unique<ShardingFilterStats>(_specificStats);
    return ret;
}

const SpecificStats* ShardFilterStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
