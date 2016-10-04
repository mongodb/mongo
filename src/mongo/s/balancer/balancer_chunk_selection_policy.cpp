/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/balancer/balancer_chunk_selection_policy.h"

#include "mongo/util/mongoutils/str.h"

namespace mongo {

BalancerChunkSelectionPolicy::BalancerChunkSelectionPolicy() = default;

BalancerChunkSelectionPolicy::~BalancerChunkSelectionPolicy() = default;

BalancerChunkSelectionPolicy::SplitInfo::SplitInfo(ShardId inShardId,
                                                   NamespaceString inNss,
                                                   ChunkVersion inCollectionVersion,
                                                   ChunkVersion inChunkVersion,
                                                   const BSONObj& inMinKey,
                                                   const BSONObj& inMaxKey,
                                                   std::vector<BSONObj> inSplitKeys)
    : shardId(std::move(inShardId)),
      nss(std::move(inNss)),
      collectionVersion(inCollectionVersion),
      chunkVersion(inChunkVersion),
      minKey(inMinKey),
      maxKey(inMaxKey),
      splitKeys(std::move(inSplitKeys)) {}

std::string BalancerChunkSelectionPolicy::SplitInfo::toString() const {
    StringBuilder splitKeysBuilder;
    for (const auto& splitKey : splitKeys) {
        splitKeysBuilder << splitKey.toString() << ", ";
    }

    return str::stream() << "Splitting chunk in " << nss.ns() << " [ " << minKey << ", " << maxKey
                         << "), residing on " << shardId << " at [ " << splitKeysBuilder.str()
                         << " ] with version " << chunkVersion.toString()
                         << " and collection version " << collectionVersion.toString();
}

}  // namespace mongo
