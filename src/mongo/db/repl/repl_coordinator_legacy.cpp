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
#include "mongo/db/dbhelpers.h"
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
#include "mongo/util/log.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kReplication);

namespace repl {

    LegacyReplicationCoordinator::LegacyReplicationCoordinator() {
        // this is ok but micros or combo with some rand() and/or 64 bits might be better --
        // imagine a restart and a clock correction simultaneously (very unlikely but possible...)
        _rbid = (int) curTimeMillis64();
    }
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

    Status LegacyReplicationCoordinator::canServeReadsFor(const NamespaceString& ns, bool slaveOk) {
        if (cc().isGod()) {
            return Status::OK();
        }
        if (canAcceptWritesForDatabase(ns.db())) {
            return Status::OK();
        }
        if (getReplicationMode() == modeMasterSlave && replSettings.slave == SimpleSlave) {
            return Status::OK();
        }
        if (slaveOk) {
            if (getReplicationMode() == modeMasterSlave || getReplicationMode() == modeNone) {
                return Status::OK();
            }
            if (getCurrentMemberState().secondary()) {
                return Status::OK();
            }
            return Status(ErrorCodes::NotMasterOrSecondaryCode,
                         "not master or secondary; cannot currently read from this replSet member");
        }
        return Status(ErrorCodes::NotMasterNoSlaveOkCode,
                      "not master and slaveOk=false");

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

    bool LegacyReplicationCoordinator::setMaintenanceMode(bool activate) {
        return theReplSet->setMaintenanceMode(activate);
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
                                                                const ReplSetReconfigArgs& args,
                                                                BSONObjBuilder* resultObj) {

        if( args.force && !theReplSet ) {
            replSettings.reconfig = args.newConfigObj.getOwned();
            resultObj->append("msg",
                              "will try this config momentarily, try running rs.conf() again in a "
                                      "few seconds");
            return Status::OK();
        }

        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }

        if( !args.force && !theReplSet->box.getState().primary() ) {
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


            scoped_ptr<ReplSetConfig> newConfig(ReplSetConfig::make(args.newConfigObj, args.force));

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

    Status LegacyReplicationCoordinator::processReplSetInitiate(OperationContext* txn,
                                                                const BSONObj& givenConfig,
                                                                BSONObjBuilder* resultObj) {

        log() << "replSet replSetInitiate admin command received from client" << rsLog;

        if( !replSet ) {
            return Status(ErrorCodes::NoReplicationEnabled, "server is not running with --replSet");
        }

        if( theReplSet ) {
            resultObj->append("info",
                              "try querying " + rsConfigNs + " to see current configuration");
            return Status(ErrorCodes::AlreadyInitialized, "already initialized");
        }

        try {
            {
                // just make sure we can get a write lock before doing anything else.  we'll
                // reacquire one later.  of course it could be stuck then, but this check lowers the
                // risk if weird things are up.
                time_t t = time(0);
                Lock::GlobalWrite lk(txn->lockState());
                if( time(0)-t > 10 ) {
                    return Status(ErrorCodes::ExceededTimeLimit,
                                  "took a long time to get write lock, so not initiating.  "
                                          "Initiate when server less busy?");
                }

                /* check that we don't already have an oplog.  that could cause issues.
                   it is ok if the initiating member has *other* data than that.
                   */
                BSONObj o;
                if( Helpers::getFirst(txn, rsoplog, o) ) {
                    return Status(ErrorCodes::AlreadyInitialized,
                                  rsoplog + string(" is not empty on the initiating member.  "
                                          "cannot initiate."));
                }
            }

            if( ReplSet::startupStatus == ReplSet::BADCONFIG ) {
                resultObj->append("info", ReplSet::startupStatusMsg.get());
                return Status(ErrorCodes::InvalidReplicaSetConfig,
                              "server already in BADCONFIG state (check logs); not initiating");
            }
            if( ReplSet::startupStatus != ReplSet::EMPTYCONFIG ) {
                resultObj->append("startupStatus", ReplSet::startupStatus);
                resultObj->append("info", replSettings.replSet);
                return Status(ErrorCodes::InvalidReplicaSetConfig,
                              "all members and seeds must be reachable to initiate set");
            }

            BSONObj configObj;
            if (!givenConfig.isEmpty()) {
                configObj = givenConfig;
            } else {
                resultObj->append("info2", "no configuration explicitly specified -- making one");
                log() << "replSet info initiate : no configuration specified.  "
                        "Using a default configuration for the set" << rsLog;

                string name;
                vector<HostAndPort> seeds;
                set<HostAndPort> seedSet;
                parseReplSetSeedList(replSettings.replSet, name, seeds, seedSet); // may throw...

                BSONObjBuilder b;
                b.append("_id", name);
                BSONObjBuilder members;
                members.append("0", BSON( "_id" << 0 << "host" << HostAndPort::me().toString() ));
                resultObj->append("me", HostAndPort::me().toString());
                for( unsigned i = 0; i < seeds.size(); i++ ) {
                    members.append(BSONObjBuilder::numStr(i+1),
                                   BSON( "_id" << i+1 << "host" << seeds[i].toString()));
                }
                b.appendArray("members", members.obj());
                configObj = b.obj();
                log() << "replSet created this configuration for initiation : " <<
                        configObj.toString() << rsLog;
            }

            scoped_ptr<ReplSetConfig> newConfig;
            try {
                newConfig.reset(ReplSetConfig::make(configObj));
            } catch (const DBException& e) {
                log() << "replSet replSetInitiate exception: " << e.what() << rsLog;
                return Status(ErrorCodes::InvalidReplicaSetConfig,
                              mongoutils::str::stream() << "couldn't parse cfg object " << e.what());
            }

            if( newConfig->version > 1 ) {
                return Status(ErrorCodes::InvalidReplicaSetConfig,
                              "can't initiate with a version number greater than 1");
            }

            log() << "replSet replSetInitiate config object parses ok, " <<
                    newConfig->members.size() << " members specified" << rsLog;

            checkMembersUpForConfigChange(*newConfig, *resultObj, true);

            log() << "replSet replSetInitiate all members seem up" << rsLog;

            createOplog();

            Lock::GlobalWrite lk(txn->lockState());
            BSONObj comment = BSON( "msg" << "initiating set");
            newConfig->saveConfigLocally(comment);
            log() << "replSet replSetInitiate config now saved locally.  "
                "Should come online in about a minute." << rsLog;
            resultObj->append("info",
                              "Config now saved locally.  Should come online in about a minute.");
            ReplSet::startupStatus = ReplSet::SOON;
            ReplSet::startupStatusMsg.set("Received replSetInitiate - "
                                          "should come online shortly.");
        }
        catch(const DBException& e ) {
            return e.toStatus();
        }

        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetGetRBID(BSONObjBuilder* resultObj) {
        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }
        resultObj->append("rbid", _rbid);
        return Status::OK();
    }

namespace {
    bool _shouldVeto(unsigned id, std::string* errmsg) {
        const Member* primary = theReplSet->box.getPrimary();
        const Member* hopeful = theReplSet->findById(id);
        const Member *highestPriority = theReplSet->getMostElectable();

        if (!hopeful) {
            *errmsg = str::stream() << "replSet couldn't find member with id " << id;
            return true;
        }

        if (theReplSet->isPrimary() &&
            theReplSet->lastOpTimeWritten >= hopeful->hbinfo().opTime) {
            // hbinfo is not updated, so we have to check the primary's last optime separately
            *errmsg = str::stream() << "I am already primary, " << hopeful->fullName() <<
                " can try again once I've stepped down";
            return true;
        }

        if (primary &&
                (hopeful->hbinfo().id() != primary->hbinfo().id()) &&
                (primary->hbinfo().opTime >= hopeful->hbinfo().opTime)) {
            // other members might be aware of more up-to-date nodes
            *errmsg = str::stream() << hopeful->fullName() <<
                " is trying to elect itself but " << primary->fullName() <<
                " is already primary and more up-to-date";
            return true;
        }

        if (highestPriority &&
            highestPriority->config().priority > hopeful->config().priority) {
            *errmsg = str::stream() << hopeful->fullName() << " has lower priority than " <<
                highestPriority->fullName();
            return true;
        }

        if (!theReplSet->isElectable(id)) {
            *errmsg = str::stream() << "I don't think " << hopeful->fullName() <<
                " is electable";
            return true;
        }

        return false;
    }
} // namespace

    Status LegacyReplicationCoordinator::processReplSetFresh(const ReplSetFreshArgs& args,
                                                             BSONObjBuilder* resultObj) {
        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }

        if( args.setName != theReplSet->name() ) {
            return Status(ErrorCodes::ReplicaSetNotFound,
                          str::stream() << "wrong repl set name. Expected: " <<
                                  theReplSet->name() << ", received: " << args.setName);
        }

        bool weAreFresher = false;
        if( theReplSet->config().version > args.cfgver ) {
            log() << "replSet member " << args.who << " is not yet aware its cfg version " <<
                    args.cfgver << " is stale" << rsLog;
            resultObj->append("info", "config version stale");
            weAreFresher = true;
        }
        // check not only our own optime, but any other member we can reach
        else if( args.opTime < theReplSet->lastOpTimeWritten ||
                 args.opTime < theReplSet->lastOtherOpTime())  {
            weAreFresher = true;
        }
        resultObj->appendDate("opTime", theReplSet->lastOpTimeWritten.asDate());
        resultObj->append("fresher", weAreFresher);

        std::string errmsg;
        bool veto = _shouldVeto(args.id, &errmsg);
        resultObj->append("veto", veto);
        if (veto) {
            resultObj->append("errmsg", errmsg);
        }

        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetElect(const ReplSetElectArgs& args,
                                                             BSONObjBuilder* resultObj) {
        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }
        theReplSet->electCmdReceived(args.set, args.whoid, args.cfgver, args.round, resultObj);
        return Status::OK();
    }

    void LegacyReplicationCoordinator::incrementRollbackID() {
        ++_rbid;
    }

    Status LegacyReplicationCoordinator::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }
        if (theReplSet->freeze(secs)) {
            if (secs == 0) {
                resultObj->append("info","unfreezing");
            }
        }
        if (secs == 1) {
            resultObj->append("warning", "you really want to freeze for only 1 second?");
        }
        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetMaintenance(bool activate,
                                                                   BSONObjBuilder* resultObj) {
        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }
        if (!setMaintenanceMode(activate)) {
            if (theReplSet->isPrimary()) {
                return Status(ErrorCodes::NotSecondary, "primaries can't modify maintenance mode");
            }
            else {
                return Status(ErrorCodes::OperationFailed, "already out of maintenance mode");
            }
        }

        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetSyncFrom(const std::string& target,
                                                                BSONObjBuilder* resultObj) {
        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }
        resultObj->append("syncFromRequested", target);

        return theReplSet->forceSyncFrom(target, resultObj);
    }

    Status LegacyReplicationCoordinator::processReplSetUpdatePosition(const BSONArray& updates,
                                                                      BSONObjBuilder* resultObj) {
        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }

        BSONForEach(elem, updates) {
            BSONObj entry = elem.Obj();
            OID id = entry["_id"].OID();
            OpTime ot = entry["optime"]._opTime();
            BSONObj config = entry["config"].Obj();
            Status status = setLastOptime(id, ot, config);
            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetUpdatePositionHandshake(
            const BSONObj& handshake,
            BSONObjBuilder* resultObj) {
        Status status = _checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }

        if (!cc().gotHandshake(handshake)) {
            return Status(ErrorCodes::NodeNotFound,
                          "node could not be found in replica set config during handshake");
        }

        // if we aren't primary, pass the handshake along
        if (!theReplSet->isPrimary()) {
            theReplSet->syncSourceFeedback.forwardSlaveHandshake();
        }
        return Status::OK();
    }
} // namespace repl
} // namespace mongo
