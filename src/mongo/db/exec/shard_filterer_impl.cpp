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
