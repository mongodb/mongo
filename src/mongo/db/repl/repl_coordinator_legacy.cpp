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
#include "mongo/bson/optime.h"
#include "mongo/db/instance.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/oplog.h" // for newRepl()
#include "mongo/db/repl/repl_set_seed_list.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replset_commands.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_initiate.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"

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

    ReplicationCoordinator::StatusAndDuration LegacyReplicationCoordinator::awaitReplication(
            const OperationContext* txn,
            const OpTime& ts,
            const WriteConcernOptions& writeConcern) {

        Timer timeoutTimer;

        if (writeConcern.wNumNodes <= 1 && writeConcern.wMode.empty()) {
            // no desired replication check
            return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
        }

        const Mode replMode = getReplicationMode();
        if (replMode == modeNone || serverGlobalParams.configsvr) {
            // no replication check needed (validated above)
            return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
        }

        if (writeConcern.wMode == "majority" && replMode == modeMasterSlave) {
            // with master/slave, majority is equivalent to w=1
            return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
        }

        try {
            while (1) {
                if (!writeConcern.wMode.empty()) {
                    if (opReplicatedEnough(ts, writeConcern.wMode)) {
                        return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
                    }
                }
                else if (opReplicatedEnough(ts, writeConcern.wNumNodes)) {
                    return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
                }

                if (writeConcern.wTimeout > 0 && timeoutTimer.millis() >= writeConcern.wTimeout) {
                    return StatusAndDuration(Status(ErrorCodes::ExceededTimeLimit,
                                                     "waiting for replication timed out"),
                                              Milliseconds(timeoutTimer.millis()));
                }

                if (writeConcern.wTimeout == -1) {
                    return StatusAndDuration(Status(ErrorCodes::ExceededTimeLimit,
                                                     "replication not finished when checked"),
                                              Milliseconds(timeoutTimer.millis()));
                }

                // TODO (dannenberg) is this the best sleep amount?
                sleepmillis(1);
                txn->checkForInterrupt();
            }

        }
        catch (const DBException& ex) {
            return StatusAndDuration(ex.toStatus(), Milliseconds(timeoutTimer.millis()));
        }
    }

    ReplicationCoordinator::StatusAndDuration 
            LegacyReplicationCoordinator::awaitReplicationOfLastOp(
                    const OperationContext* txn,
                    const WriteConcernOptions& writeConcern) {
        return awaitReplication(txn, cc().getLastOp(), writeConcern);
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

    bool LegacyReplicationCoordinator::isMasterForReportingPurposes() {
        // we must check replSet since because getReplicationMode() isn't aware of modeReplSet
        // until theReplSet is initialized
        if (replSet) {
            if (theReplSet && getCurrentMemberState().primary()) {
                return true;
            }
            return false;
        }

        if (!replSettings.slave)
            return true;

        if (replAllDead) {
            return false;
        }

        if (replSettings.master) {
            // if running with --master --slave, allow.
            return true;
        }

        //TODO: Investigate if this is needed/used, see SERVER-9188
        if (cc().isGod()) {
            return true;
        }

        return false;
    }

    bool LegacyReplicationCoordinator::canAcceptWritesForDatabase(const StringData& dbName) {
        // we must check replSet since because getReplicationMode() isn't aware of modeReplSet
        // until theReplSet is initialized
        if (replSet) {
            if (theReplSet && getCurrentMemberState().primary()) {
                return true;
            }
            return dbName == "local";
        }

        if (!replSettings.slave)
            return true;

        if (replAllDead) {
            return dbName == "local";
        }

        if (replSettings.master) {
            // if running with --master --slave, allow.
            return true;
        }

        //TODO: Investigate if this is needed/used, see SERVER-9188
        if (cc().isGod()) {
            return true;
        }

        return dbName == "local";
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

    Status LegacyReplicationCoordinator::setLastOptime(const OID& rid,
                                                       const OpTime& ts,
                                                       const BSONObj& config) {
        std::string oplogNs = getReplicationMode() == modeReplSet?
                "local.oplog.rs" : "local.oplog.$main";
        if (!updateSlaveTracking(BSON("_id" << rid), config, oplogNs, ts)) {
            return Status(ErrorCodes::NodeNotFound,
                          str::stream() << "could not update node with _id: " 
                                        << config["_id"].Int()
                                        << " beacuse it cannot be found in current ReplSetConfig");
        }

        if (getReplicationMode() == modeReplSet && !getCurrentMemberState().primary()) {
            // pass along if we are not primary
            LOG(2) << "received notification that " << config << " has reached optime: "
                   << ts.toStringPretty();
            theReplSet->syncSourceFeedback.updateMap(rid, ts);
        }
        return Status::OK();
    }
    
    void LegacyReplicationCoordinator::processReplSetGetStatus(BSONObjBuilder* result) {
        theReplSet->summarizeStatus(*result);
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

namespace {
    Status _checkReplEnabledForCommand(BSONObjBuilder* result) {
        if( !replSet ) {
            if (serverGlobalParams.configsvr) {
                result->append("info", "configsvr"); // for shell prompt
            }
            return Status(ErrorCodes::NoReplicationEnabled, "not running with --replSet");
        }

        if( theReplSet == 0 ) {
            result->append("startupStatus", ReplSet::startupStatus);
            if( ReplSet::startupStatus == ReplSet::EMPTYCONFIG )
                result->append("info", "run rs.initiate(...) if not yet done for the set");
            return Status(ErrorCodes::NotYetInitialized, ReplSet::startupStatusMsg.empty() ?
                    "replset unknown error 2" : ReplSet::startupStatusMsg.get());
        }

        return Status::OK();
    }
} // namespace

    Status LegacyReplicationCoordinator::processReplSetReconfig(OperationContext* txn,
                                                                const BSONObj& newConfigObj,
                                                                bool force,
                                                                BSONObjBuilder* resultObj) {

        if( force && !theReplSet ) {
            replSettings.reconfig = newConfigObj.getOwned();
            resultObj->append("msg",
                              "will try this config momentarily, try running rs.conf() again in a "
                                      "few seconds");
            return Status::OK();
        }

        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }

        if( !force && !theReplSet->box.getState().primary() ) {
            return Status(ErrorCodes::NotMaster,
                          "replSetReconfig command must be sent to the current replica set "
                                  "primary.");
        }

        try {
            {
                // just make sure we can get a write lock before doing anything else.  we'll
                // reacquire one later.  of course it could be stuck then, but this check lowers the
                // risk if weird things are up - we probably don't want a change to apply 30 minutes
                // after the initial attempt.
                time_t t = time(0);
                Lock::GlobalWrite lk(txn->lockState());
                if( time(0)-t > 20 ) {
                    return Status(ErrorCodes::ExceededTimeLimit,
                                  "took a long time to get write lock, so not initiating.  "
                                          "Initiate when server less busy?");
                }
            }


            scoped_ptr<ReplSetConfig> newConfig(ReplSetConfig::make(newConfigObj, force));

            log() << "replSet replSetReconfig config object parses ok, " <<
                    newConfig->members.size() << " members specified" << rsLog;

            Status status = ReplSetConfig::legalChange(theReplSet->getConfig(), *newConfig);
            if (!status.isOK()) {
                return status;
            }

            checkMembersUpForConfigChange(*newConfig, *resultObj, false);

            log() << "replSet replSetReconfig [2]" << rsLog;

            theReplSet->haveNewConfig(*newConfig, true);
            ReplSet::startupStatusMsg.set("replSetReconfig'd");
        }
        catch(const DBException& e) {
            log() << "replSet replSetReconfig exception: " << e.what() << rsLog;
            return e.toStatus();
        }

        resetSlaveCache();
        return Status::OK();
    }

} // namespace repl
} // namespace mongo
