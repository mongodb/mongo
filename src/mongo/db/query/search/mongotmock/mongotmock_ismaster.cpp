// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/wire_version.h"

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kHelloString = "hello"sv;
// Aliases for the hello command in order to provide backwards compatibility.
constexpr auto kCamelCaseIsMasterString = "isMaster"sv;
constexpr auto kLowerCaseIsMasterString = "ismaster"sv;

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
