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

#include <string>

#include "mongo/db/security_common.h"
#include "mongo/client/authentication_table.h"
#include "mongo/client/authlevel.h"
#include "mongo/util/concurrency/spin_lock.h"

// this is used by both mongos and mongod

namespace mongo {

    /** An AuthenticationInfo object is present within every mongo::Client object */
    class AuthenticationInfo : boost::noncopyable {
        bool _isLocalHost;
        bool _isLocalHostAndLocalHostIsAuthorizedForAll;
    public:
        void startRequest(); // need to call at the beginning of each request
        void setIsALocalHostConnectionWithSpecialAuthPowers(); // called, if localhost, when conneciton established.
        AuthenticationInfo() {
            _isLocalHost = false; 
            _isLocalHostAndLocalHostIsAuthorizedForAll = false;
            _usingTempAuth = false;
        }
        ~AuthenticationInfo() {}
        bool isLocalHost() const { return _isLocalHost; } // why are you calling this? makes no sense to be externalized
        bool isSpecialLocalhostAdmin() const;

        // -- modifiers ----
        
        void logout(const std::string& dbname ) {
            scoped_spinlock lk(_lock);
            _authTable.removeAuth( dbname );
        }
        void authorize(const std::string& dbname , const std::string& user ) {
            scoped_spinlock lk(_lock);
            _authTable.addAuth( dbname, user, Auth::WRITE );
        }
        void authorizeReadOnly(const std::string& dbname , const std::string& user ) {
            scoped_spinlock lk(_lock);
            _authTable.addAuth( dbname, user, Auth::READ );
        }
        
        // -- accessors ---

        bool isAuthorized(const std::string& dbname) const { 
            return _isAuthorized( dbname, Auth::WRITE ); 
        }
        
        bool isAuthorizedReads(const std::string& dbname) const { 
            return _isAuthorized( dbname, Auth::READ ); 
        }
        
        /**
         * @param lockType - this is from dbmutex 1 is write, 0 is read
         */
        bool isAuthorizedForLock(const std::string& dbname, int lockType ) const { 
            return _isAuthorized( dbname , lockType > 0 ? Auth::WRITE : Auth::READ ); 
        }

        std::string getUser( const std::string& dbname ) const;

        void print() const;

        BSONObj toBSON() const;

        void setTemporaryAuthorization( BSONObj& obj );
        void clearTemporaryAuthorization();
        bool hasTemporaryAuthorization();

        // Returns true if this AuthenticationInfo has been auth'd to use the internal user
        bool usingInternalUser();

        const AuthenticationTable getAuthTable() const;

        // When TemporaryAuthReleaser goes out of scope it clears the temporary authentication set
        // in its AuthenticationInfo object, unless that AuthenticationInfo already had temporary
        // auth set at the time that the TemporaryAuthReleaser was initialized.
        class TemporaryAuthReleaser : boost::noncopyable {
        public:
            TemporaryAuthReleaser ( AuthenticationInfo* ai );
            ~TemporaryAuthReleaser();
        private:
            AuthenticationInfo* _ai;
            bool _hadTempAuthFromStart;
        };

    private:
        void _checkLocalHostSpecialAdmin();

        /** takes a lock */
        bool _isAuthorized(const std::string& dbname, Auth::Level level) const;

        // Must be in _lock
        bool _isAuthorizedSingle_inlock(const std::string& dbname, Auth::Level level) const;
        
        /** cannot call this locked */
        bool _isAuthorizedSpecialChecks( const std::string& dbname ) const ;

        // Must be in _lock
        void _addTempAuth_inlock( const std::string& dbname, const std::string& user,
                                  Auth::Level level);

    private:
        // while most access to _authTable is from our thread (the TLS thread), currentOp()
        // inspects it too thus we need this.
        // This protects _authTable, _tempAuthTable, and _usingTempAuth.
        mutable SpinLock _lock;

        // todo: caching should not last forever
        AuthenticationTable _authTable;
        // when _usingTempAuth is true, this is used for all auth checks instead of _authTable
        AuthenticationTable _tempAuthTable;

        bool _usingTempAuth;
        static bool _warned;
    };

} // namespace mongo
