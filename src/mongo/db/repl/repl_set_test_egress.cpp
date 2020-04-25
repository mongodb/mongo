/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_test_egress_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {
namespace {

HostAndPort selectTarget(OperationContext* opCtx) {
    auto members = ReplicationCoordinator::get(opCtx)->getMemberData();

    for (const auto& member : members) {
        if (member.up() && !member.isSelf()) {
            return member.getHostAndPort();
        }
    }

    uasserted(ErrorCodes::InternalError, "No viable replica set members to conenct to");
}

HostAndPort validateTarget(OperationContext* opCtx, StringData targetStr) {
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

constexpr auto kReplSetTestEgress = "replSetTestEgress"_sd;
auto getNetworkInterface() {
    static auto uniqueNI = ([] {
        auto ret = executor::makeNetworkInterface(kReplSetTestEgress.toString());
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
            const auto& cmd = request();

            HostAndPort target;
            if (auto optTarget = cmd.getTarget()) {
                target = validateTarget(opCtx, optTarget.get());
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
            net->dropConnections(target);
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
            return NamespaceString(request().getDbName(), "");
        }
    };

    bool adminOnly() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
};

MONGO_REGISTER_TEST_COMMAND(CmdReplSetTestEgress);

}  // namespace
}  // namespace repl
}  // namespace mongo
