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
#include "../util/concurrency/spin_lock.h"

namespace mongo {

    /* 
     * for a particular db
     * levels
     *     0 : none
     *     1 : read
     *     2 : write
     */
    struct Auth {
        Auth() { level = 0; }
        int level;
    };

    class AuthenticationInfo : boost::noncopyable {
    public:
        bool isLocalHost;
        
        AuthenticationInfo(){ isLocalHost = false; }
        ~AuthenticationInfo() {}

        void logout(const string& dbname ) {
            scoped_spinlock lk(_lock);
            _dbs.erase(dbname);
        }
        void authorize(const string& dbname ) {
            scoped_spinlock lk(_lock);
            _dbs[dbname].level = 2;
        }
        void authorizeReadOnly(const string& dbname) {
            scoped_spinlock lk(_lock);
            _dbs[dbname].level = 1;
        }

        bool isAuthorized(const string& dbname) const { return _isAuthorized( dbname, 2 ); }
        bool isAuthorizedReads(const string& dbname) const { return _isAuthorized( dbname, 1 ); }
        bool isAuthorizedForLock(const string& dbname, int lockType ) const { 
            return _isAuthorized( dbname , lockType > 0 ? 2 : 1 ); 
        }

        void print() const;

    protected:
        /** takes a lock */
        bool _isAuthorized(const string& dbname, int level) const;

        bool _isAuthorizedSingle_inlock(const string& dbname, int level) const;
        
        bool _isAuthorizedSpecialChecks_inlock( const string& dbname ) const ;

    private:
        mutable SpinLock _lock;

        typedef map<string,Auth> MA;
        MA _dbs; // dbname -> auth

        static bool _warned;
    };

} // namespace mongo
