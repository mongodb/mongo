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

// static
const char* ShardFilterStage::kStageType = "SHARDING_FILTER";

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
                    invariant(member->hasObj());

                    // Skip this working set member with a warning - no shard key should not be
                    // possible unless manually inserting data into a shard
                    LOGV2_WARNING(
                        23787,
                        "No shard key found in document, it may have been inserted manually "
                        "into shard",
                        "document"_attr = redact(member->doc.value().toBson()),
                        "keyPattern"_attr = _shardFilterer.getKeyPattern());
                } else {
                    invariant(res == ShardFilterer::DocumentBelongsResult::kDoesNotBelong);
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
