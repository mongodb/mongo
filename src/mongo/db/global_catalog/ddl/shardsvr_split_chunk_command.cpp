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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/split_chunk.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"

#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class SplitChunkCommand : public ErrmsgCommandDeprecated {
public:
    SplitChunkCommand() : ErrmsgCommandDeprecated("splitChunk", "_shardsvrSplitChunk") {}

    std::string help() const override {
        return "internal command usage only\n"
               "example:\n"
               " { splitChunk:\"db.foo\" , keyPattern: {a:1} , min : {a:100} , max: {a:200} { "
               "splitKeys : [ {a:150} , ... ]}";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forClusterResource(dbName.tenantId()),
                     ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceStringUtil::deserialize(dbName.tenantId(),
                                                CommandHelpers::parseNsFullyQualified(cmdObj),
                                                SerializationContext::stateDefault());
    }

    bool errmsgRun(OperationContext* opCtx,
                   const DatabaseName& dbName,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

        const NamespaceString nss(parseNs(dbName, cmdObj));

        // Check whether parameters passed to splitChunk are sound
        BSONObj keyPatternObj;
        {
            BSONElement keyPatternElem;
            auto keyPatternStatus =
                bsonExtractTypedField(cmdObj, "keyPattern", BSONType::object, &keyPatternElem);

            if (!keyPatternStatus.isOK()) {
                errmsg = "need to specify the key pattern the collection is sharded over";
                return false;
            }
            keyPatternObj = keyPatternElem.Obj();
        }

        auto chunkRange = ChunkRange::fromBSON(cmdObj);

        std::string shardName;
        auto parseShardNameStatus = bsonExtractStringField(cmdObj, "from", &shardName);
        uassertStatusOK(parseShardNameStatus);

        LOGV2(22104, "Received splitChunk request", "request"_attr = redact(cmdObj));

        std::vector<BSONObj> splitKeys;
        {
            BSONElement splitKeysElem;
            auto splitKeysElemStatus =
                bsonExtractTypedField(cmdObj, "splitKeys", BSONType::array, &splitKeysElem);

            if (!splitKeysElemStatus.isOK()) {
                errmsg = "need to provide the split points to chunk over";
                return false;
            }

            BSONObjIterator it(splitKeysElem.Obj());
            while (it.more()) {
                splitKeys.push_back(it.next().Obj().getOwned());
            }
        }

        OID expectedCollectionEpoch;
        uassertStatusOK(bsonExtractOIDField(cmdObj, "epoch", &expectedCollectionEpoch));

        boost::optional<Timestamp> expectedCollectionTimestamp;
        if (cmdObj["timestamp"]) {
            expectedCollectionTimestamp.emplace();
            uassertStatusOK(bsonExtractTimestampField(
                cmdObj, "timestamp", expectedCollectionTimestamp.get_ptr()));
        }

        // Check that the preconditions for split chunk are met and throw StaleShardVersion
        // otherwise.
        {
            uassertStatusOK(
                FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                    opCtx, nss, boost::none));
            const auto metadata = checkCollectionIdentity(
                opCtx, nss, expectedCollectionEpoch, expectedCollectionTimestamp);
            checkShardKeyPattern(opCtx, nss, metadata, chunkRange);
            checkChunkMatchesRange(opCtx, nss, metadata, chunkRange);
        }

        uassertStatusOK(splitChunk(opCtx,
                                   nss,
                                   keyPatternObj,
                                   chunkRange,
                                   std::move(splitKeys),
                                   shardName,
                                   expectedCollectionEpoch,
                                   expectedCollectionTimestamp));

        return true;
    }
};
MONGO_REGISTER_COMMAND(SplitChunkCommand).forShard();

}  // namespace
}  // namespace mongo
