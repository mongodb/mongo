// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/repl_set_test_egress_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace repl {
namespace {
using namespace std::literals::string_view_literals;

HostAndPort selectTarget(OperationContext* opCtx) {
    auto members = ReplicationCoordinator::get(opCtx)->getMemberData();

    for (const auto& member : members) {
        if (member.up() && !member.isSelf()) {
            return member.getHostAndPort();
        }
    }

    uasserted(ErrorCodes::InternalError, "No viable replica set members to conenct to");
}

HostAndPort validateTarget(OperationContext* opCtx, std::string_view targetStr) {
    auto members = ReplicationCoordinator::get(opCtx)->getMemberData();

    auto target = HostAndPort::parseThrowing(targetStr);
    for (const auto& member : members) {
        if (target == member.getHostAndPort()) {
            if (member.isSelf()) {
                LOGV2_WARNING(4697200,
                              "Using replSetTestEgress to connect to self",
                              "target"_attr = targetStr);
            } else if (!member.up()) {
                LOGV2_WARNING(4697201,
                              "replSetTestEgress connecting to node which appears down",
                              "target"_attr = targetStr);
            }

            return target;
        }
    }

    uasserted(ErrorCodes::BadValue, str::stream() << targetStr << " is not a replica set member");
}

constexpr auto kReplSetTestEgress = "replSetTestEgress"sv;
auto getNetworkInterface() {
    static auto uniqueNI = ([] {
        auto ret = executor::makeNetworkInterface(std::string{kReplSetTestEgress});
        ret->startup();
        return ret;
    })();
    return uniqueNI.get();
}

const Status kDisconnectStatus(ErrorCodes::ClientDisconnect, "Invalidating temporary connection");
class CmdReplSetTestEgress final : public TypedCommand<CmdReplSetTestEgress> {
public:
    using Request = ReplSetTestEgress;
    using Reply = ReplSetTestEgressReply;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx) {
            const auto& provider =
                rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
            uassert(
                ErrorCodes::CommandNotSupported,
                str::stream() << "replSetTestEgress command is not supported in this storage mode: "
                              << provider.name(),
                provider.supportsLegacyReplSetCommands());

            const auto& cmd = request();

            HostAndPort target;
            if (auto optTarget = cmd.getTarget()) {
                target = validateTarget(opCtx, optTarget.value());
            } else {
                target = selectTarget(opCtx);
            }
            Seconds timeout(cmd.getTimeoutSecs());

            // ConnectionPool wants very much to have at least one idle connection
            // at the ready so that we never have to wait for an outgoing request.
            // We must thwart that.
            auto net = getNetworkInterface();
            LOGV2_DEBUG(
                4697203, 4, "Dropping any existing connections", "target"_attr = target.toString());
            net->dropConnections(
                target, Status(ErrorCodes::PooledConnectionsDropped, kDisconnectStatus.reason()));
            LOGV2_DEBUG(4697204,
                        4,
                        "Opening test egress connection",
                        "target"_attr = target.toString(),
                        "timeout"_attr = timeout);
            net->testEgress(
                target, transport::ConnectSSLMode::kGlobalSSLMode, timeout, kDisconnectStatus);

            Reply reply;
            reply.setTarget(target.toString());
            return reply;
        }

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const final {}

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
    };

    bool adminOnly() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuthzChecks() const override {
        return false;
    }
};

MONGO_REGISTER_COMMAND(CmdReplSetTestEgress).testOnly().forShard();

}  // namespace
}  // namespace repl
}  // namespace mongo
