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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {
namespace {

const ReadPreferenceSetting kConfigPrimarySelector(ReadPreference::PrimaryOnly);
const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

/**
 * Checks if the given key range for the given namespace conflicts with an existing key range.
 * Note: range should have the full shard key.
 * Returns ErrorCodes::RangeOverlapConflict is an overlap is detected.
 */
Status checkForOverlappingZonedKeyRange(OperationContext* opCtx,
                                        Shard* configServer,
                                        const NamespaceString& nss,
                                        const ChunkRange& range,
                                        const std::string& zoneName,
                                        const KeyPattern& shardKeyPattern) {
    DistributionStatus chunkDist(nss, ShardToChunksMap{});

    auto tagStatus = configServer->exhaustiveFindOnConfig(opCtx,
                                                          kConfigPrimarySelector,
                                                          repl::ReadConcernLevel::kLocalReadConcern,
                                                          TagsType::ConfigNS,
                                                          BSON(TagsType::ns(nss.ns())),
                                                          BSONObj(),
                                                          0);
    if (!tagStatus.isOK()) {
        return tagStatus.getStatus();
    }

    const auto& tagDocList = tagStatus.getValue().docs;
    for (const auto& tagDoc : tagDocList) {
        auto tagParseStatus = TagsType::fromBSON(tagDoc);
        if (!tagParseStatus.isOK()) {
            return tagParseStatus.getStatus();
        }

        // Always extend ranges to full shard key to be compatible with tags created before
        // the zone commands were implemented.
        const auto& parsedTagDoc = tagParseStatus.getValue();
        auto overlapStatus = chunkDist.addRangeToZone(
            ZoneRange(shardKeyPattern.extendRangeBound(parsedTagDoc.getMinKey(), false),
                      shardKeyPattern.extendRangeBound(parsedTagDoc.getMaxKey(), false),
                      parsedTagDoc.getTag()));
        if (!overlapStatus.isOK()) {
            return overlapStatus;
        }
    }

    auto overlapStatus =
        chunkDist.addRangeToZone(ZoneRange(range.getMin(), range.getMax(), zoneName));
    if (!overlapStatus.isOK()) {
        return overlapStatus;
    }

    return Status::OK();
}

/**
 * Returns a new range based on the given range with the full shard key.
 * Returns:
 * - ErrorCodes::NamespaceNotSharded if nss is not sharded.
 * - ErrorCodes::ShardKeyNotFound if range is not compatible (for example, not a prefix of shard
 * key) with the shard key of nss.
 */
ChunkRange includeFullShardKey(OperationContext* opCtx,
                               Shard* configServer,
                               const NamespaceString& nss,
                               const ChunkRange& range,
                               KeyPattern* shardKeyPatternOut) {
    auto findCollResult = uassertStatusOK(configServer->exhaustiveFindOnConfig(
                                              opCtx,
                                              kConfigPrimarySelector,
                                              repl::ReadConcernLevel::kLocalReadConcern,
                                              CollectionType::ConfigNS,
                                              BSON(CollectionType::kNssFieldName << nss.ns()),
                                              BSONObj(),
                                              1))
                              .docs;
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << nss.ns() << " is not sharded",
            !findCollResult.empty());

    CollectionType coll(findCollResult.front());
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << nss.ns() << " is not sharded",
            !coll.getDropped());

    const auto& shardKeyPattern = coll.getKeyPattern();
    const auto& shardKeyBSON = shardKeyPattern.toBSON();
    *shardKeyPatternOut = shardKeyPattern;

    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "min: " << range.getMin() << " is not a prefix of the shard key "
                          << shardKeyBSON << " of ns: " << nss.ns(),
            range.getMin().isFieldNamePrefixOf(shardKeyBSON));
    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "max: " << range.getMax() << " is not a prefix of the shard key "
                          << shardKeyBSON << " of ns: " << nss.ns(),
            range.getMax().isFieldNamePrefixOf(shardKeyBSON));

    return ChunkRange(shardKeyPattern.extendRangeBound(range.getMin(), false),
                      shardKeyPattern.extendRangeBound(range.getMax(), false));
}

/**
 * Checks whether every hashed field in the given shard key pattern corresponds to a field of type
 * NumberLong, MinKey, or MaxKey in the provided chunk range. Returns ErrorCodes::InvalidOptions if
 * there exists a field violating this constraint.
 */
Status checkHashedShardKeyRange(const ChunkRange& range, const KeyPattern& shardKeyPattern) {
    BSONObjIterator rangeMin(range.getMin());
    BSONObjIterator rangeMax(range.getMax());
    BSONObjIterator shardKey(shardKeyPattern.toBSON());

    while (shardKey.more()) {
        auto shardKeyField = shardKey.next();
        auto rangeMinField = rangeMin.next();
        auto rangeMaxField = rangeMax.next();

        if (ShardKeyPattern::isHashedPatternEl(shardKeyField) &&
            (!ShardKeyPattern::isValidHashedValue(rangeMinField) ||
             !ShardKeyPattern::isValidHashedValue(rangeMaxField))) {
            return {ErrorCodes::InvalidOptions,
                    str::stream() << "there exists a field in the range " << range.getMin()
                                  << " -->> " << range.getMax()
                                  << " not of type NumberLong, MinKey, or MaxKey which corresponds "
                                     "to a hashed field in the shard key pattern "
                                  << shardKeyPattern};
        }
    }

    return Status::OK();
}

}  // namespace

Status ShardingCatalogManager::addShardToZone(OperationContext* opCtx,
                                              const std::string& shardName,
                                              const std::string& zoneName) {
    Lock::ExclusiveLock lk(opCtx->lockState(), _kZoneOpLock);

    auto updateStatus = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        ShardType::ConfigNS,
        BSON(ShardType::name(shardName)),
        BSON("$addToSet" << BSON(ShardType::tags() << zoneName)),
        false,
        kNoWaitWriteConcern);

    if (!updateStatus.isOK()) {
        return updateStatus.getStatus();
    }

    if (!updateStatus.getValue()) {
        return {ErrorCodes::ShardNotFound,
                str::stream() << "shard " << shardName << " does not exist"};
    }

    return Status::OK();
}

Status ShardingCatalogManager::removeShardFromZone(OperationContext* opCtx,
                                                   const std::string& shardName,
                                                   const std::string& zoneName) {
    Lock::ExclusiveLock lk(opCtx->lockState(), _kZoneOpLock);

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    const NamespaceString shardNS(ShardType::ConfigNS);

    //
    // Check whether the shard even exist in the first place.
    //

    auto findShardExistsStatus =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            kConfigPrimarySelector,
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            shardNS,
                                            BSON(ShardType::name() << shardName),
                                            BSONObj(),
                                            1);

    if (!findShardExistsStatus.isOK()) {
        return findShardExistsStatus.getStatus();
    }

    if (findShardExistsStatus.getValue().docs.size() == 0) {
        return {ErrorCodes::ShardNotFound,
                str::stream() << "shard " << shardName << " does not exist"};
    }

    //
    // Ensure the shard is not only shard that the zone belongs to. Otherwise, the zone must
    // not have any chunk ranges associated with it.
    //
    auto isRequiredByZone =
        _isShardRequiredByZoneStillInUse(opCtx, kConfigPrimarySelector, shardName, zoneName);

    if (!isRequiredByZone.isOK()) {
        return isRequiredByZone.getStatus();
    }

    if (isRequiredByZone.getValue()) {
        return {ErrorCodes::ZoneStillInUse,
                "cannot remove a shard from zone if a chunk range is associated with it"};
    }

    //
    // Perform update.
    //

    auto updateStatus = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        ShardType::ConfigNS,
        BSON(ShardType::name(shardName)),
        BSON("$pull" << BSON(ShardType::tags() << zoneName)),
        false,
        kNoWaitWriteConcern);

    if (!updateStatus.isOK()) {
        return updateStatus.getStatus();
    }

    // The update did not match a document, another thread could have removed it.
    if (!updateStatus.getValue()) {
        return {ErrorCodes::ShardNotFound,
                str::stream() << "shard " << shardName << " no longer exist"};
    }

    return Status::OK();
}


void ShardingCatalogManager::assignKeyRangeToZone(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const ChunkRange& givenRange,
                                                  const std::string& zoneName) {
    uassertStatusOK(ShardKeyPattern::checkShardKeyIsValidForMetadataStorage(givenRange.getMin()));
    uassertStatusOK(ShardKeyPattern::checkShardKeyIsValidForMetadataStorage(givenRange.getMax()));

    auto configServer = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    Lock::ExclusiveLock lk(opCtx->lockState(), _kZoneOpLock);

    auto zoneDoc = uassertStatusOK(configServer->exhaustiveFindOnConfig(
                                       opCtx,
                                       kConfigPrimarySelector,
                                       repl::ReadConcernLevel::kLocalReadConcern,
                                       ShardType::ConfigNS,
                                       BSON(ShardType::tags() << zoneName),
                                       BSONObj(),
                                       1))
                       .docs;
    uassert(ErrorCodes::ZoneNotFound,
            str::stream() << "zone " << zoneName << " does not exist",
            !zoneDoc.empty());

    ChunkRange actualRange = givenRange;
    KeyPattern keyPattern;
    try {
        actualRange = includeFullShardKey(opCtx, configServer.get(), nss, givenRange, &keyPattern);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotSharded>&) {
        // range remains the same as 'givenRange'
        uassertStatusOK(givenRange.extractKeyPattern(&keyPattern));
    }

    uassertStatusOK(checkHashedShardKeyRange(actualRange, keyPattern));
    uassertStatusOK(checkForOverlappingZonedKeyRange(
        opCtx, configServer.get(), nss, actualRange, zoneName, keyPattern));

    BSONObjBuilder updateBuilder;
    updateBuilder.append(TagsType::ns(), nss.ns());
    updateBuilder.append(TagsType::min(), actualRange.getMin());
    updateBuilder.append(TagsType::max(), actualRange.getMax());
    updateBuilder.append(TagsType::tag(), zoneName);

    uassertStatusOK(Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        TagsType::ConfigNS,
        BSON(TagsType::ns(nss.ns()) << TagsType::min(actualRange.getMin())),
        updateBuilder.obj(),
        true,
        kNoWaitWriteConcern));
}

void ShardingCatalogManager::removeKeyRangeFromZone(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const ChunkRange& givenRange) {
    auto configServer = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    Lock::ExclusiveLock lk(opCtx->lockState(), _kZoneOpLock);

    ChunkRange actualRange = givenRange;
    KeyPattern keyPattern;
    try {
        actualRange = includeFullShardKey(opCtx, configServer.get(), nss, givenRange, &keyPattern);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotSharded>&) {
        // range remains the same as 'givenRange'
        uassertStatusOK(givenRange.extractKeyPattern(&keyPattern));
    }

    BSONObjBuilder removeBuilder;
    removeBuilder.append(TagsType::ns(), nss.ns());
    removeBuilder.append(TagsType::min(), actualRange.getMin());
    removeBuilder.append(TagsType::max(), actualRange.getMax());

    uassertStatusOK(Grid::get(opCtx)->catalogClient()->removeConfigDocuments(
        opCtx, TagsType::ConfigNS, removeBuilder.obj(), kNoWaitWriteConcern));
}

}  // namespace mongo
