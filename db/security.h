// security.h

/**
*    Copyright (C) 2009 10gen Inc.
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

#include <boost/thread/tss.hpp>
#undef assert
#define assert xassert

#include "db.h"
#include "dbhelpers.h"
#include "nonce.h"

namespace mongo {

    // --noauth cmd line option
    extern bool noauth;
    extern bool authWriteOnly;

    /* for a particular db */
    struct Auth {
        Auth() { level = 0; }
        int level;
    };

    class AuthenticationInfo : boost::noncopyable {
        map<string, Auth> m; // dbname -> auth
		static int warned;
    public:
		bool isLocalHost;
        AuthenticationInfo() { isLocalHost = false; }
        virtual ~AuthenticationInfo() {
        }
        void logout(const char *dbname) { 
			assertInWriteLock();
			m.erase(dbname); 
		}
        void authorize(const char *dbname) { 
			assertInWriteLock();
            m[dbname].level = 2;
        }
        void authorizeReadOnly(const char *dbname) {
			assertInWriteLock();
            m[dbname].level = 1;            
        }
        bool isAuthorized(const char *dbname) { return _isAuthorized( dbname, 2 ); }
        bool isReadOnlyAuthorized(const char *dbname) { return _isAuthorized( dbname, 1 ); }
    protected:
        virtual bool _isAuthorized(const char *dbname, int level) { 
            if( m[dbname].level >= level ) return true;
			if( noauth ) return true;
            if( authWriteOnly && ( 1 >= level ) ) return true;
			if( m["admin"].level >= level ) return true;
			if( m["local"].level >= level ) return true;
            if( cc().isGod() ) return true;
			if( isLocalHost ) { 
                readlock l(""); 
                Client::Context c("admin.system.users");
				BSONObj result;
				if( Helpers::getSingleton("admin.system.users", result) )
					return false;
				if( warned == 0 ) {
					warned++;
					log() << "warning: no users configured in admin.system.users, allowing localhost access" << endl;
				}
				return true;
			}
			return false;
        }
    };

} // namespace mongo
