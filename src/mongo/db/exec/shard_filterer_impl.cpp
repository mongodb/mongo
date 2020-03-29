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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/shard_filterer_impl.h"

#include "mongo/db/exec/filter.h"
#include "mongo/db/matcher/matchable.h"

namespace mongo {

ShardFiltererImpl::ShardFiltererImpl(ScopedCollectionFilter cf) : _collectionFilter(std::move(cf)) {
    if (_collectionFilter.isSharded()) {
        _keyPattern = ShardKeyPattern(_collectionFilter.getKeyPattern());
    }
}

ShardFilterer::DocumentBelongsResult ShardFiltererImpl::_shardKeyBelongsToMe(
    const BSONObj shardKey) const {
    if (shardKey.isEmpty()) {
        return DocumentBelongsResult::kNoShardKey;
    }

    return _collectionFilter.keyBelongsToMe(shardKey) ? DocumentBelongsResult::kBelongs
                                                      : DocumentBelongsResult::kDoesNotBelong;
}


ShardFilterer::DocumentBelongsResult ShardFiltererImpl::documentBelongsToMe(
    const WorkingSetMember& wsm) const {
    if (!_collectionFilter.isSharded()) {
        return DocumentBelongsResult::kBelongs;
    }

    if (wsm.hasObj()) {
        return _shardKeyBelongsToMe(_keyPattern->extractShardKeyFromDoc(wsm.doc.value().toBson()));
    }
    // Transform 'IndexKeyDatum' provided by 'wsm' into 'IndexKeyData' to call
    // extractShardKeyFromIndexKeyData().
    invariant(!wsm.keyData.empty());
    std::vector<ShardKeyPattern::IndexKeyData> indexKeyDataVector;
    indexKeyDataVector.resize(wsm.keyData.size());
    for (auto&& indexKeyData : wsm.keyData) {
        indexKeyDataVector.push_back({indexKeyData.keyData, indexKeyData.indexKeyPattern});
    }
    return _shardKeyBelongsToMe(_keyPattern->extractShardKeyFromIndexKeyData(indexKeyDataVector));
}

ShardFilterer::DocumentBelongsResult ShardFiltererImpl::documentBelongsToMe(
    const Document& doc) const {
    if (!_collectionFilter.isSharded()) {
        return DocumentBelongsResult::kBelongs;
    }
    return _shardKeyBelongsToMe(_keyPattern->extractShardKeyFromDoc(doc.toBson()));
}
}  // namespace mongo
