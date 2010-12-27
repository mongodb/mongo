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

#include "nonce.h"
#include "concurrency.h"
#include "security_key.h"

namespace mongo {

    /* for a particular db */
    struct Auth {
        Auth() { level = 0; }
        int level;
    };

    class AuthenticationInfo : boost::noncopyable {
        mongo::mutex _lock;
        map<string, Auth> m; // dbname -> auth
		static int warned;
    public:
		bool isLocalHost;
        AuthenticationInfo() : _lock("AuthenticationInfo") { isLocalHost = false; }
        ~AuthenticationInfo() {
        }
        void logout(const string& dbname ) { 
            scoped_lock lk(_lock);
			m.erase(dbname); 
		}
        void authorize(const string& dbname ) { 
            scoped_lock lk(_lock);
            m[dbname].level = 2;
        }
        void authorizeReadOnly(const string& dbname) {
            scoped_lock lk(_lock);
            m[dbname].level = 1;            
        }
        bool isAuthorized(const string& dbname) { return _isAuthorized( dbname, 2 ); }
        bool isAuthorizedReads(const string& dbname) { return _isAuthorized( dbname, 1 ); }
        bool isAuthorizedForLock(const string& dbname, int lockType ) { return _isAuthorized( dbname , lockType > 0 ? 2 : 1 ); }
        
        void print();

    protected:
        bool _isAuthorized(const string& dbname, int level) { 
            if( m[dbname].level >= level ) return true;
			if( noauth ) return true;
			if( m["admin"].level >= level ) return true;
			if( m["local"].level >= level ) return true;
            return _isAuthorizedSpecialChecks( dbname );
        }

        bool _isAuthorizedSpecialChecks( const string& dbname );
    };

} // namespace mongo
