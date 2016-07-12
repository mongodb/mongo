/**
*    Copyright (C) 2008-2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <list>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_interface.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/map_util.h"

namespace mongo {

using std::unique_ptr;
using std::list;
using std::string;
using std::stringstream;

namespace repl {

void appendReplicationInfo(OperationContext* txn, BSONObjBuilder& result, int level) {
    ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
    if (replCoord->getSettings().usingReplSets()) {
        IsMasterResponse isMasterResponse;
        replCoord->fillIsMasterForReplSet(&isMasterResponse);
        result.appendElements(isMasterResponse.toBSON());
        if (level) {
            replCoord->appendSlaveInfoData(&result);
        }
        return;
    }

    // TODO(dannenberg) replAllDead is bad and should be removed when master slave is removed
    if (replAllDead) {
        result.append("ismaster", 0);
        string s = string("dead: ") + replAllDead;
        result.append("info", s);
    } else {
        result.appendBool("ismaster",
                          getGlobalReplicationCoordinator()->isMasterForReportingPurposes());
    }

    if (level) {
        BSONObjBuilder sources(result.subarrayStart("sources"));

        int n = 0;
        list<BSONObj> src;
        {
            const char* localSources = "local.sources";
            AutoGetCollectionForRead ctx(txn, localSources);
            unique_ptr<PlanExecutor> exec(InternalPlanner::collectionScan(
                txn, localSources, ctx.getCollection(), PlanExecutor::YIELD_MANUAL));
            BSONObj obj;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
                src.push_back(obj.getOwned());
            }

            // Non-yielding collection scans from InternalPlanner will never error.
            invariant(PlanExecutor::IS_EOF == state);
        }

        for (list<BSONObj>::const_iterator i = src.begin(); i != src.end(); i++) {
            BSONObj s = *i;
            BSONObjBuilder bb;
            bb.append(s["host"]);
            string sourcename = s["source"].valuestr();
            if (sourcename != "main")
                bb.append(s["source"]);
            {
                BSONElement e = s["syncedTo"];
                BSONObjBuilder t(bb.subobjStart("syncedTo"));
                t.appendDate("time", e.timestampTime());
                t.append("inc", e.timestampInc());
                t.done();
            }

            if (level > 1) {
                wassert(!txn->lockState()->isLocked());
                // note: there is no so-style timeout on this connection; perhaps we should have
                // one.
                ScopedDbConnection conn(s["host"].valuestr());

                DBClientConnection* cliConn = dynamic_cast<DBClientConnection*>(&conn.conn());
                if (cliConn && replAuthenticate(cliConn)) {
                    BSONObj first = conn->findOne((string) "local.oplog.$" + sourcename,
                                                  Query().sort(BSON("$natural" << 1)));
                    BSONObj last = conn->findOne((string) "local.oplog.$" + sourcename,
                                                 Query().sort(BSON("$natural" << -1)));
                    bb.appendDate("masterFirst", first["ts"].timestampTime());
                    bb.appendDate("masterLast", last["ts"].timestampTime());
                    const auto lag = (last["ts"].timestampTime() - s["syncedTo"].timestampTime());
                    bb.append("lagSeconds", durationCount<Milliseconds>(lag) / 1000.0);
                }
                conn.done();
            }

            sources.append(BSONObjBuilder::numStr(n++), bb.obj());
        }

        sources.done();

        replCoord->appendSlaveInfoData(&result);
    }
}

class ReplicationInfoServerStatus : public ServerStatusSection {
public:
    ReplicationInfoServerStatus() : ServerStatusSection("repl") {}
    bool includeByDefault() const {
        return true;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const {
        if (!getGlobalReplicationCoordinator()->isReplEnabled()) {
            return BSONObj();
        }

        int level = configElement.numberInt();

        BSONObjBuilder result;
        appendReplicationInfo(txn, result, level);
        getGlobalReplicationCoordinator()->processReplSetGetRBID(&result);

        return result.obj();
    }

} replicationInfoServerStatus;

class OplogInfoServerStatus : public ServerStatusSection {
public:
    OplogInfoServerStatus() : ServerStatusSection("oplog") {}
    bool includeByDefault() const {
        return false;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const {
        ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
        if (!replCoord->isReplEnabled()) {
            return BSONObj();
        }

        BSONObjBuilder result;
        // TODO(siyuan) Output term of OpTime
        result.append("latestOptime", replCoord->getMyLastAppliedOpTime().getTimestamp());

        const std::string& oplogNS =
            replCoord->getReplicationMode() == ReplicationCoordinator::modeReplSet
            ? rsOplogName
            : masterSlaveOplogName;
        BSONObj o;
        uassert(17347,
                "Problem reading earliest entry from oplog",
                Helpers::getSingleton(txn, oplogNS.c_str(), o));
        result.append("earliestOptime", o["ts"].timestamp());
        return result.obj();
    }
} oplogInfoServerStatus;

class CmdIsMaster : public Command {
public:
    virtual bool requiresAuth() {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "Check if this server is primary for a replica pair/set; also if it is --master or "
                "--slave in simple master/slave setups.\n";
        help << "{ isMaster : 1 }";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required
    CmdIsMaster() : Command("isMaster", true, "ismaster") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        /* currently request to arbiter is (somewhat arbitrarily) an ismaster request that is not
           authenticated.
        */
        if (cmdObj["forShell"].trueValue()) {
            LastError::get(txn->getClient()).disable();
        }

        // Tag connections to avoid closing them on stepdown.
        auto hangUpElement = cmdObj["hangUpOnStepDown"];
        if (!hangUpElement.eoo() && !hangUpElement.trueValue()) {
            auto session = txn->getClient()->session();
            if (session) {
                session->replaceTags(session->getTags() |
                                     executor::NetworkInterface::kMessagingPortKeepOpen);
            }
        }
        appendReplicationInfo(txn, result, 0);

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            result.append("configsvr", 1);
        }

        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
        result.appendNumber("maxWriteBatchSize", BatchedCommandRequest::kMaxWriteBatchSize);
        result.appendDate("localTime", jsTime());
        result.append("maxWireVersion", WireSpec::instance().maxWireVersionIncoming);
        result.append("minWireVersion", WireSpec::instance().minWireVersionIncoming);
        result.append("readOnly", storageGlobalParams.readOnly);

        const auto parameter = mapFindWithDefault(ServerParameterSet::getGlobal()->getMap(),
                                                  "automationServiceDescriptor");
        if (parameter)
            parameter->append(txn, result, "automationServiceDescriptor");

        return true;
    }
} cmdismaster;

OpCounterServerStatusSection replOpCounterServerStatusSection("opcountersRepl", &replOpCounters);

}  // namespace repl
}  // namespace mongo
