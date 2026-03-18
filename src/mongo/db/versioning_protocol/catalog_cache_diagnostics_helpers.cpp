/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/versioning_protocol/catalog_cache_diagnostics_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace catalog_cache_diagnostics_helpers {
namespace {

void appendChunkManagerInfo(OperationContext* opCtx,
                            BSONObjBuilder* builder,
                            const NamespaceString& nss,
                            const ChunkManager& cm,
                            bool fullMetadata) {
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << nss.toStringForErrorMsg()
                          << " does not have a routing table.",
            cm.hasRoutingTable());

    builder->appendTimestamp("version", cm.getVersion().toLong());
    builder->append("versionEpoch", cm.getVersion().epoch());
    builder->append("versionTimestamp", cm.getVersion().getTimestamp());

    // Added to the builder bson if the max bson size is exceeded
    BSONObjBuilder exceededSizeElt(BSON("exceededSize" << true));

    if (fullMetadata) {
        BSONArrayBuilder chunksArrBuilder;
        bool exceedsSizeLimit = false;

        LOGV2(22753,
              "Routing info requested by getShardVersion",
              "routingInfo"_attr = redact(cm.toString()));

        cm.forEachChunk([&](const auto& chunk) {
            if (!exceedsSizeLimit) {
                BSONArrayBuilder chunkBB(chunksArrBuilder.subarrayStart());
                chunkBB.append(chunk.getMin());
                chunkBB.append(chunk.getMax());
                chunkBB.done();
                if (chunksArrBuilder.len() + builder->len() + exceededSizeElt.len() >
                    BSONObjMaxUserSize) {
                    exceedsSizeLimit = true;
                }
            }

            return true;
        });

        if (!exceedsSizeLimit) {
            builder->append("chunks", chunksArrBuilder.arr());
        }

        if (exceedsSizeLimit) {
            builder->appendElements(exceededSizeElt.done());
        }
    }
}

}  // namespace

void appendWhenUnknown(BSONObjBuilder* builder, bool fullMetadata) {
    builder->append("global", "UNKNOWN");
    if (fullMetadata) {
        builder->append("metadata", BSONObj());
    }
}

void appendCatalogCacheInfo(OperationContext* opCtx,
                            BSONObjBuilder* builder,
                            const NamespaceString& nss,
                            bool fullMetadata) {
    const auto catalogCache = Grid::get(opCtx)->catalogCache();
    // The ability to append this dbInfo is left to preserve backward compatibility, but in the
    // future we should prefer using getShardVersion only for collections and getDatabaseVersion for
    // databases.
    if (nss.isDbOnly()) {
        auto cachedDbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.dbName()));
        builder->append("primaryShard", cachedDbInfo->getPrimary().toString());
        builder->append("version", cachedDbInfo->getVersion().toBSON());
    } else {
        const auto cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
        appendChunkManagerInfo(opCtx, builder, nss, cri.getChunkManager(), fullMetadata);
    }
}

void appendLatestCachedCollInfo(OperationContext* opCtx,
                                BSONObjBuilder* builder,
                                const NamespaceString& nss,
                                bool fullMetadata) {
    const auto catalogCache = Grid::get(opCtx)->catalogCache();
    uassert(ErrorCodes::InvalidOptions,
            "Cannot call getShardVersion with only a dbName. Please use getDatabaseVersion for "
            "database information",
            !nss.isDbOnly());
    const auto cachedCollInfo = catalogCache->peekCollectionCacheEntry(nss);
    if (!cachedCollInfo) {
        appendWhenUnknown(builder, fullMetadata);
        return;
    }
    builder->append("timeInStore", cachedCollInfo.getTime().toString());
    try {
        appendChunkManagerInfo(
            opCtx, builder, nss, CurrentChunkManager(cachedCollInfo), fullMetadata);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // For unsharded collections, we still append the timeInStore and unsharded version.
        const auto& unshardedVersion = ChunkVersion::UNTRACKED();
        builder->appendTimestamp("version", unshardedVersion.toLong());
        builder->append("versionEpoch", unshardedVersion.epoch());
        builder->append("versionTimestamp", unshardedVersion.getTimestamp());
    }
}

void appendLatestCachedDbInfo(OperationContext* opCtx,
                              BSONObjBuilder* builder,
                              const DatabaseName& dbName) {
    const auto catalogCache = Grid::get(opCtx)->catalogCache();
    const auto cachedDbInfo = catalogCache->peekDatabaseCacheEntry(dbName);
    if (!cachedDbInfo) {
        appendWhenUnknown(builder, false /* fullMetadata */);
        return;
    }
    builder->append("dbVersion", cachedDbInfo->getVersion().toBSON());
    builder->append("primaryShard", cachedDbInfo->getPrimary());
    builder->append("timeInStore", cachedDbInfo.getTime().toString());
}

}  // namespace catalog_cache_diagnostics_helpers

}  // namespace mongo
