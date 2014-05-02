/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/repl/sync_source_feedback.h"

#include "mongo/client/constants.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/rs.h"  // theReplSet
#include "mongo/db/storage/mmap_v1/dur_transaction.h"

namespace mongo {

    // used in replAuthenticate
    static const BSONObj userReplQuery = fromjson("{\"user\":\"repl\"}");

    void SyncSourceFeedback::associateMember(const BSONObj& id, Member* member) {
        invariant(member);
        const OID rid = id["_id"].OID();
        boost::unique_lock<boost::mutex> lock(_mtx);
        _handshakeNeeded = true;
        _members[rid] = member;
        _cond.notify_all();
    }

    bool SyncSourceFeedback::replAuthenticate() {
        if (!getGlobalAuthorizationManager()->isAuthEnabled())
            return true;

        if (!isInternalAuthSet())
            return false;
        return authenticateInternalUser(_connection.get());
    }

    void SyncSourceFeedback::ensureMe() {
        string myname = getHostName();
        {
            Client::WriteContext ctx("local");
            DurTransaction txn;
            // local.me is an identifier for a server for getLastError w:2+
            if (!Helpers::getSingleton("local.me", _me) ||
                !_me.hasField("host") ||
                _me["host"].String() != myname) {

                // clean out local.me
                Helpers::emptyCollection(&txn, "local.me");

                // repopulate
                BSONObjBuilder b;
                b.appendOID("_id", 0, true);
                b.append("host", myname);
                _me = b.obj();
                Helpers::putSingleton(&txn, "local.me", _me);
            }
            // _me is used outside of a read lock, so we must copy it out of the mmap
            _me = _me.getOwned();
        }
    }

    bool SyncSourceFeedback::replHandshake() {
        // construct a vector of handshake obj for us as well as all chained members
        std::vector<BSONObj> handshakeObjs;
        {
            boost::unique_lock<boost::mutex> lock(_mtx);
            // handshake obj for us
            BSONObjBuilder cmd;
            cmd.append("replSetUpdatePosition", 1);
            BSONObjBuilder sub (cmd.subobjStart("handshake"));
            sub.appendAs(_me["_id"], "handshake");
            sub.append("member", theReplSet->selfId());
            sub.append("config", theReplSet->myConfig().asBson());
            sub.doneFast();
            handshakeObjs.push_back(cmd.obj());

            // handshake objs for all chained members
            for (OIDMemberMap::iterator itr = _members.begin();
                 itr != _members.end(); ++itr) {
                BSONObjBuilder cmd;
                cmd.append("replSetUpdatePosition", 1);
                // outer handshake indicates this is a handshake command
                // inner is needed as part of the structure to be passed to gotHandshake
                BSONObjBuilder subCmd (cmd.subobjStart("handshake"));
                subCmd.append("handshake", itr->first);
                subCmd.append("member", itr->second->id());
                subCmd.append("config", itr->second->config().asBson());
                subCmd.doneFast();
                handshakeObjs.push_back(cmd.obj());
            }
        }

        LOG(1) << "handshaking upstream updater";
        for (std::vector<BSONObj>::iterator it = handshakeObjs.begin();
                it != handshakeObjs.end();
                ++it) {
            BSONObj res;
            try {
                if (!_connection->runCommand("admin", *it, res)) {
                    massert(17447, "upstream updater is not supported by the member from which we"
                            " are syncing, please update all nodes to 2.6 or later.",
                            res["errmsg"].str().find("no such cmd") == std::string::npos);
                    log() << "replSet error while handshaking the upstream updater: "
                        << res["errmsg"].valuestrsafe();

                    _resetConnection();
                    return false;
                }
            }
            catch (const DBException& e) {
                log() << "SyncSourceFeedback error sending handshake: " << e.what() << endl;
                _resetConnection();
                return false;
            }
        }
        return true;
    }

    bool SyncSourceFeedback::_connect(const std::string& hostName) {
        if (hasConnection()) {
            return true;
        }
        log() << "replset setting syncSourceFeedback to " << hostName << rsLog;
        _connection.reset(new DBClientConnection(false, 0, OplogReader::tcp_timeout));
        string errmsg;
        if (!_connection->connect(hostName.c_str(), errmsg) ||
            (getGlobalAuthorizationManager()->isAuthEnabled() && !replAuthenticate())) {
            _resetConnection();
            log() << "repl: " << errmsg << endl;
            return false;
        }

        replHandshake();
        return hasConnection();
    }

    void SyncSourceFeedback::forwardSlaveHandshake() {
        boost::unique_lock<boost::mutex> lock(_mtx);
        _handshakeNeeded = true;
        _cond.notify_all();
    }

    void SyncSourceFeedback::updateMap(const mongo::OID& rid, const OpTime& ot) {
        boost::unique_lock<boost::mutex> lock(_mtx);
        // only update if ot is newer than what we have already
        if (ot > _slaveMap[rid]) {
            _slaveMap[rid] = ot;
            _positionChanged = true;
            LOG(2) << "now last is " << _slaveMap[rid].toString() << endl;
            _cond.notify_all();
        }
    }

    bool SyncSourceFeedback::updateUpstream() {
        if (theReplSet->isPrimary()) {
            // primary has no one to update to
            return true;
        }
        BSONObjBuilder cmd;
        cmd.append("replSetUpdatePosition", 1);
        // create an array containing objects each member connected to us and for ourself
        BSONArrayBuilder array (cmd.subarrayStart("optimes"));
        OID myID = _me["_id"].OID();
        {
            boost::unique_lock<boost::mutex> lock(_mtx);
            for (map<mongo::OID, OpTime>::const_iterator itr = _slaveMap.begin();
                    itr != _slaveMap.end(); ++itr) {
                BSONObjBuilder entry(array.subobjStart());
                entry.append("_id", itr->first);
                entry.append("optime", itr->second);
                if (itr->first == myID) {
                    entry.append("config", theReplSet->myConfig().asBson());
                }
                else {
                    entry.append("config", _members[itr->first]->config().asBson());
                }
                entry.doneFast();
            }
        }
        array.done();
        BSONObj res;

        bool ok;
        try {
            ok = _connection->runCommand("admin", cmd.obj(), res);
        }
        catch (const DBException& e) {
            log() << "SyncSourceFeedback error sending update: " << e.what() << endl;
            _resetConnection();
            return false;
        }
        if (!ok) {
            log() << "SyncSourceFeedback error sending update, response: " << res.toString() <<endl;
            _resetConnection();
            return false;
        }
        return true;
    }

    void SyncSourceFeedback::run() {
        Client::initThread("SyncSourceFeedbackThread");
        bool sleepNeeded = false;
        bool positionChanged = false;
        bool handshakeNeeded = false;
        while (!inShutdown()) {
            if (!theReplSet) {
                sleepsecs(5);
                continue;
            }
            if (sleepNeeded) {
                sleepmillis(500);
                sleepNeeded = false;
            }
            {
                boost::unique_lock<boost::mutex> lock(_mtx);
                while (!_positionChanged && !_handshakeNeeded) {
                    _cond.wait(lock);
                }
                positionChanged = _positionChanged;
                handshakeNeeded = _handshakeNeeded;
                _positionChanged = false;
                _handshakeNeeded = false;
            }

            MemberState state = theReplSet->state();
            if (state.primary() || state.fatal() || state.startup()) {
                continue;
            }
            const Member* target = replset::BackgroundSync::get()->getSyncTarget();
            if (_syncTarget != target) {
                _resetConnection();
                _syncTarget = target;
            }
            if (!hasConnection()) {
                // fix connection if need be
                if (!target) {
                    sleepNeeded = true;
                    continue;
                }
                if (!_connect(target->fullName())) {
                    sleepNeeded = true;
                    continue;
                }
            }
            if (handshakeNeeded) {
                if (!replHandshake()) {
                    boost::unique_lock<boost::mutex> lock(_mtx);
                    _handshakeNeeded = true;
                    continue;
                }
            }
            if (positionChanged) {
                if (!updateUpstream()) {
                    boost::unique_lock<boost::mutex> lock(_mtx);
                    _positionChanged = true;
                }
            }
        }
        cc().shutdown();
    }
}
