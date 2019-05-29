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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_internal_shard_filter.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/util/log.h"

namespace mongo {

//
// This DocumentSource is not registered and can only be created as part of expansions for other
// DocumentSources.
//

DocumentSourceInternalShardFilter::DocumentSourceInternalShardFilter(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::unique_ptr<ShardFilterer> shardFilterer)
    : DocumentSource(pExpCtx), _shardFilterer(std::move(shardFilterer)) {}

DocumentSource::GetNextResult DocumentSourceInternalShardFilter::getNext() {
    pExpCtx->checkForInterrupt();

    auto next = pSource->getNext();
    invariant(_shardFilterer);
    for (; next.isAdvanced(); next = pSource->getNext()) {
        BSONMatchableDocument matchable(next.getDocument().toBson());
        const auto belongsRes = _shardFilterer->documentBelongsToMe(matchable);
        if (belongsRes == ShardFilterer::DocumentBelongsResult::kBelongs) {
            return next;
        }

        if (belongsRes == ShardFilterer::DocumentBelongsResult::kNoShardKey) {
            warning() << "no shard key found in document " << redact(next.getDocument().toBson())
                      << " "
                      << "for shard key pattern " << _shardFilterer->getKeyPattern() << ", "
                      << "document may have been inserted manually into shard";
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

Pipeline::SourceContainer::iterator DocumentSourceInternalShardFilter::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (_shardFilterer->isCollectionSharded()) {
        return std::next(itr);
    }

    if (itr == container->begin()) {
        // Delete this stage from the pipeline if the operation does not require shard versioning.
        container->erase(itr);
        return container->begin();
    }

    auto ret = std::prev(itr);
    container->erase(itr);
    return ret;
}

Value DocumentSourceInternalShardFilter::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(DOC(getSourceName() << Document()));
}

}  // namespace mongo
