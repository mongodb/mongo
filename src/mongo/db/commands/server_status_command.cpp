/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {
constexpr auto kTimingSection = "timing"_sd;

class CmdServerStatus : public BasicCommand {
public:
    CmdServerStatus() : BasicCommand("serverStatus"), _started(Date_t::now()) {}

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool allowsAfterClusterTime(const BSONObj& cmdObj) const final {
        return false;
    }

    std::string help() const final {
        return "returns lots of administrative server statistics. Optionally set {..., all: 1} to "
               "retrieve all server status sections.";
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const final {
        ActionSet actions;
        actions.addAction(ActionType::serverStatus);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        const auto service = opCtx->getServiceContext();
        const auto clock = service->getFastClockSource();
        const auto runStart = clock->now();
        BSONObjBuilder timeBuilder(256);

        const auto authSession = AuthorizationSession::get(Client::getCurrent());

        // This command is important to observability, and like FTDC, does not need to acquire the
        // PBWM lock to return correct results.
        ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
        opCtx->lockState()->skipAcquireTicket();

        // --- basic fields that are global

        result.append("host", prettyHostName());
        result.append("version", VersionInfoInterface::instance().version());
        result.append("process", serverGlobalParams.binaryName);
        result.append("pid", ProcessId::getCurrent().asLongLong());
        result.append("uptime", (double)(time(nullptr) - serverGlobalParams.started));
        auto uptime = clock->now() - _started;
        result.append("uptimeMillis", durationCount<Milliseconds>(uptime));
        result.append("uptimeEstimate", durationCount<Seconds>(uptime));
        result.appendDate("localTime", jsTime());

        timeBuilder.appendNumber("after basic",
                                 durationCount<Milliseconds>(clock->now() - runStart));

        // Individual section 'includeByDefault()' settings will be bypassed if the caller specified
        // {all: 1}.
        const auto& allElem = cmdObj["all"];
        bool includeAllSections = allElem.type() ? allElem.trueValue() : false;

        // --- all sections
        auto registry = ServerStatusSectionRegistry::get();
        for (auto i = registry->begin(); i != registry->end(); ++i) {
            ServerStatusSection* section = i->second;

            std::vector<Privilege> requiredPrivileges;
            section->addRequiredPrivileges(&requiredPrivileges);
            if (!authSession->isAuthorizedForPrivileges(requiredPrivileges))
                continue;

            bool include = section->includeByDefault();
            const auto& elem = cmdObj[section->getSectionName()];
            if (elem.type()) {
                include = elem.trueValue();
            }

            if (!include && !includeAllSections) {
                continue;
            }

            section->appendSection(opCtx, elem, &result);
            timeBuilder.appendNumber(
                static_cast<std::string>(str::stream() << "after " << section->getSectionName()),
                durationCount<Milliseconds>(clock->now() - runStart));
        }

        // --- counters
        MetricTree* metricTree = globalMetricTree(/* create */ false);
        auto metricsEl = cmdObj["metrics"_sd];
        if (metricTree && (metricsEl.eoo() || metricsEl.trueValue())) {
            if (metricsEl.type() == BSONType::Object) {
                metricTree->appendTo(BSON("metrics" << metricsEl.embeddedObject()), result);
            } else {
                metricTree->appendTo(result);
            }
        }

        // --- some hard coded global things hard to pull out

        auto runElapsed = clock->now() - runStart;
        timeBuilder.appendNumber("at end", durationCount<Milliseconds>(runElapsed));
        if (runElapsed > Milliseconds(1000)) {
            BSONObj t = timeBuilder.obj();
            LOGV2(20499,
                  "serverStatus was very slow: {timeStats}",
                  "serverStatus was very slow",
                  "timeStats"_attr = t);

            bool include_timing = true;
            const auto& elem = cmdObj[kTimingSection];
            if (!elem.eoo()) {
                include_timing = elem.trueValue();
            }

            if (include_timing) {
                result.append(kTimingSection, t);
            }
        }

        return true;
    }

private:
    const Date_t _started;
};

Command* serverStatusCommand;

MONGO_INITIALIZER(CreateCmdServerStatus)(InitializerContext* context) {
    serverStatusCommand = new CmdServerStatus();
}

}  // namespace

OpCounterServerStatusSection::OpCounterServerStatusSection(const std::string& sectionName,
                                                           OpCounters* counters)
    : ServerStatusSection(sectionName), _counters(counters) {}

BSONObj OpCounterServerStatusSection::generateSection(OperationContext* opCtx,
                                                      const BSONElement& configElement) const {
    return _counters->getObj();
}

OpCounterServerStatusSection globalOpCounterServerStatusSection("opcounters", &globalOpCounters);

namespace {

// some universal sections

class ExtraInfo : public ServerStatusSection {
public:
    ExtraInfo() : ServerStatusSection("extra_info") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder bb;

        bb.append("note", "fields vary by platform");
        ProcessInfo p;
        p.getExtraInfo(bb);

        return bb.obj();
    }

} extraInfo;

class Asserts : public ServerStatusSection {
public:
    Asserts() : ServerStatusSection("asserts") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder asserts;
        asserts.append("regular", assertionCount.regular.loadRelaxed());
        asserts.append("warning", assertionCount.warning.loadRelaxed());
        asserts.append("msg", assertionCount.msg.loadRelaxed());
        asserts.append("user", assertionCount.user.loadRelaxed());
        asserts.append("tripwire", assertionCount.tripwire.loadRelaxed());
        asserts.append("rollovers", assertionCount.rollovers.loadRelaxed());
        return asserts.obj();
    }

} asserts;

class MemBase : public ServerStatusMetric {
public:
    MemBase() : ServerStatusMetric(".mem.bits") {}

    void appendAtLeaf(BSONObjBuilder& b) const override {
        b.append("bits", sizeof(int*) == 4 ? 32 : 64);

        ProcessInfo p;
        int v = 0;
        if (p.supported()) {
            b.appendNumber("resident", p.getResidentSize());
            v = p.getVirtualMemorySize();
            b.appendNumber("virtual", v);
            b.appendBool("supported", true);
        } else {
            b.append("note", "not all mem info support on this platform");
            b.appendBool("supported", false);
        }
    }
};

MemBase& memBase = addMetricToTree(std::make_unique<MemBase>());

class HttpClientServerStatus : public ServerStatusSection {
public:
    HttpClientServerStatus() : ServerStatusSection("http_client") {}

    bool includeByDefault() const final {
        return false;
    }

    void addRequiredPrivileges(std::vector<Privilege>* out) final {}

    BSONObj generateSection(OperationContext*, const BSONElement& configElement) const final {
        return HttpClient::getServerStatus();
    }
} httpClientServerStatus;

}  // namespace

}  // namespace mongo
