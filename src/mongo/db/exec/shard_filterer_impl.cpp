// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/shard_filterer_impl.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/util/assert_util.h"

#include <utility>
#include <vector>

namespace mongo {

std::unique_ptr<ShardFilterer> ShardFiltererImpl::clone() const {
    return std::make_unique<ShardFiltererImpl>(_collectionFilter);
}

ShardFiltererImpl::ShardFiltererImpl(ScopedCollectionFilter cf)
    : _collectionFilter(std::move(cf)) {}

ShardFilterer::DocumentBelongsResult ShardFiltererImpl::keyBelongsToMeHelper(
    const BSONObj& shardKey) const {
    if (shardKey.isEmpty()) {
        return DocumentBelongsResult::kNoShardKey;
    }

    return keyBelongsToMe(shardKey) ? DocumentBelongsResult::kBelongs
                                    : DocumentBelongsResult::kDoesNotBelong;
}

ShardFilterer::DocumentBelongsResult ShardFiltererImpl::documentBelongsToMe(
    const WorkingSetMember& wsm) const {
    if (!_collectionFilter.isSharded()) {
        return DocumentBelongsResult::kBelongs;
    }

    if (wsm.hasObj()) {
        return keyBelongsToMeHelper(_collectionFilter.getShardKeyPattern().extractShardKeyFromDoc(
            wsm.doc.value().toBson()));
    }
    // Transform 'IndexKeyDatum' provided by 'wsm' into 'IndexKeyData' to call
    // extractShardKeyFromIndexKeyData().
    invariant(!wsm.keyData.empty());
    std::vector<ShardKeyPattern::IndexKeyData> indexKeyDataVector;
    indexKeyDataVector.resize(wsm.keyData.size());
    for (auto&& indexKeyData : wsm.keyData) {
        indexKeyDataVector.push_back({indexKeyData.keyData, indexKeyData.indexKeyPattern});
    }
    return keyBelongsToMeHelper(
        _collectionFilter.getShardKeyPattern().extractShardKeyFromIndexKeyData(indexKeyDataVector));
}

ShardFilterer::DocumentBelongsResult ShardFiltererImpl::documentBelongsToMe(
    const BSONObj& doc) const {
    if (!_collectionFilter.isSharded()) {
        return DocumentBelongsResult::kBelongs;
    }
    return keyBelongsToMeHelper(_collectionFilter.getShardKeyPattern().extractShardKeyFromDoc(doc));
}

const KeyPattern& ShardFiltererImpl::getKeyPattern() const {
    return _collectionFilter.getShardKeyPattern().getKeyPattern();
}

size_t ShardFiltererImpl::getApproximateSize() const {
    // _collectionFilter contains a pointer to metadata but it doesn't own this metadata, so we
    // don't account for the size of the metadata here.
    return sizeof(ShardFiltererImpl);
}
}  // namespace mongo
