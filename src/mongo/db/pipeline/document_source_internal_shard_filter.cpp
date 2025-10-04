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


#include "mongo/db/pipeline/document_source_internal_shard_filter.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/util/assert_util.h"

#include <iterator>
#include <list>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

//
// This DocumentSource is not registered and can only be created as part of expansions for other
// DocumentSources.
//

ALLOCATE_DOCUMENT_SOURCE_ID(_internalShardFilter, DocumentSourceInternalShardFilter::id)

boost::intrusive_ptr<DocumentSourceInternalShardFilter>
DocumentSourceInternalShardFilter::buildIfNecessary(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto opCtx = expCtx->getOperationContext();
    // We can only rely on the ownership filter if the operation is coming from the router
    // (i.e. it is versioned).
    if (!OperationShardingState::isComingFromRouter(opCtx)) {
        return nullptr;
    }
    static const auto orphanPolicy =
        CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup;
    return make_intrusive<DocumentSourceInternalShardFilter>(
        expCtx,
        std::make_unique<ShardFiltererImpl>(
            CollectionShardingState::acquire(opCtx, expCtx->getNamespaceString())
                ->getOwnershipFilter(opCtx, orphanPolicy)));
}

DocumentSourceInternalShardFilter::DocumentSourceInternalShardFilter(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::unique_ptr<ShardFilterer> shardFilterer)
    : DocumentSource(kStageName, pExpCtx), _shardFilterer(std::move(shardFilterer)) {}

DocumentSourceContainer::iterator DocumentSourceInternalShardFilter::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
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

Value DocumentSourceInternalShardFilter::serialize(const SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << Document()));
}

}  // namespace mongo
