/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/s/shard_version_factory.h"

namespace mongo {

ShardVersion ShardVersionFactory::make(
    const ChunkManager& chunkManager, const boost::optional<CollectionIndexes>& collectionIndexes) {
    tassert(7288900,
            str::stream() << "Cannot create ShardVersion when placement version has uuid "
                          << chunkManager.getUUID() << " and index version has uuid "
                          << collectionIndexes->uuid(),
            !collectionIndexes || chunkManager.uuidMatches(collectionIndexes->uuid()));
    return ShardVersion(chunkManager.getVersion(), collectionIndexes);
}

ShardVersion ShardVersionFactory::make(
    const ChunkManager& chunkManager,
    const ShardId& shardId,
    const boost::optional<CollectionIndexes>& collectionIndexes) {

    tassert(7288901,
            str::stream() << "Cannot create ShardVersion when placement version has uuid "
                          << chunkManager.getUUID() << " and index version has uuid "
                          << collectionIndexes->uuid(),
            !collectionIndexes || chunkManager.uuidMatches(collectionIndexes->uuid()));
    return ShardVersion(chunkManager.getVersion(shardId), collectionIndexes);
}

ShardVersion ShardVersionFactory::make(
    const CollectionMetadata& cm, const boost::optional<CollectionIndexes>& collectionIndexes) {
    tassert(7288902,
            str::stream() << "Cannot create ShardVersion when placement version has uuid "
                          << cm.getUUID() << " and index version has uuid "
                          << collectionIndexes->uuid(),
            !collectionIndexes || !cm.isSharded() || cm.uuidMatches(collectionIndexes->uuid()));
    return ShardVersion(cm.getShardVersion(), collectionIndexes);
}


// The other three constructors should be used instead of this one whenever possible. This
// builder should only be used for the rare cases in which we know that the chunk version and
// collection indexes come from the same collection.
ShardVersion ShardVersionFactory::make(
    const ChunkVersion& chunkVersion, const boost::optional<CollectionIndexes>& collectionIndexes) {
    return ShardVersion(chunkVersion, collectionIndexes);
}
}  // namespace mongo
