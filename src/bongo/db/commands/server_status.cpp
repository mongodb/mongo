// server_status.cpp

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kCommand

#include "bongo/platform/basic.h"

#include "bongo/config.h"
#include "bongo/db/auth/action_set.h"
#include "bongo/db/auth/action_type.h"
#include "bongo/db/auth/authorization_manager.h"
#include "bongo/db/auth/authorization_session.h"
#include "bongo/db/auth/privilege.h"
#include "bongo/db/client.h"
#include "bongo/db/commands.h"
#include "bongo/db/commands/server_status.h"
#include "bongo/db/commands/server_status_internal.h"
#include "bongo/db/commands/server_status_metric.h"
#include "bongo/db/operation_context.h"
#include "bongo/db/service_context.h"
#include "bongo/db/stats/counters.h"
#include "bongo/platform/process_id.h"
#include "bongo/transport/message_compressor_registry.h"
#include "bongo/transport/transport_layer.h"
#include "bongo/util/log.h"
#include "bongo/util/net/hostname_canonicalization.h"
#include "bongo/util/net/ssl_manager.h"
#include "bongo/util/processinfo.h"
#include "bongo/util/ramlog.h"
#include "bongo/util/time_support.h"
#include "bongo/util/version.h"

namespace bongo {

using std::endl;
using std::map;
using std::string;
using std::stringstream;

class CmdServerStatus : public Command {
public:
    CmdServerStatus() : Command("serverStatus", true), _started(Date_t::now()), _runCalled(false) {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << "returns lots of administrative server statistics";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::serverStatus);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        _runCalled = true;

        const auto service = txn->getServiceContext();
        const auto clock = service->getFastClockSource();
        const auto runStart = clock->now();
        BSONObjBuilder timeBuilder(256);

        const auto authSession = AuthorizationSession::get(Client::getCurrent());

        // --- basic fields that are global

        result.append("host", prettyHostName());
        result.append("version", VersionInfoInterface::instance().version());
        result.append("process", serverGlobalParams.binaryName);
        result.append("pid", ProcessId::getCurrent().asLongLong());
        result.append("uptime", (double)(time(0) - serverGlobalParams.started));
        auto uptime = clock->now() - _started;
        result.append("uptimeMillis", durationCount<Milliseconds>(uptime));
        result.append("uptimeEstimate", durationCount<Seconds>(uptime));
        result.appendDate("localTime", jsTime());

        timeBuilder.appendNumber("after basic",
                                 durationCount<Milliseconds>(clock->now() - runStart));

        // --- all sections

        for (SectionMap::const_iterator i = _sections->begin(); i != _sections->end(); ++i) {
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

            section->appendSection(txn, elem, &result);
            timeBuilder.appendNumber(
                static_cast<string>(str::stream() << "after " << section->getSectionName()),
                durationCount<Milliseconds>(clock->now() - runStart));
        }

        // --- counters
        bool includeMetricTree = MetricTree::theMetricTree != NULL;
        if (cmdObj["metrics"].type() && !cmdObj["metrics"].trueValue())
            includeMetricTree = false;

        if (includeMetricTree) {
            MetricTree::theMetricTree->appendTo(result);
        }

        // --- some hard coded global things hard to pull out

        {
            RamLog::LineIterator rl(RamLog::get("warnings"));
            if (rl.lastWrite() >= time(0) - (10 * 60)) {  // only show warnings from last 10 minutes
                BSONArrayBuilder arr(result.subarrayStart("warnings"));
                while (rl.more()) {
                    arr.append(rl.next());
                }
                arr.done();
            }
        }

        auto runElapsed = clock->now() - runStart;
        timeBuilder.appendNumber("at end", durationCount<Milliseconds>(runElapsed));
        if (runElapsed > Milliseconds(1000)) {
            BSONObj t = timeBuilder.obj();
            log() << "serverStatus was very slow: " << t;
            result.append("timing", t);
        }

        return true;
    }

    void addSection(ServerStatusSection* section) {
        verify(!_runCalled);
        if (_sections == 0) {
            _sections = new SectionMap();
        }
        (*_sections)[section->getSectionName()] = section;
    }

private:
    const Date_t _started;
    bool _runCalled;

    typedef map<string, ServerStatusSection*> SectionMap;
    static SectionMap* _sections;
} cmdServerStatus;


CmdServerStatus::SectionMap* CmdServerStatus::_sections = 0;

ServerStatusSection::ServerStatusSection(const string& sectionName) : _sectionName(sectionName) {
    cmdServerStatus.addSection(this);
}

OpCounterServerStatusSection::OpCounterServerStatusSection(const string& sectionName,
                                                           OpCounters* counters)
    : ServerStatusSection(sectionName), _counters(counters) {}

BSONObj OpCounterServerStatusSection::generateSection(OperationContext* txn,
                                                      const BSONElement& configElement) const {
    return _counters->getObj();
}

OpCounterServerStatusSection globalOpCounterServerStatusSection("opcounters", &globalOpCounters);


namespace {

// some universal sections

class Connections : public ServerStatusSection {
public:
    Connections() : ServerStatusSection("connections") {}
    virtual bool includeByDefault() const {
        return true;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const {
        BSONObjBuilder bb;
        auto stats = txn->getServiceContext()->getTransportLayer()->sessionStats();
        bb.append("current", static_cast<int>(stats.numOpenSessions));
        bb.append("available", static_cast<int>(stats.numAvailableSessions));
        bb.append("totalCreated", static_cast<int>(stats.numCreatedSessions));
        return bb.obj();
    }

} connections;

class ExtraInfo : public ServerStatusSection {
public:
    ExtraInfo() : ServerStatusSection("extra_info") {}
    virtual bool includeByDefault() const {
        return true;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const {
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
    virtual bool includeByDefault() const {
        return true;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const {
        BSONObjBuilder asserts;
        asserts.append("regular", assertionCount.regular);
        asserts.append("warning", assertionCount.warning);
        asserts.append("msg", assertionCount.msg);
        asserts.append("user", assertionCount.user);
        asserts.append("rollovers", assertionCount.rollovers);
        return asserts.obj();
    }

} asserts;


class Network : public ServerStatusSection {
public:
    Network() : ServerStatusSection("network") {}
    virtual bool includeByDefault() const {
        return true;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const {
        BSONObjBuilder b;
        networkCounter.append(b);
        appendMessageCompressionStats(&b);
        return b.obj();
    }

} network;

#ifdef BONGO_CONFIG_SSL
class Security : public ServerStatusSection {
public:
    Security() : ServerStatusSection("security") {}
    virtual bool includeByDefault() const {
        return true;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const {
        BSONObj result;
        if (getSSLManager()) {
            result = getSSLManager()->getSSLConfiguration().getServerStatusBSON();
        }

        return result;
    }
} security;
#endif

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

class AdvisoryHostFQDNs final : public ServerStatusSection {
public:
    AdvisoryHostFQDNs() : ServerStatusSection("advisoryHostFQDNs") {}

    bool includeByDefault() const override {
        return false;
    }

    void appendSection(OperationContext* txn,
                       const BSONElement& configElement,
                       BSONObjBuilder* out) const override {
        out->append(
            "advisoryHostFQDNs",
            getHostFQDNs(getHostNameCached(), HostnameCanonicalizationMode::kForwardAndReverse));
    }
} advisoryHostFQDNs;
}  // namespace

}  // namespace bongo
