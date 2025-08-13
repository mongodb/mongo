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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/namespace_string_util.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class CheckShardingIndex : public ErrmsgCommandDeprecated {
public:
    CheckShardingIndex() : ErrmsgCommandDeprecated("checkShardingIndex") {}

    std::string help() const override {
        return "Internal command.\n";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::find)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
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
                   const BSONObj& jsobj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, jsobj));

        BSONObj keyPattern = jsobj.getObjectField("keyPattern");
        if (keyPattern.isEmpty()) {
            errmsg = "no key pattern found in checkShardingindex";
            return false;
        }

        if (keyPattern.nFields() == 1 && keyPattern.firstElementFieldNameStringData() == "_id") {
            result.appendBool("idskip", true);
            return true;
        }

        AutoStatsTracker statsTracker(opCtx,
                                      nss,
                                      Top::LockType::ReadLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      DatabaseProfileSettings::get(opCtx->getServiceContext())
                                          .getDatabaseProfileLevel(nss.dbName()));

        const auto collection = acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead));
        if (!collection.exists()) {
            errmsg = "ns not found";
            return false;
        }

        std::string tmpErrMsg = "couldn't find valid index for shard key";
        const auto shardKeyIdx = findShardKeyPrefixedIndex(opCtx,
                                                           collection.getCollectionPtr(),
                                                           keyPattern,
                                                           /*requireSingleKey=*/true,
                                                           &tmpErrMsg);

        uassert(ErrorCodes::InvalidOptions, str::stream() << tmpErrMsg, shardKeyIdx);

        return true;
    }
};
MONGO_REGISTER_COMMAND(CheckShardingIndex).forShard();

}  // namespace
}  // namespace mongo
