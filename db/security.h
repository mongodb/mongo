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

#include "nonce.h"
#include "concurrency.h"

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
        ~AuthenticationInfo() {
        }
        void logout(const string& dbname ) { 
			assertInWriteLock(); // TODO: can we get rid of this?  only 1 thread should be looking at an AuthenticationInfo
			m.erase(dbname); 
		}
        void authorize(const string& dbname ) { 
			assertInWriteLock();
            m[dbname].level = 2;
        }
        void authorizeReadOnly(const string& dbname) {
			assertInWriteLock();
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
            if( authWriteOnly && ( 1 >= level ) ) return true;
			if( m["admin"].level >= level ) return true;
			if( m["local"].level >= level ) return true;
            return _isAuthorizedSpecialChecks( dbname );
        }

        bool _isAuthorizedSpecialChecks( const string& dbname );
    };

} // namespace mongo
