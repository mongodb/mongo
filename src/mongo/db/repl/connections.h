// @file

/*
 *    Copyright (C) 2010 10gen Inc.
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
 */

#pragma once

#include <map>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/repl/rs.h" // extern Tee* rslog

namespace mongo {

    /** here we keep a single connection (with reconnect) for a set of hosts,
        one each, and allow one user at a time per host.  if in use already for that
        host, we block.  so this is an easy way to keep a 1-deep pool of connections
        that many threads can share.

        thread-safe.

        Example:
        {
            ScopedConn c("foo.acme.com:9999");
            c->runCommand(...);
        }

        throws exception on connect error (but fine to try again later with a new
        scopedconn object for same host).
    */
    class ScopedConn {
    public:
        // A flag to keep ScopedConns open when all other sockets are disconnected
        static const unsigned keepOpen;

        /** throws assertions if connect failure etc. */
        ScopedConn(const std::string& hostport);
        ~ScopedConn() {
            // conLock releases...
        }
        void reconnect() {
            connInfo->cc.reset(new DBClientConnection(true, 0, connInfo->getTimeout()));
            connInfo->cc->_logLevel = 2;
            connInfo->connected = false;
            connect();
        }

        void setTimeout(time_t timeout) {
            connInfo->setTimeout(timeout);
        }

        /* If we were to run a query and not exhaust the cursor, future use of the connection would be problematic.
           So here what we do is wrapper known safe methods and not allow cursor-style queries at all.  This makes
           ScopedConn limited in functionality but very safe.  More non-cursor wrappers can be added here if needed.
           */
        bool runCommand(const string &dbname, const BSONObj& cmd, BSONObj &info, int options=0) {
            return conn()->runCommand(dbname, cmd, info, options);
        }
        unsigned long long count(const string &ns) {
            return conn()->count(ns);
        }
        BSONObj findOne(const string &ns, const Query& q, const BSONObj *fieldsToReturn = 0, int queryOptions = 0) {
            return conn()->findOne(ns, q, fieldsToReturn, queryOptions);
        }

    private:
        auto_ptr<scoped_lock> connLock;
        static mongo::mutex mapMutex;
        struct ConnectionInfo {
            mongo::mutex lock;
            scoped_ptr<DBClientConnection> cc;
            bool connected;
            ConnectionInfo() : lock("ConnectionInfo"),
                cc(new DBClientConnection(/*reconnect*/ true,
                                          /*replicaSet*/ 0,
                                          /*timeout*/ ReplSetConfig::DEFAULT_HB_TIMEOUT)),
                connected(false) {
                cc->_logLevel = 2;
            }

            void tagPort() {
                MessagingPort& mp = cc->port();
                mp.tag |= ScopedConn::keepOpen;
            }

            void setTimeout(time_t timeout) {
                _timeout = timeout;
                cc->setSoTimeout(_timeout);
            }

            int getTimeout() {
                return _timeout;
            }

        private:
            int _timeout;
        } *connInfo;
        typedef map<string,ScopedConn::ConnectionInfo*> M;
        static M& _map;
        scoped_ptr<DBClientConnection>& conn() { return connInfo->cc; }
        const string _hostport;

        // we should already be locked...
        bool connect() {
          string err;
          if (!connInfo->cc->connect(_hostport, err)) {
            log() << "couldn't connect to " << _hostport << ": " << err << rsLog;
            return false;
          }
          connInfo->connected = true;
          connInfo->tagPort();

          // if we cannot authenticate against a member, then either its key file
          // or our key file has to change.  if our key file has to change, we'll
          // be rebooting. if their file has to change, they'll be rebooted so the
          // connection created above will go dead, reconnect, and reauth.
          if (AuthorizationManager::isAuthEnabled()) {
              if (!connInfo->cc->auth("local",
                                      internalSecurity.user,
                                      internalSecurity.pwd,
                                      err,
                                      false)) {
                  log() << "could not authenticate against " << _hostport << ", " << err << rsLog;
                  return false;
              }
          }

          return true;
        }
    };

    inline ScopedConn::ScopedConn(const std::string& hostport) : _hostport(hostport) {
        bool first = false;
        {
            scoped_lock lk(mapMutex);
            connInfo = _map[_hostport];
            if( connInfo == 0 ) {
                connInfo = _map[_hostport] = new ConnectionInfo();
                first = true;
                connLock.reset( new scoped_lock(connInfo->lock) );
            }
        }

        // already locked connLock above
        if (first) {
            connect();
            return;
        }

        connLock.reset( new scoped_lock(connInfo->lock) );
        if (connInfo->connected) {
            return;
        }

        // Keep trying to connect if we're not yet connected
        connect();
    }
}
