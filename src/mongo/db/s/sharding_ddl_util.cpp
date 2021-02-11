/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/s/sharding_ddl_util.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

namespace sharding_ddl_util {

namespace {

void cloneTags(OperationContext* opCtx,
               const NamespaceString& fromNss,
               const NamespaceString& toNss) {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, fromNss));

    if (tags.empty()) {
        return;
    }

    // Wait for majority just for last tag
    auto lastTag = tags.back();
    tags.pop_back();
    for (auto& tag : tags) {
        tag.setNS(toNss);
        uassertStatusOK(catalogClient->insertConfigDocument(
            opCtx, TagsType::ConfigNS, tag.toBSON(), ShardingCatalogClient::kLocalWriteConcern));
    }
    lastTag.setNS(toNss);
    uassertStatusOK(catalogClient->insertConfigDocument(
        opCtx, TagsType::ConfigNS, lastTag.toBSON(), ShardingCatalogClient::kMajorityWriteConcern));
}

void deleteChunks(OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove config.chunks entries
    const auto chunksQuery = [&]() {
        auto optUUID = nssOrUUID.uuid();
        if (optUUID) {
            return BSON(ChunkType::collectionUUID << *optUUID);
        }

        auto optNss = nssOrUUID.nss();
        invariant(optNss);
        return BSON(ChunkType::ns(optNss->ns()));
    }();

    uassertStatusOK(catalogClient->removeConfigDocuments(
        opCtx, ChunkType::ConfigNS, chunksQuery, ShardingCatalogClient::kMajorityWriteConcern));
}

void deleteCollection(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove config.collection entry
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             CollectionType::ConfigNS,
                                             BSON(CollectionType::kNssFieldName << nss.ns()),
                                             ShardingCatalogClient::kMajorityWriteConcern));
}

}  // namespace

void removeTagsMetadataFromConfig(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove config.tags entries
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             TagsType::ConfigNS,
                                             BSON(TagsType::ns(nss.ns())),
                                             ShardingCatalogClient::kMajorityWriteConcern));
}

void removeCollMetadataFromConfig(OperationContext* opCtx, const CollectionType& coll) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto& nss = coll.getNss();

    ON_BLOCK_EXIT(
        [&] { Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss); });

    const NamespaceStringOrUUID nssOrUUID = coll.getTimestamp()
        ? NamespaceStringOrUUID(nss.db().toString(), coll.getUuid())
        : NamespaceStringOrUUID(nss);

    deleteChunks(opCtx, nssOrUUID);

    removeTagsMetadataFromConfig(opCtx, nss);

    deleteCollection(opCtx, nss);
}

bool removeCollMetadataFromConfig(OperationContext* opCtx, const NamespaceString& nss) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    ON_BLOCK_EXIT(
        [&] { Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss); });

    try {
        auto coll = catalogClient->getCollection(opCtx, nss);
        removeCollMetadataFromConfig(opCtx, coll);
        return true;
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is not sharded or doesn't exist, just tags need to be removed
        removeTagsMetadataFromConfig(opCtx, nss);
        return false;
    }
}

void shardedRenameMetadata(OperationContext* opCtx,
                           const NamespaceString& fromNss,
                           const NamespaceString& toNss) {
    auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Delete eventual TO chunk/collection entries referring a dropped collection
    removeCollMetadataFromConfig(opCtx, toNss);

    // Clone FROM tags to TO
    cloneTags(opCtx, fromNss, toNss);

    auto collType = catalogClient->getCollection(opCtx, fromNss);
    collType.setNss(toNss);
    // Insert the TO collection entry
    uassertStatusOK(
        catalogClient->insertConfigDocument(opCtx,
                                            CollectionType::ConfigNS,
                                            collType.toBSON(),
                                            ShardingCatalogClient::kMajorityWriteConcern));

    // Delete FROM tag/collection entries
    removeTagsMetadataFromConfig(opCtx, fromNss);
    deleteCollection(opCtx, fromNss);
}

void checkShardedRenamePreconditions(OperationContext* opCtx,
                                     const NamespaceString& toNss,
                                     const bool dropTarget) {
    if (!dropTarget) {
        // Check that the sharded target collection doesn't exist
        auto catalogCache = Grid::get(opCtx)->catalogCache();
        try {
            catalogCache->getShardedCollectionRoutingInfo(opCtx, toNss);
            // If no exception is thrown, the collection exists and is sharded
            uasserted(ErrorCodes::CommandFailed,
                      str::stream() << "Sharded target collection " << toNss.ns()
                                    << " exists but dropTarget is not set");
        } catch (const DBException& ex) {
            auto code = ex.code();
            if (code != ErrorCodes::NamespaceNotFound && code != ErrorCodes::NamespaceNotSharded) {
                throw;
            }
        }

        // Check that the unsharded target collection doesn't exist
        auto collectionCatalog = CollectionCatalog::get(opCtx);
        auto targetColl = collectionCatalog->lookupCollectionByNamespace(opCtx, toNss);
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "Target collection " << toNss.ns()
                              << " exists but dropTarget is not set",
                !targetColl);
    }

    // Check that there are no tags associated to the target collection
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, toNss));
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "Can't rename to target collection " << toNss.ns()
                          << " because it must not have associated tags",
            tags.empty());
}

boost::optional<CreateCollectionResponse> checkIfCollectionAlreadySharded(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& key,
    const BSONObj& collation,
    bool unique) {
    auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss));

    if (!cm.isSharded()) {
        return boost::none;
    }

    auto defaultCollator =
        cm.getDefaultCollator() ? cm.getDefaultCollator()->getSpec().toBSON() : BSONObj();

    // If the collection is already sharded, fail if the deduced options in this request do not
    // match the options the collection was originally sharded with.
    uassert(ErrorCodes::AlreadyInitialized,
            str::stream() << "sharding already enabled for collection " << nss,
            SimpleBSONObjComparator::kInstance.evaluate(cm.getShardKeyPattern().toBSON() == key) &&
                SimpleBSONObjComparator::kInstance.evaluate(defaultCollator == collation) &&
                cm.isUnique() == unique);

    CreateCollectionResponse response(cm.getVersion());
    response.setCollectionUUID(cm.getUUID());
    return response;
}

void acquireCriticalSection(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollection cCollLock(opCtx, nss, MODE_X);
    auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
    csr->enterCriticalSectionCatchUpPhase(csrLock);
    csr->enterCriticalSectionCommitPhase(csrLock);
}

void releaseCriticalSection(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollection collLock(opCtx, nss, MODE_X);
    auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
    csr->exitCriticalSection(opCtx);
    csr->clearFilteringMetadata(opCtx);
}

}  // namespace sharding_ddl_util
}  // namespace mongo
