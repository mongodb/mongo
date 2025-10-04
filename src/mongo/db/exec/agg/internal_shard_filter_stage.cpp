/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/internal_shard_filter_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<mongo::exec::agg::Stage> internalShardFilterToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* dsInternalShardFilter =
        dynamic_cast<DocumentSourceInternalShardFilter*>(documentSource.get());
    tassert(10980300, "expected 'DocumentSourceInternalShardFilter' type", dsInternalShardFilter);
    return make_intrusive<mongo::exec::agg::InternalShardFilterStage>(
        dsInternalShardFilter->kStageName,
        dsInternalShardFilter->getExpCtx(),
        dsInternalShardFilter->_shardFilterer);
}

namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(internalShardFilterStage,
                           DocumentSourceInternalShardFilter::id,
                           internalShardFilterToStageFn);

InternalShardFilterStage::InternalShardFilterStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::shared_ptr<ShardFilterer>& shardFilterer)
    : Stage(stageName, expCtx), _shardFilterer(shardFilterer) {}

GetNextResult InternalShardFilterStage::doGetNext() {
    auto next = pSource->getNext();
    invariant(_shardFilterer);
    for (; next.isAdvanced(); next = pSource->getNext()) {
        const auto belongsRes = _shardFilterer->documentBelongsToMe(next.getDocument().toBson());
        if (belongsRes == ShardFilterer::DocumentBelongsResult::kBelongs) {
            return next;
        }

        if (belongsRes == ShardFilterer::DocumentBelongsResult::kNoShardKey) {
            LOGV2_WARNING(23870,
                          "no shard key found in document {next_getDocument_toBson} for shard key "
                          "pattern {shardFilterer_getKeyPattern}, document may have been inserted "
                          "manually into shard",
                          "next_getDocument_toBson"_attr = redact(next.getDocument().toBson()),
                          "shardFilterer_getKeyPattern"_attr = _shardFilterer->getKeyPattern());
        }

        // For performance reasons, a streaming stage must not keep references to documents across
        // calls to getNext(). Such stages must retrieve a result from their child and then release
        // it (or return it) before asking for another result. Failing to do so can result in extra
        // work, since the Document/Value library must copy data on write when that data has a
        // refcount above one.
        next.releaseDocument();
    }
    return next;
}

}  // namespace exec::agg
}  // namespace mongo
