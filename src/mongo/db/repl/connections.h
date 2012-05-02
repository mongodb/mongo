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
#include "../security_common.h"

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
        /** throws assertions if connect failure etc. */
        ScopedConn(string hostport);
        ~ScopedConn() {
            // conLock releases...
        }
        void reconnect() {
            x->cc.reset(new DBClientConnection(true, 0, 10));
            x->cc->_logLevel = 2;
            x->connected = false;
            connect();
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
        struct X {
            mongo::mutex z;
            scoped_ptr<DBClientConnection> cc;
            bool connected;
            X() : z("X"), cc(new DBClientConnection(/*reconnect*/ true, 0, /*timeout*/ 10.0)), connected(false) {
                cc->_logLevel = 2;
            }
        } *x;
        typedef map<string,ScopedConn::X*> M;
        static M& _map;
        scoped_ptr<DBClientConnection>& conn() { return x->cc; }
        const string _hostport;

        // we should already be locked...
        bool connect() {
          string err;
          if (!x->cc->connect(_hostport, err)) {
            log() << "couldn't connect to " << _hostport << ": " << err << rsLog;
            return false;
          }
          x->connected = true;

          // if we cannot authenticate against a member, then either its key file
          // or our key file has to change.  if our key file has to change, we'll
          // be rebooting. if their file has to change, they'll be rebooted so the
          // connection created above will go dead, reconnect, and reauth.
          if (!noauth && !x->cc->auth("local", internalSecurity.user, internalSecurity.pwd, err, false)) {
            log() << "could not authenticate against " << _hostport << ", " << err << rsLog;
            return false;
          }

          return true;
        }
    };

    inline ScopedConn::ScopedConn(string hostport) : _hostport(hostport) {
        bool first = false;
        {
            scoped_lock lk(mapMutex);
            x = _map[_hostport];
            if( x == 0 ) {
                x = _map[_hostport] = new X();
                first = true;
                connLock.reset( new scoped_lock(x->z) );
            }
        }

        // already locked connLock above
        if (first) {
            connect();
            return;
        }

        connLock.reset( new scoped_lock(x->z) );
        if (x->connected) {
            return;
        }

        // Keep trying to connect if we're not yet connected
        connect();
    }

}
