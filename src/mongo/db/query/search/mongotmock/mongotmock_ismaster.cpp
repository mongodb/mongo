/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/commands.h"
#include "mongo/db/wire_version.h"

namespace mongo {
namespace {

constexpr auto kHelloString = "hello"_sd;
// Aliases for the hello command in order to provide backwards compatibility.
constexpr auto kCamelCaseIsMasterString = "isMaster"_sd;
constexpr auto kLowerCaseIsMasterString = "ismaster"_sd;

/**
 * Implements { hello : 1} for mock_mongot.
 */
class MongotMockHello final : public BasicCommand {
public:
    MongotMockHello()
        : BasicCommand(kHelloString, {kCamelCaseIsMasterString, kLowerCaseIsMasterString}) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        MONGO_UNREACHABLE;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        MONGO_UNREACHABLE;
    }

    std::string help() const final {
        return "Check if this server is primary for a replica set\n"
               "{ hello : 1 }";
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const final {
        MONGO_UNREACHABLE;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& jsobj,
             BSONObjBuilder& result) final {
        // Parse the command name, which should be one of the following: hello, isMaster, or
        // ismaster. If the command is "hello", we must attach an "isWritablePrimary" response field
        // instead of "ismaster".
        bool useLegacyResponseFields = (jsobj.firstElementFieldNameStringData() != kHelloString);

        if (useLegacyResponseFields) {
            result.appendBool("ismaster", true);
        } else {
            result.appendBool("isWritablePrimary", true);
        }

        result.appendBool("ismongot", true);
        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", static_cast<long long>(MaxMessageSizeBytes));
        result.appendDate("localTime", Date_t::now());

        auto incomingExternalClient =
            WireSpec::getWireSpec(opCtx->getServiceContext()).getIncomingExternalClient();
        result.append("maxWireVersion", incomingExternalClient.maxWireVersion);
        result.append("minWireVersion", incomingExternalClient.minWireVersion);

        // The mongod paired with a mongotmock should be able to auth as the __system user with
        // the SCRAM-SHA-256 authentication mechanism.
        result.append("saslSupportedMechs", BSON_ARRAY("SCRAM-SHA-256"));
        return true;
    }
};
MONGO_REGISTER_COMMAND(MongotMockHello).forShard();

}  // namespace
}  // namespace mongo
