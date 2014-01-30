// @file s/client_info.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/client/connpool.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_session_external_state_s.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/s/write_ops/batch_downconvert.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/chunk.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/writeback_listener.h"
#include "mongo/server.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    ClientInfo::ClientInfo(AbstractMessagingPort* messagingPort) : ClientBasic(messagingPort) {
        _cur = &_a;
        _prev = &_b;
        _autoSplitOk = true;
        if (messagingPort) {
            _remote = messagingPort->remote();
        }
    }

    ClientInfo::~ClientInfo() {
    }

    void ClientInfo::addShardHost( const string& shardHost ) {
        _cur->shardHostsWritten.insert( shardHost );
        _sinceLastGetError.insert( shardHost );
    }

    void ClientInfo::addHostOpTime(ConnectionString connStr, HostOpTime stat) {
        _cur->hostOpTimes[connStr] = stat;
    }

    void ClientInfo::addHostOpTimes( const HostOpTimeMap& hostOpTimes ) {
        for ( HostOpTimeMap::const_iterator it = hostOpTimes.begin();
            it != hostOpTimes.end(); ++it ) {
            addHostOpTime(it->first, it->second);
        }
    }

    void ClientInfo::newPeerRequest( const HostAndPort& peer ) {
        if ( ! _remote.hasPort() )
            _remote = peer;
        else if ( _remote != peer ) {
            stringstream ss;
            ss << "remotes don't match old [" << _remote.toString() << "] new [" << peer.toString() << "]";
            throw UserException( 13134 , ss.str() );
        }

        newRequest();
    }

    void ClientInfo::newRequest() {
        _lastAccess = (int) time(0);

        RequestInfo* temp = _cur;
        _cur = _prev;
        _prev = temp;
        _cur->clear();
    }

    ClientInfo* ClientInfo::create(AbstractMessagingPort* messagingPort) {
        ClientInfo * info = _tlInfo.get();
        massert(16472, "A ClientInfo already exists for this thread", !info);
        info = new ClientInfo(messagingPort);
        info->setAuthorizationSession(new AuthorizationSession(
                new AuthzSessionExternalStateMongos(getGlobalAuthorizationManager())));
        _tlInfo.reset( info );
        info->newRequest();
        return info;
    }

    ClientInfo * ClientInfo::get(AbstractMessagingPort* messagingPort) {
        ClientInfo * info = _tlInfo.get();
        if (!info) {
            info = create(messagingPort);
        }
        massert(16483,
                mongoutils::str::stream() << "AbstractMessagingPort was provided to ClientInfo::get"
                        << " but differs from the one stored in the current ClientInfo object. "
                        << "Current ClientInfo messaging port "
                        << (info->port() ? "is not" : "is")
                        << " NULL",
                messagingPort == NULL || messagingPort == info->port());
        return info;
    }

    bool ClientInfo::exists() {
        return _tlInfo.get();
    }

    bool ClientBasic::hasCurrent() {
        return ClientInfo::exists();
    }

    ClientBasic* ClientBasic::getCurrent() {
        return ClientInfo::get();
    }


    void ClientInfo::disconnect() {
        // should be handled by TL cleanup
        _lastAccess = 0;
    }

    void ClientInfo::disableForCommand() {
        RequestInfo* temp = _cur;
        _cur = _prev;
        _prev = temp;
    }

    static TimerStats gleWtimeStats;
    static ServerStatusMetricField<TimerStats> displayGleLatency( "getLastError.wtime", &gleWtimeStats );

    boost::thread_specific_ptr<ClientInfo> ClientInfo::_tlInfo;


    // Look for $gleStats in a command response, and fill in ClientInfo with the data,
    // if found.
    // This data will be used by subsequent GLE calls, to ensure we look for the correct
    // write on the correct PRIMARY.
    void saveGLEStats(const BSONObj& result, const std::string& hostString) {
        if (!ClientInfo::exists()) {
            return;
        }
        if (result[kGLEStatsFieldName].type() != Object) {
            return;
        }
        std::string errmsg;
        ConnectionString shardConn = ConnectionString::parse(hostString, errmsg);

        BSONElement subobj = result[kGLEStatsFieldName];
        OpTime lastOpTime = subobj[kGLEStatsLastOpTimeFieldName]._opTime();
        OID electionId = subobj[kGLEStatsElectionIdFieldName].OID();
        ClientInfo* clientInfo = ClientInfo::get( NULL );
        LOG(4) << "saveGLEStats lastOpTime:" << lastOpTime 
               << " electionId:" << electionId;

        clientInfo->addHostOpTime(shardConn, HostOpTime(lastOpTime, electionId));
    }


} // namespace mongo
