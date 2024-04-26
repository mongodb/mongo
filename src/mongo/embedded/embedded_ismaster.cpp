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

#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache_gen.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class CmdIsMaster : public BasicCommand {
public:
    bool requiresAuth() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Check if this server is primary for a replica set\n"
               "{ isMaster : 1 }";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();  // No auth required
    }

    CmdIsMaster() : BasicCommand("isMaster", "ismaster") {}

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {

        auto wireSpec = WireSpec::getWireSpec(getGlobalServiceContext()).get();

        ClientMetadata::tryFinalize(opCtx->getClient());
        audit::logClientMetadata(opCtx->getClient());

        result.appendBool("ismaster", true);

        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", static_cast<long long>(MaxMessageSizeBytes));
        result.appendNumber("maxWriteBatchSize",
                            static_cast<long long>(write_ops::kMaxWriteBatchSize));
        result.appendDate("localTime", jsTime());
        result.append("logicalSessionTimeoutMinutes", localLogicalSessionTimeoutMinutes);
        result.appendNumber("connectionId", opCtx->getClient()->getConnectionId());

        result.append("minWireVersion", wireSpec->incomingExternalClient.minWireVersion);
        result.append("maxWireVersion", wireSpec->incomingExternalClient.maxWireVersion);

        result.append("readOnly", opCtx->readOnly());

        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdIsMaster).forShard();

}  // namespace
}  // namespace mongo
