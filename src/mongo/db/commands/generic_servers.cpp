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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/generic_servers_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/log_process_details.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_util.h"
#include "mongo/logv2/ramlog.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/processinfo.h"

#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

struct AdminOnlyNoTenant {
    static constexpr bool kAdminOnly = true;
    static constexpr bool kAllowedWithSecurityToken = false;
};

template <typename RequestT, typename Traits = AdminOnlyNoTenant>
class GenericTC : public TypedCommand<GenericTC<RequestT, Traits>> {
public:
    using Request = RequestT;
    using Reply = typename RequestT::Reply;
    using TC = TypedCommand<GenericTC<RequestT, Traits>>;

    class Invocation final : public TC::InvocationBase {
    public:
        using TC::InvocationBase::InvocationBase;
        using TC::InvocationBase::request;

        void doCheckAuthorization(OperationContext* opCtx) const final;
        Reply typedRun(OperationContext* opCtx);

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName(), "");
        }
    };

    bool adminOnly() const final {
        return Traits::kAdminOnly;
    }

    bool allowedWithSecurityToken() const final {
        return Traits::kAllowedWithSecurityToken;
    }

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return TC::AllowedOnSecondary::kAlways;
    }
};

struct AnyDbAllowTenant {
    static constexpr bool kAdminOnly = false;
    static constexpr bool kAllowedWithSecurityToken = true;
};

// { features: 1 }
using FeaturesCmd = GenericTC<FeaturesCommand, AnyDbAllowTenant>;
template <>
void FeaturesCmd::Invocation::doCheckAuthorization(OperationContext* opCtx) const {
    if (request().getOidReset().value_or(false)) {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        uassert(ErrorCodes::Unauthorized,
                "Not authorized to reset machine identifier",
                as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                     ActionType::oidReset));
    }
}
template <>
FeaturesReply FeaturesCmd::Invocation::typedRun(OperationContext*) {
    FeaturesReply reply;
    if (auto engine = getGlobalScriptEngine()) {
        reply.setJs(FeaturesReplyJS(engine->utf8Ok()));
    }

    if (request().getOidReset().value_or(false)) {
        reply.setOidMachineOld(static_cast<long>(OID::getMachineId()));
        OID::regenMachineId();
    }

    reply.setOidMachine(static_cast<long>(OID::getMachineId()));
    return reply;
}
FeaturesCmd featuresCmd;

struct AnyDbNoTenant {
    static constexpr bool kAdminOnly = false;
    static constexpr bool kAllowedWithSecurityToken = false;
};

// { hostInfo: 1 }
using HostInfoCmd = GenericTC<HostInfoCommand, AnyDbNoTenant>;
template <>
void HostInfoCmd::Invocation::doCheckAuthorization(OperationContext* opCtx) const {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassert(ErrorCodes::Unauthorized,
            "Not authorized to read hostInfo",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                 ActionType::hostInfo));
}
template <>
HostInfoReply HostInfoCmd::Invocation::typedRun(OperationContext*) {
    ProcessInfo p;

    HostInfoSystemReply system;
    system.setCurrentTime(jsTime());
    system.setHostname(prettyHostName());
    system.setCpuAddrSize(static_cast<int>(p.getAddrSize()));
    system.setMemSizeMB(static_cast<long>(p.getSystemMemSizeMB()));
    system.setMemLimitMB(static_cast<long>(p.getMemSizeMB()));
    system.setNumCores(static_cast<int>(p.getNumAvailableCores()));
    system.setNumPhysicalCores(static_cast<int>(p.getNumPhysicalCores()));
    system.setNumCpuSockets(static_cast<int>(p.getNumCpuSockets()));
    system.setCpuArch(p.getArch());
    system.setNumaEnabled(p.hasNumaEnabled());
    system.setNumNumaNodes(static_cast<int>(p.getNumNumaNodes()));

    HostInfoOsReply os;
    os.setType(p.getOsType());
    os.setName(p.getOsName());
    os.setVersion(p.getOsVersion());

    HostInfoReply reply;
    reply.setSystem(std::move(system));
    reply.setOs(std::move(os));

    // OS-specific information.
    BSONObjBuilder extra;
    p.appendSystemDetails(extra);
    reply.setExtra(extra.obj());

    return reply;
}
HostInfoCmd hostInfoCmd;

// { getCmdLineOpts: 1}
using GetCmdLineOptsCmd = GenericTC<GetCmdLineOptsCommand>;
template <>
void GetCmdLineOptsCmd::Invocation::doCheckAuthorization(OperationContext* opCtx) const {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassert(ErrorCodes::Unauthorized,
            "Not authorized to read command line options",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                 ActionType::getCmdLineOpts));
}
template <>
GetCmdLineOptsReply GetCmdLineOptsCmd::Invocation::typedRun(OperationContext*) {
    GetCmdLineOptsReply reply;
    reply.setArgv(serverGlobalParams.argvArray);
    reply.setParsed(serverGlobalParams.parsedOpts);
    return reply;
}
GetCmdLineOptsCmd getCmdLineOptsCmd;

// { logRotate: 1 || string }
using LogRotateCmd = GenericTC<LogRotateCommand>;
template <>
void LogRotateCmd::Invocation::doCheckAuthorization(OperationContext* opCtx) const {
    auto* as = AuthorizationSession::get(opCtx->getClient());
    uassert(ErrorCodes::Unauthorized,
            "Not authorized to rotate logs",
            as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                 ActionType::logRotate));
}
template <>
OkReply LogRotateCmd::Invocation::typedRun(OperationContext* opCtx) {
    auto arg = request().getCommandParameter();
    boost::optional<StringData> logType = boost::none;
    if (stdx::holds_alternative<std::string>(arg)) {
        logType = stdx::get<std::string>(arg);
    }

    logv2::LogRotateErrorAppender minorErrors;
    auto status = logv2::rotateLogs(serverGlobalParams.logRenameOnRotate,
                                    logType,
                                    [&minorErrors](Status err) { minorErrors.append(err); });

    // Mask the detailed error message so file paths & host info are not
    // revealed to the client, but keep the real status code as a hint.
    constexpr auto rotateErrmsg = "Log rotation failed due to one or more errors"_sd;
    uassert(status.code(), rotateErrmsg, status.isOK());

    logProcessDetailsForLogRotate(opCtx->getServiceContext());

    status = minorErrors.getCombinedStatus();
    if (!status.isOK()) {
        LOGV2_ERROR(6221501, "Log rotation failed", "error"_attr = status);
        uasserted(status.code(), rotateErrmsg);
    }

    return OkReply();
}
LogRotateCmd logRotateCmd;

// { getLog: '*' or 'logName' }
// We use BasicCommand here instead of TypedCommand
// to avoid having to do an extra set of string copies
// or risk log lines falling out of scope.
MONGO_FAIL_POINT_DEFINE(hangInGetLog);
class GetLogCmd : public BasicCommand {
public:
    GetLogCmd() : BasicCommand("getLog") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    bool adminOnly() const final {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const final {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                  ActionType::getLog)) {
            return {ErrorCodes::Unauthorized, "Not authorized to get log"};
        }
        return Status::OK();
    }

    std::string help() const final {
        return "{ getLog : '*' }  OR { getLog : 'global' }";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        if (MONGO_unlikely(hangInGetLog.shouldFail())) {
            LOGV2(5113600, "Hanging in getLog");
            hangInGetLog.pauseWhileSet();
        }

        auto request = GetLogCommand::parse(IDLParserContext{"getLog"}, cmdObj);
        auto logName = request.getCommandParameter();
        if (logName == "*") {
            std::vector<std::string> names;
            logv2::RamLog::getNames(names);

            BSONArrayBuilder arr(result.subarrayStart("names"_sd));
            for (const auto& name : names) {
                arr.append(name);
            }
            arr.doneFast();

        } else {
            logv2::RamLog* ramlog = logv2::RamLog::getIfExists(logName.toString());
            uassert(ErrorCodes::OperationFailed,
                    str::stream() << "No log named '" << logName << "'",
                    ramlog != nullptr);
            logv2::RamLog::LineIterator rl(ramlog);

            result.appendNumber("totalLinesWritten",
                                static_cast<long long>(rl.getTotalLinesWritten()));

            BSONArrayBuilder arr(result.subarrayStart("log"));
            while (rl.more()) {
                arr.append(rl.next());
            }
            arr.doneFast();
        }

        return true;
    }
} getLogCmd;

// { clearLog: 'name' }
using ClearLogCmd = GenericTC<ClearLogCommand>;
template <>
void ClearLogCmd::Invocation::doCheckAuthorization(OperationContext* opCtx) const {
    // We don't perform authorization,
    // so refuse to authorize this when test commands are not enabled.
    invariant(getTestCommandsEnabled());
}
template <>
OkReply ClearLogCmd::Invocation::typedRun(OperationContext* opCtx) {
    auto logName = request().getCommandParameter();
    uassert(
        ErrorCodes::InvalidOptions, "Only the 'global' log can be cleared", logName == "global");

    auto log = logv2::RamLog::getIfExists(logName.toString());
    invariant(log);
    log->clear();

    return OkReply();
}
MONGO_REGISTER_TEST_COMMAND(ClearLogCmd);

}  // namespace
}  // namespace mongo
