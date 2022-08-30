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
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/split_chunk.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/str.h"

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
               "splitKeys : [ {a:150} , ... ], fromChunkSplitter: <bool>}";
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

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceString(dbName.tenantId(), CommandHelpers::parseNsFullyQualified(cmdObj));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

        const NamespaceString nss(parseNs({boost::none, dbname}, cmdObj));

        // Check whether parameters passed to splitChunk are sound
        BSONObj keyPatternObj;
        {
            BSONElement keyPatternElem;
            auto keyPatternStatus =
                bsonExtractTypedField(cmdObj, "keyPattern", Object, &keyPatternElem);

            if (!keyPatternStatus.isOK()) {
                errmsg = "need to specify the key pattern the collection is sharded over";
                return false;
            }
            keyPatternObj = keyPatternElem.Obj();
        }

        auto chunkRange = uassertStatusOK(ChunkRange::fromBSON(cmdObj));

        std::string shardName;
        auto parseShardNameStatus = bsonExtractStringField(cmdObj, "from", &shardName);
        uassertStatusOK(parseShardNameStatus);

        LOGV2(22104,
              "Received splitChunk request: {request}",
              "Received splitChunk request",
              "request"_attr = redact(cmdObj));

        std::vector<BSONObj> splitKeys;
        {
            BSONElement splitKeysElem;
            auto splitKeysElemStatus =
                bsonExtractTypedField(cmdObj, "splitKeys", mongo::Array, &splitKeysElem);

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

        bool fromChunkSplitter = [&]() {
            bool field = false;
            Status status = bsonExtractBooleanField(cmdObj, "fromChunkSplitter", &field);
            return status.isOK() && field;
        }();

        // Check that the preconditions for split chunk are met and throw StaleShardVersion
        // otherwise.
        {
            onShardVersionMismatch(opCtx, nss, boost::none);
            OperationShardingState::
                unsetShardRoleForLegacyDDLOperationsSentWithShardVersionIfNeeded(opCtx, nss);
            const auto metadata = checkCollectionIdentity(
                opCtx, nss, expectedCollectionEpoch, expectedCollectionTimestamp);
            checkShardKeyPattern(opCtx, nss, metadata, chunkRange);
            checkChunkMatchesRange(opCtx, nss, metadata, chunkRange);
        }

        auto topChunk = uassertStatusOK(splitChunk(opCtx,
                                                   nss,
                                                   keyPatternObj,
                                                   chunkRange,
                                                   std::move(splitKeys),
                                                   shardName,
                                                   expectedCollectionEpoch,
                                                   expectedCollectionTimestamp,
                                                   fromChunkSplitter));

        // Otherwise, we want to check whether or not top-chunk optimization should be performed. If
        // yes, then we should have a ChunkRange that was returned. Regardless of whether it should
        // be performed, we will return true.
        if (topChunk) {
            result.append("shouldMigrate",
                          BSON("min" << topChunk->getMin() << "max" << topChunk->getMax()));
        }

        return true;
    }

} cmdSplitChunk;

}  // namespace
}  // namespace mongo
