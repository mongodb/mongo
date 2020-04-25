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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/server_status_internal.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/version.h"

namespace mongo {

using std::endl;
using std::map;
using std::string;
using std::stringstream;

namespace {
constexpr auto kTimingSection = "timing"_sd;
}  // namespace

class CmdServerStatus : public BasicCommand {
public:
    CmdServerStatus() : BasicCommand("serverStatus"), _started(Date_t::now()), _runCalled(false) {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool allowsAfterClusterTime(const BSONObj& cmdObj) const override {
        return false;
    }
    std::string help() const override {
        return "returns lots of administrative server statistics";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::serverStatus);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        _runCalled = true;

        const auto service = opCtx->getServiceContext();
        const auto clock = service->getFastClockSource();
        const auto runStart = clock->now();
        BSONObjBuilder timeBuilder(256);

        const auto authSession = AuthorizationSession::get(Client::getCurrent());

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

        // --- all sections

        for (SectionMap::const_iterator i = _sections.begin(); i != _sections.end(); ++i) {
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

            if (!include) {
                continue;
            }

            section->appendSection(opCtx, elem, &result);
            timeBuilder.appendNumber(
                static_cast<string>(str::stream() << "after " << section->getSectionName()),
                durationCount<Milliseconds>(clock->now() - runStart));
        }

        // --- counters
        bool includeMetricTree = MetricTree::theMetricTree != nullptr;
        if (cmdObj["metrics"].type() && !cmdObj["metrics"].trueValue())
            includeMetricTree = false;

        if (includeMetricTree) {
            MetricTree::theMetricTree->appendTo(result);
        }

        // --- some hard coded global things hard to pull out

        auto runElapsed = clock->now() - runStart;
        timeBuilder.appendNumber("at end", durationCount<Milliseconds>(runElapsed));
        if (runElapsed > Milliseconds(1000)) {
            BSONObj t = timeBuilder.obj();
            LOGV2(20499, "serverStatus was very slow: {t}", "t"_attr = t);

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

    void addSection(ServerStatusSection* section) {
        // Disallow adding a section named "timing" as it is reserved for the server status command.
        dassert(section->getSectionName() != kTimingSection);
        verify(!_runCalled);
        _sections[section->getSectionName()] = section;
    }

private:
    const Date_t _started;
    bool _runCalled;

    typedef map<string, ServerStatusSection*> SectionMap;
    SectionMap _sections;
};

namespace {

// This widget ensures that the serverStatus command is registered even if no
// server status sections are registered.

const struct CmdServerStatusInstantiator {
    explicit CmdServerStatusInstantiator() {
        getInstance();
    }

    static CmdServerStatus& getInstance() {
        static CmdServerStatus instance;
        return instance;
    }
} kDoNotMentionThisVariable;

}  // namespace

ServerStatusSection::ServerStatusSection(const string& sectionName) : _sectionName(sectionName) {
    CmdServerStatusInstantiator::getInstance().addSection(this);
}

OpCounterServerStatusSection::OpCounterServerStatusSection(const string& sectionName,
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
        asserts.append("rollovers", assertionCount.rollovers.loadRelaxed());
        return asserts.obj();
    }

} asserts;

class MemBase : public ServerStatusMetric {
public:
    MemBase() : ServerStatusMetric(".mem.bits") {}
    virtual void appendAtLeaf(BSONObjBuilder& b) const {
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
} memBase;

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
