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


#include <string>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/sharding_index_catalog_cache.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class GetShardVersion : public BasicCommand {
public:
    GetShardVersion() : BasicCommand("getShardVersion", "getshardversion") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forExactNamespace(parseNs(dbName, cmdObj)),
                     ActionType::getShardVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        BSONElement first = cmdObj.firstElement();
        uassert(ErrorCodes::BadValue,
                str::stream() << "namespace has invalid type " << typeName(first.type()),
                first.canonicalType() == canonicalizeBSONType(mongo::String));
        return NamespaceStringUtil::deserialize(
            dbName.tenantId(), first.valueStringData(), SerializationContext::stateDefault());
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        const auto catalogCache = Grid::get(opCtx)->catalogCache();

        if (nss.coll().empty()) {
            // Return the database's information.
            auto cachedDbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.dbName()));
            result.append("primaryShard", cachedDbInfo->getPrimary().toString());
            result.append("version", cachedDbInfo->getVersion().toBSON());
        } else {
            // Return the collection's information.
            const auto [cm, sii] =
                uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
            uassert(ErrorCodes::NamespaceNotSharded,
                    str::stream() << "Collection " << nss.toStringForErrorMsg()
                                  << " is not sharded.",
                    cm.isSharded());

            result.appendTimestamp("version", cm.getVersion().toLong());
            result.append("versionEpoch", cm.getVersion().epoch());
            result.append("versionTimestamp", cm.getVersion().getTimestamp());
            // Added to the result bson if the max bson size is exceeded
            BSONObjBuilder exceededSizeElt(BSON("exceededSize" << true));

            if (sii) {
                result.append("indexVersion", sii->getCollectionIndexes().indexVersion());
            }

            if (cmdObj["fullMetadata"].trueValue()) {
                BSONArrayBuilder chunksArrBuilder;
                bool exceedsSizeLimit = false;

                LOGV2(22753,
                      "Routing info requested by getShardVersion: {routingInfo}",
                      "Routing info requested by getShardVersion",
                      "routingInfo"_attr = redact(cm.toString()));

                cm.forEachChunk([&](const auto& chunk) {
                    if (!exceedsSizeLimit) {
                        BSONArrayBuilder chunkBB(chunksArrBuilder.subarrayStart());
                        chunkBB.append(chunk.getMin());
                        chunkBB.append(chunk.getMax());
                        chunkBB.done();
                        if (chunksArrBuilder.len() + result.len() + exceededSizeElt.len() >
                            BSONObjMaxUserSize) {
                            exceedsSizeLimit = true;
                        }
                    }

                    return true;
                });

                if (!exceedsSizeLimit) {
                    result.append("chunks", chunksArrBuilder.arr());

                    if (sii) {
                        BSONArrayBuilder indexesArrBuilder;
                        sii->forEachIndex([&](const auto& index) {
                            BSONObjBuilder indexB(index.toBSON());
                            if (result.len() + exceededSizeElt.len() + indexesArrBuilder.len() +
                                    indexB.len() >
                                BSONObjMaxUserSize) {
                                exceedsSizeLimit = true;
                            } else {
                                indexesArrBuilder.append(indexB.done());
                            }

                            return !exceedsSizeLimit;
                        });

                        result.append("indexes", indexesArrBuilder.arr());
                    }
                }

                if (exceedsSizeLimit) {
                    result.appendElements(exceededSizeElt.done());
                }
            }
        }

        return true;
    }
};
MONGO_REGISTER_COMMAND(GetShardVersion).forRouter();

}  // namespace
}  // namespace mongo
