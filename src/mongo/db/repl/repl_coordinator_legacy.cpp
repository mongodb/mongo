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

#include "mongo/base/status.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replset_commands.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/assert_util.h" // TODO: remove along with invariant from getCurrentMemberState
#include "mongo/util/fail_point_service.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/repl/bgsync.h"

namespace mongo {
namespace repl {

    LegacyReplicationCoordinator::LegacyReplicationCoordinator() {}
    LegacyReplicationCoordinator::~LegacyReplicationCoordinator() {}

    void LegacyReplicationCoordinator::startReplication() {
        // TODO
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
        return theReplSet->state();
    }

    Status LegacyReplicationCoordinator::awaitReplication(const OpTime& ts,
                                                          const WriteConcernOptions& writeConcern,
                                                          Milliseconds timeout) {
        // TODO
        return Status::OK();
    }

    bool LegacyReplicationCoordinator::canAcceptWritesFor(const NamespaceString& collection) {
        // TODO
        return false;
    }

    bool LegacyReplicationCoordinator::canServeReadsFor(const NamespaceString& collection) {
        // TODO
        return false;
    }

    bool LegacyReplicationCoordinator::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        // TODO
        return false;
    }

    Status LegacyReplicationCoordinator::setLastOptime(const HostAndPort& member,
                                                       const OpTime& ts) {
        // TODO
        return Status::OK();
    }

    MONGO_FP_DECLARE(rsDelayHeartbeatResponse);
    
    bool LegacyReplicationCoordinator::processHeartbeat(OperationContext* txn, 
                                                        const BSONObj& cmdObj, 
                                                        std::string* errmsg, 
                                                        BSONObjBuilder* result) {
        if( replSetBlind ) {
            if (theReplSet) {
                *errmsg = str::stream() << theReplSet->selfFullName() << " is blind";
            }
            return false;
        }

        MONGO_FAIL_POINT_BLOCK(rsDelayHeartbeatResponse, delay) {
            const BSONObj& data = delay.getData();
            sleepsecs(data["delay"].numberInt());
        }

        /* we don't call ReplSetCommand::check() here because heartbeat
           checks many things that are pre-initialization. */
        if( !replSet ) {
            *errmsg = "not running with --replSet";
            return false;
        }

        /* we want to keep heartbeat connections open when relinquishing primary.  tag them here. */
        {
            AbstractMessagingPort *mp = cc().port();
            if( mp )
                mp->tag |= ScopedConn::keepOpen;
        }

        if( cmdObj["pv"].Int() != 1 ) {
            *errmsg = "incompatible replset protocol version";
            return false;
        }
        {
            string s = string(cmdObj.getStringField("replSetHeartbeat"));
            if (replSettings.ourSetName() != s) {
                *errmsg = "repl set names do not match";
                log() << "replSet set names do not match, our cmdline: " << replSettings.replSet
                      << rsLog;
                log() << "replSet s: " << s << rsLog;
                result->append("mismatch", true);
                return false;
            }
        }

        result->append("rs", true);
        if( cmdObj["checkEmpty"].trueValue() ) {
            result->append("hasData", replHasDatabases(txn));
        }
        if( (theReplSet == 0) || (theReplSet->startupStatus == ReplSetImpl::LOADINGCONFIG) ) {
            string from( cmdObj.getStringField("from") );
            if( !from.empty() ) {
                scoped_lock lck( replSettings.discoveredSeeds_mx );
                replSettings.discoveredSeeds.insert(from);
            }
            result->append("hbmsg", "still initializing");
            return true;
        }

        if( theReplSet->name() != cmdObj.getStringField("replSetHeartbeat") ) {
            *errmsg = "repl set names do not match (2)";
            result->append("mismatch", true);
            return false;
        }
        result->append("set", theReplSet->name());

        MemberState currentState = theReplSet->state();
        result->append("state", currentState.s);
        if (currentState == MemberState::RS_PRIMARY) {
            result->appendDate("electionTime", theReplSet->getElectionTime().asDate());
        }

        result->append("e", theReplSet->iAmElectable());
        result->append("hbmsg", theReplSet->hbmsg());
        result->append("time", (long long) time(0));
        result->appendDate("opTime", theReplSet->lastOpTimeWritten.asDate());
        const Member *syncTarget = BackgroundSync::get()->getSyncTarget();
        if (syncTarget) {
            result->append("syncingTo", syncTarget->fullName());
        }

        int v = theReplSet->config().version;
        result->append("v", v);
        if( v > cmdObj["v"].Int() )
            *result << "config" << theReplSet->config().asBson();

        Member* from = NULL;
        if (cmdObj.hasField("fromId")) {
            if (v == cmdObj["v"].Int()) {
                from = theReplSet->getMutableMember(cmdObj["fromId"].Int());
            }
        }
        if (!from) {
            from = theReplSet->findByName(cmdObj.getStringField("from"));
            if (!from) {
                return true;
            }
        }

        // if we thought that this node is down, let it know
        if (!from->hbinfo().up()) {
            result->append("stateDisagreement", true);
        }

        // note that we got a heartbeat from this node
        theReplSet->mgr->send(stdx::bind(&ReplSet::msgUpdateHBRecv,
                                         theReplSet, from->hbinfo().id(), time(0)));


        return true;
    }
} // namespace repl
} // namespace mongo
