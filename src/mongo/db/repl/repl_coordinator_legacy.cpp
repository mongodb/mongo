/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/repl/repl_coordinator_legacy.h"

#include <boost/thread/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/oplog.h" // for newRepl()
#include "mongo/db/repl/repl_set_seed_list.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replset_commands.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/repl/bgsync.h"

namespace mongo {
namespace repl {

    LegacyReplicationCoordinator::LegacyReplicationCoordinator() {}
    LegacyReplicationCoordinator::~LegacyReplicationCoordinator() {}

    void LegacyReplicationCoordinator::startReplication(TopologyCoordinator*,
                                                        ReplicationExecutor::NetworkInterface*) {
        // if we are going to be a replica set, we aren't doing other forms of replication.
        if (!replSettings.replSet.empty()) {
            if (replSettings.slave || replSettings.master) {
                log() << "***" << endl;
                log() << "ERROR: can't use --slave or --master replication options with --replSet";
                log() << "***" << endl;
            }
            newRepl();

            replSet = true;
            ReplSetSeedList *replSetSeedList = new ReplSetSeedList(replSettings.replSet);
            boost::thread t(stdx::bind(&startReplSets, replSetSeedList));
        } else {
            startMasterSlave();
        }
    }

    void LegacyReplicationCoordinator::shutdown() {
        // TODO
    }

    bool LegacyReplicationCoordinator::isShutdownOkay() const {
        // TODO
        return false;
    }

    ReplicationCoordinator::Mode LegacyReplicationCoordinator::getReplicationMode() const {
        if (theReplSet) {
            return modeReplSet;
        } else if (replSettings.slave || replSettings.master) {
            return modeMasterSlave;
        }
        return modeNone;
    }

    MemberState LegacyReplicationCoordinator::getCurrentMemberState() const {
        invariant(getReplicationMode() == modeReplSet);
        return theReplSet->state();
    }

    Status LegacyReplicationCoordinator::awaitReplication(const OpTime& ts,
                                                          const WriteConcernOptions& writeConcern,
                                                          Milliseconds timeout) {
        // TODO
        return Status::OK();
    }

    Status LegacyReplicationCoordinator::stepDown(bool force,
                                                  const Milliseconds& waitTime,
                                                  const Milliseconds& stepdownTime) {
        return _stepDownHelper(force, waitTime, stepdownTime, Milliseconds(0));
    }

    Status LegacyReplicationCoordinator::stepDownAndWaitForSecondary(
            const Milliseconds& initialWaitTime,
            const Milliseconds& stepdownTime,
            const Milliseconds& postStepdownWaitTime) {
        return _stepDownHelper(false, initialWaitTime, stepdownTime, postStepdownWaitTime);
    }

namespace {
    /**
     * Waits up to timeout milliseconds for one secondary to get within threshold milliseconds
     * of us.
     */
    Status _waitForSecondary(const ReplicationCoordinator::Milliseconds& timeout,
                             const ReplicationCoordinator::Milliseconds& threshold) {
        if (theReplSet->getConfig().members.size() <= 1) {
            return Status(ErrorCodes::ExceededTimeLimit,
                          mongoutils::str::stream() << "no secondaries within " <<
                                  threshold.total_seconds() << " seconds of my optime");
        }

        long long timeoutTime, now, start;
        timeoutTime = now = start = curTimeMillis64()/1000;
        timeoutTime += timeout.total_seconds();

        OpTime lastOp = repl::theReplSet->lastOpTimeWritten;
        OpTime closest = repl::theReplSet->lastOtherElectableOpTime();
        long long int diff = lastOp.getSecs() - closest.getSecs();
        while (now <= timeoutTime && (diff < 0 || diff > threshold.total_seconds())) {
            sleepsecs(1);
            now = curTimeMillis64() / 1000;

            lastOp = repl::theReplSet->lastOpTimeWritten;
            closest = repl::theReplSet->lastOtherElectableOpTime();
            diff = lastOp.getSecs() - closest.getSecs();
        }

        if (diff < 0) {
            // not our problem but we'll wait until things settle down
            return Status(ErrorCodes::SecondaryAheadOfPrimary,
                          "someone is ahead of the primary?");
        }
        if (diff > threshold.total_seconds()) {
            return Status(ErrorCodes::ExceededTimeLimit,
                          mongoutils::str::stream() << "no secondaries within " <<
                                  threshold.total_seconds() << " seconds of my optime");
        }
        return Status::OK();
    }
} // namespace

    Status LegacyReplicationCoordinator::_stepDownHelper(bool force,
                                                         const Milliseconds& initialWaitTime,
                                                         const Milliseconds& stepdownTime,
                                                         const Milliseconds& postStepdownWaitTime) {
        invariant(getReplicationMode() == modeReplSet);
        if (!getCurrentMemberState().primary()) {
            return Status(ErrorCodes::NotMaster, "not primary so can't step down");
        }

        if (!force) {
            Status status = _waitForSecondary(initialWaitTime, Milliseconds(10 * 1000));
            if (!status.isOK()) {
                return status;
            }
        }

        // step down
        bool worked = repl::theReplSet->stepDown(stepdownTime.total_seconds());
        if (!worked) {
            return Status(ErrorCodes::NotMaster, "not primary so can't step down");
        }

        if (postStepdownWaitTime.total_milliseconds() > 0) {
            log() << "waiting for secondaries to catch up" << endl;

            // The only caller of this with a non-zero postStepdownWaitTime is
            // stepDownAndWaitForSecondary, and the only caller of that is the shutdown command
            // which doesn't actually care if secondaries failed to catch up here, so we ignore the
            // return status of _waitForSecondary
            _waitForSecondary(postStepdownWaitTime, Milliseconds(0));
        }
        return Status::OK();
    }

    bool LegacyReplicationCoordinator::canAcceptWritesForDatabase(const StringData& dbName) {
        if (_isMaster())
            return true;
        if (dbName == "local")
            return true;
        return false;
    }

    bool LegacyReplicationCoordinator::canServeReadsFor(const NamespaceString& collection) {
        // TODO
        return false;
    }

    bool LegacyReplicationCoordinator::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        if (!idx->unique()) {
            return false;
        }
        if (!theReplSet) {
            return false;
        }
        // see SERVER-6671
        MemberState ms = theReplSet->state();
        if (! ((ms == MemberState::RS_STARTUP2) ||
               (ms == MemberState::RS_RECOVERING) ||
               (ms == MemberState::RS_ROLLBACK))) {
            return false;
        }
        // 2 is the oldest oplog version where operations
        // are fully idempotent.
        if (theReplSet->oplogVersion < 2) {
            return false;
        }
        // Never ignore _id index
        if (idx->isIdIndex()) {
            return false;
        }

        return true;
    }

    Status LegacyReplicationCoordinator::setLastOptime(const HostAndPort& member,
                                                       const OpTime& ts) {
        // TODO
        return Status::OK();
    }
    
    Status LegacyReplicationCoordinator::processHeartbeat(const BSONObj& cmdObj, 
                                                          BSONObjBuilder* resultObj) {
        if( cmdObj["pv"].Int() != 1 ) {
            return Status(ErrorCodes::BadValue, "incompatible replset protocol version");
        }

        {
            string s = string(cmdObj.getStringField("replSetHeartbeat"));
            if (replSettings.ourSetName() != s) {
                log() << "replSet set names do not match, our cmdline: " << replSettings.replSet
                      << rsLog;
                log() << "replSet s: " << s << rsLog;
                resultObj->append("mismatch", true);
                return Status(ErrorCodes::BadValue, "repl set names do not match");
            }
        }

        resultObj->append("rs", true);
        if( (theReplSet == 0) || (theReplSet->startupStatus == ReplSetImpl::LOADINGCONFIG) ) {
            string from( cmdObj.getStringField("from") );
            if( !from.empty() ) {
                scoped_lock lck( replSettings.discoveredSeeds_mx );
                replSettings.discoveredSeeds.insert(from);
            }
            resultObj->append("hbmsg", "still initializing");
            return Status::OK();
        }

        if( theReplSet->name() != cmdObj.getStringField("replSetHeartbeat") ) {
            resultObj->append("mismatch", true);
            return Status(ErrorCodes::BadValue, "repl set names do not match (2)");
        }
        resultObj->append("set", theReplSet->name());

        MemberState currentState = theReplSet->state();
        resultObj->append("state", currentState.s);
        if (currentState == MemberState::RS_PRIMARY) {
            resultObj->appendDate("electionTime", theReplSet->getElectionTime().asDate());
        }

        resultObj->append("e", theReplSet->iAmElectable());
        resultObj->append("hbmsg", theReplSet->hbmsg());
        resultObj->append("time", (long long) time(0));
        resultObj->appendDate("opTime", theReplSet->lastOpTimeWritten.asDate());
        const Member *syncTarget = BackgroundSync::get()->getSyncTarget();
        if (syncTarget) {
            resultObj->append("syncingTo", syncTarget->fullName());
        }

        int v = theReplSet->config().version;
        resultObj->append("v", v);
        if( v > cmdObj["v"].Int() )
            *resultObj << "config" << theReplSet->config().asBson();

        Member* from = NULL;
        if (cmdObj.hasField("fromId")) {
            if (v == cmdObj["v"].Int()) {
                from = theReplSet->getMutableMember(cmdObj["fromId"].Int());
            }
        }
        if (!from) {
            from = theReplSet->findByName(cmdObj.getStringField("from"));
            if (!from) {
                return Status::OK();
            }
        }

        // if we thought that this node is down, let it know
        if (!from->hbinfo().up()) {
            resultObj->append("stateDisagreement", true);
        }

        // note that we got a heartbeat from this node
        theReplSet->mgr->send(stdx::bind(&ReplSet::msgUpdateHBRecv,
                                         theReplSet, from->hbinfo().id(), time(0)));


        return Status::OK();
    }
} // namespace repl
} // namespace mongo
