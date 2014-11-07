// @connections.cpp

/*
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/scoped_conn.h"

#include "mongo/db/repl/rslog.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

    static const int DEFAULT_HEARTBEAT_TIMEOUT_SECS = 10;

    // This is a bitmask with the first bit set. It's used to mark connections that should be kept
    // open during stepdowns
    const unsigned ScopedConn::keepOpen = 1;
    ScopedConn::M& ScopedConn::_map = *(new ScopedConn::M());
    mutex ScopedConn::mapMutex("ScopedConn::mapMutex");

    ScopedConn::ConnectionInfo::ConnectionInfo() : lock("ConnectionInfo"),
                    cc(new DBClientConnection(/*reconnect*/ true,
                                              /*replicaSet*/ 0,
                                              /*timeout*/ DEFAULT_HEARTBEAT_TIMEOUT_SECS)),
                    connected(false) {
                    cc->_logLevel = logger::LogSeverity::Debug(2);
                }

    // we should already be locked...
    bool ScopedConn::connect() {
        std::string err;
        if (!connInfo->cc->connect(HostAndPort(_hostport), err)) {
            log() << "couldn't connect to " << _hostport << ": " << err << rsLog;
            return false;
        }
        connInfo->connected = true;
        connInfo->tagPort();

        // if we cannot authenticate against a member, then either its key file
        // or our key file has to change.  if our key file has to change, we'll
        // be rebooting. if their file has to change, they'll be rebooted so the
        // connection created above will go dead, reconnect, and reauth.
        if (getGlobalAuthorizationManager()->isAuthEnabled()) {
            return authenticateInternalUser(connInfo->cc.get());
        }

        return true;
    }

} // namespace repl
} // namespace mongo
