// security.cpp
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

// security.cpp

#include "pch.h"
#include "../db/security_common.h"
#include "../db/security.h"
#include "config.h"
#include "client.h"
#include "grid.h"

// this is the _mongos only_ implementation of security.h

namespace mongo {

    bool AuthenticationInfo::_warned;

    bool CmdAuthenticate::getUserObj(const string& dbname, const string& user, BSONObj& userObj, string& pwd) {
        if (user == internalSecurity.user) {
            uassert(15890, "key file must be used to log in with internal user", cmdLine.keyFile);
            pwd = internalSecurity.pwd;
        }
        else {
            string systemUsers = dbname + ".system.users";
            DBConfigPtr config = grid.getDBConfig( systemUsers );
            Shard s = config->getShard( systemUsers );

            static BSONObj userPattern = BSON("user" << 1);

            ScopedDbConnection conn( s, 30.0 );
            OCCASIONALLY conn->ensureIndex(systemUsers, userPattern, false, "user_1");
            {
                BSONObjBuilder b;
                b << "user" << user;
                BSONObj query = b.done();
                userObj = conn->findOne(systemUsers, query, 0, QueryOption_SlaveOk);
                if( userObj.isEmpty() ) {
                    log() << "auth: couldn't find user " << user << ", " << systemUsers << endl;
                    conn.done(); // return to pool
                    return false;
                }
            }

            pwd = userObj.getStringField("pwd");

            conn.done(); // return to pool
        }
        return true;
    }

    void AuthenticationInfo::setIsALocalHostConnectionWithSpecialAuthPowers() {
        verify(!_isLocalHost);
        _isLocalHost = true;
    }

    bool AuthenticationInfo::_isAuthorizedSpecialChecks( const string& dbname ) const {
        if ( !_isLocalHost ) {
            return false;
        }

        string adminNs = "admin.system.users";

        DBConfigPtr config = grid.getDBConfig( adminNs );
        Shard s = config->getShard( adminNs );

        ShardConnection conn( s, adminNs );
        BSONObj result = conn->findOne("admin.system.users", Query());
        if( result.isEmpty() ) {
            if( ! _warned ) {
                // you could get a few of these in a race, but that's ok
                _warned = true;
                log() << "note: no users configured in admin.system.users, allowing localhost access" << endl;
            }

            // Must return conn to pool
            // TODO: Check for errors during findOne(), or just let the conn die?
            conn.done();
            return true;
        }

        // Must return conn to pool
        conn.done();
        return false;
    }

    bool CmdLogout::run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        AuthenticationInfo *ai = ClientInfo::get()->getAuthenticationInfo();
        ai->logout(dbname);
        return true;
    }
}
