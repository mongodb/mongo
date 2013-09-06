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
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/rs.h"  // theReplSet

namespace mongo {

    // used in replAuthenticate
    static const BSONObj userReplQuery = fromjson("{\"user\":\"repl\"}");

    void SyncSourceFeedback::associateMember(const BSONObj& id, const int memberId) {
        const OID rid = id["_id"].OID();
        boost::unique_lock<boost::mutex> lock(_mtx);
        _handshakeNeeded = true;
        _members[rid] = theReplSet->getMutableMember(memberId);
        _cond.notify_all();
    }

    bool SyncSourceFeedback::replAuthenticate(bool skipAuthCheck) {

        if (!AuthorizationManager::isAuthEnabled()) {
            return true;
        }
        if (!skipAuthCheck && !cc().getAuthorizationSession()->hasInternalAuthorization()) {
            log() << "replauthenticate: requires internal authorization, failing" << endl;
            return false;
        }

        if (isInternalAuthSet()) { 
            return authenticateInternalUser(_connection.get()); 
        }

        BSONObj user;
        {
            Client::ReadContext ctxt("local.");
            if(!Helpers::findOne("local.system.users", userReplQuery, user) ||
                    // try the first user in local
                    !Helpers::getSingleton("local.system.users", user)) {
                log() << "replauthenticate: no user in local.system.users to use"
                        << "for authentication" << endl;
                return false;
            }
        }
        std::string u = user.getStringField("user");
        std::string p = user.getStringField("pwd");
        massert(16889, "bad user object? [1]", !u.empty());
        massert(16887, "bad user object? [2]", !p.empty());

        std::string err;

        if( !_connection->auth("local", u.c_str(), p.c_str(), err, false) ) {
            log() << "replauthenticate: can't authenticate to master server, user:" << u << endl;
            return false;
        }

        return true;
    }

    void SyncSourceFeedback::ensureMe() {
        string myname = getHostName();
        {
            Client::WriteContext ctx("local");
            // local.me is an identifier for a server for getLastError w:2+
            if (!Helpers::getSingleton("local.me", _me) ||
                !_me.hasField("host") ||
                _me["host"].String() != myname) {

                // clean out local.me
                Helpers::emptyCollection("local.me");

                // repopulate
                BSONObjBuilder b;
                b.appendOID("_id", 0, true);
                b.append("host", myname);
                _me = b.obj();
                Helpers::putSingleton("local.me", _me);
            }
        }
    }

    bool SyncSourceFeedback::replHandshake() {
        // handshake for us
        BSONObjBuilder cmd;
        cmd.append("replSetUpdatePosition", 1);
        BSONObjBuilder sub (cmd.subobjStart("handshake"));
        sub.appendAs(_me["_id"], "handshake");
        sub.append("member", theReplSet->selfId());
        sub.append("config", theReplSet->myConfig().asBson());
        sub.doneFast();

        LOG(1) << "detecting upstream updater";
        BSONObj res;
        try {
            if (!_connection->runCommand("admin", cmd.obj(), res)) {
                if (res["errmsg"].str().find("no such cmd") != std::string::npos) {
                    log() << "upstream updater is unsupported on this version";
                    _supportsUpdater = false;
                }
                resetConnection();
                return false;
            }
            else {
                LOG(1) << "upstream updater is supported";
                _supportsUpdater = true;
            }
        }
        catch (const DBException& e) {
            log() << "SyncSourceFeedback error sending handshake: " << e.what() << endl;
            resetConnection();
            return false;
        }

        // handshakes for those connected to us
        {
            for (OIDMemberMap::iterator itr = _members.begin();
                 itr != _members.end(); ++itr) {
                BSONObjBuilder slaveCmd;
                slaveCmd.append("replSetUpdatePosition", 1);
                // outer handshake indicates this is a handshake command
                // inner is needed as part of the structure to be passed to gotHandshake
                BSONObjBuilder slaveSub (slaveCmd.subobjStart("handshake"));
                slaveSub.append("handshake", itr->first);
                slaveSub.append("member", itr->second->id());
                slaveSub.append("config", itr->second->config().asBson());
                slaveSub.doneFast();
                BSONObj slaveRes;
                try {
                    if (!_connection->runCommand("admin", slaveCmd.obj(), slaveRes)) {
                        resetConnection();
                        return false;
                    }
                }
                catch (const DBException& e) {
                    log() << "SyncSourceFeedback error sending chained handshakes: "
                          << e.what() << endl;
                    resetConnection();
                    return false;
                }
            }
        }
        return true;
    }

    bool SyncSourceFeedback::_connect(const std::string& hostName) {
        if (hasConnection()) {
            return true;
        }
        _connection.reset(new DBClientConnection(false, 0, OplogReader::tcp_timeout));
        string errmsg;
        if (!_connection->connect(hostName.c_str(), errmsg) ||
                (AuthorizationManager::isAuthEnabled() && !replAuthenticate(true))) {
            resetConnection();
            log() << "repl: " << errmsg << endl;
            return false;
        }

        if (!replHandshake()) {
            if (!supportsUpdater()) {
                return connectOplogReader(hostName);
            }
            return false;
        }
        return true;
    }

    bool SyncSourceFeedback::connect(const Member* target) {
        boost::unique_lock<boost::mutex> lock(_connmtx);
        resetConnection();
        resetOplogReaderConnection();
        _syncTarget = target;
        if (_connect(target->fullName())) {
            if (!supportsUpdater()) {
                return true;
            }
        }
        return false;
    }

    void SyncSourceFeedback::forwardSlaveHandshake() {
        boost::unique_lock<boost::mutex> lock(_mtx);
        _handshakeNeeded = true;
        _cond.notify_all();
    }

    void SyncSourceFeedback::percolate(const mongo::OID& rid, const OpTime& ot) {
        // Update our own record of where this node is, and then register an upstream
        // message about this.
        // Note that we must keep the map up to date even if we are not actively reporting
        // upstream via the new command, since our sync source might later change to a node
        // that does support the command.
        updateMap(rid, ot);
        if (!supportsUpdater()) {
            // this is only necessary if our sync source does not support
            // the new syncSourceFeedback command
            theReplSet->ghost->send(boost::bind(&GhostSync::percolate,
                                                theReplSet->ghost,
                                                rid,
                                                ot));
        }
    }



    void SyncSourceFeedback::updateMap(const mongo::OID& rid, const OpTime& ot) {
        boost::unique_lock<boost::mutex> lock(_mtx);
        LOG(1) << "replSet last: " << _slaveMap[rid].toString() << " to " << ot.toString() << endl;
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
            resetConnection();
            return false;
        }
        if (!ok) {
            log() << "SyncSourceFeedback error sending update, response: " << res.toString() <<endl;
            resetConnection();
            return false;
        }
        return true;
    }

    void SyncSourceFeedback::run() {
        Client::initThread("SyncSourceFeedbackThread");
        bool sleepNeeded = false;
        while (true) {
            if (sleepNeeded) {
                sleepmillis(500);
                sleepNeeded = false;
            }
            {
                boost::unique_lock<boost::mutex> lock(_mtx);
                while (!_positionChanged && !_handshakeNeeded) {
                    _cond.wait(lock);
                }
                if (theReplSet->isPrimary()) {
                    _positionChanged = false;
                    _handshakeNeeded = false;
                    continue;
                }
                const Member* target = replset::BackgroundSync::get()->getSyncTarget();
                boost::unique_lock<boost::mutex> connlock(_connmtx);
                if (_syncTarget != target) {
                    resetConnection();
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
                    else if (!supportsUpdater()) {
                        _handshakeNeeded = false;
                        _positionChanged = false;
                        continue;
                    }
                }
                if (_handshakeNeeded) {
                    if (!replHandshake()) {
                        _handshakeNeeded = true;
                        continue;
                    }
                    else {
                        _handshakeNeeded = false;
                    }
                }
                if (_positionChanged) {
                    if (!updateUpstream()) {
                        _positionChanged = true;
                        continue;
                    }
                    else {
                        _positionChanged = false;
                    }
                }
            }
        }
    }
}
