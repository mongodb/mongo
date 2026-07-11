// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/sharding_environment/client/sharding_connection_hook.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/internal_auth.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

using std::string;

ShardingConnectionHook::ShardingConnectionHook(std::unique_ptr<rpc::EgressMetadataHook> egressHook)
    : _egressHook(std::move(egressHook)) {}

void ShardingConnectionHook::onCreate(DBClientBase* conn) {
    if (conn->type() == ConnectionString::ConnectionType::kInvalid) {
        uasserted(ErrorCodes::BadValue, str::stream() << "Unrecognized connection string.");
    }

    // Authenticate as the first thing we do
    // NOTE: Replica set authentication allows authentication against *any* online host
    if (auth::isInternalAuthSet()) {
        LOGV2_DEBUG(22722, 2, "Calling onCreate auth", "connectionString"_attr = conn->toString());
        try {
            conn->authenticateInternalUser();
        } catch (DBException& e) {
            e.addContext(str::stream()
                         << "can't authenticate to server " << conn->getServerAddress());
            throw;
        }
    }

    conn->setRequestMetadataWriter([this](OperationContext* opCtx, BSONObjBuilder* metadataBob) {
        return _egressHook->writeRequestMetadata(opCtx, metadataBob);
    });


    if (conn->type() == ConnectionString::ConnectionType::kStandalone) {
        BSONObj helloResponse;
        if (!conn->runCommand(DatabaseName::kAdmin, BSON("hello" << 1), helloResponse)) {
            uassertStatusOK(getStatusFromCommandResult(helloResponse));
        }

        long long configServerModeNumber;
        Status status =
            bsonExtractIntegerField(helloResponse, "configsvr", &configServerModeNumber);

        if (status == ErrorCodes::NoSuchKey) {
            // This isn't a config server we're talking to.
            return;
        }
        uassertStatusOK(status);

        const long long minKnownConfigServerMode = 1;
        const long long maxKnownConfigServerMode = 2;
        uassert(28785,
                str::stream() << "Unrecognized configsvr mode number: " << configServerModeNumber
                              << ". Range of known configsvr mode numbers is: ["
                              << minKnownConfigServerMode << ", " << maxKnownConfigServerMode
                              << "]",
                configServerModeNumber >= minKnownConfigServerMode &&
                    configServerModeNumber <= maxKnownConfigServerMode);

        uassertStatusOK(status);
    }
}

void ShardingConnectionHook::onRelease(DBClientBase* conn) {
    // This is currently for making the replica set connections release
    // secondary connections to the pool.
    conn->reset();
}

}  // namespace mongo
