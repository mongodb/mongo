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

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/s/shard_key_index_util.h"

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
        return NamespaceStringUtil::parseNamespaceFromRequest(
            dbName.tenantId(), CommandHelpers::parseNsFullyQualified(cmdObj));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& jsobj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        const NamespaceString nss(
            parseNs(DatabaseNameUtil::deserialize(boost::none, dbname), jsobj));

        BSONObj keyPattern = jsobj.getObjectField("keyPattern");
        if (keyPattern.isEmpty()) {
            errmsg = "no key pattern found in checkShardingindex";
            return false;
        }

        if (keyPattern.nFields() == 1 && keyPattern.firstElementFieldNameStringData() == "_id") {
            result.appendBool("idskip", true);
            return true;
        }

        AutoGetCollectionForReadCommand collection(opCtx, nss);
        if (!collection) {
            errmsg = "ns not found";
            return false;
        }

        std::string tmpErrMsg = "couldn't find valid index for shard key";
        const auto shardKeyIdx = findShardKeyPrefixedIndex(opCtx,
                                                           *collection,
                                                           keyPattern,
                                                           /*requireSingleKey=*/true,
                                                           &tmpErrMsg);
        if (!shardKeyIdx) {
            errmsg = tmpErrMsg;
            return false;
        }

        return true;
    }

} cmdCheckShardingIndex;

}  // namespace
}  // namespace mongo
