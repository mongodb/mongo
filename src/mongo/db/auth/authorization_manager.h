/**
*    Copyright (C) 2013 10gen Inc.
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

#include <boost/scoped_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_name_hash.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    /**
     * Internal secret key info.
     */
    struct AuthInfo {
        User* user;
        BSONObj authParams;
    };
    extern AuthInfo internalSecurity; // set at startup and not changed after initialization.

    /**
     * Contains server/cluster-wide information about Authorization.
     */
    class AuthorizationManager {
        MONGO_DISALLOW_COPYING(AuthorizationManager);
    public:

        // The newly constructed AuthorizationManager takes ownership of "externalState"
        explicit AuthorizationManager(AuthzManagerExternalState* externalState);

        ~AuthorizationManager();

        static const std::string SERVER_RESOURCE_NAME;
        static const std::string CLUSTER_RESOURCE_NAME;
        static const std::string WILDCARD_RESOURCE_NAME;

        static const std::string USER_NAME_FIELD_NAME;
        static const std::string USER_SOURCE_FIELD_NAME;
        static const std::string PASSWORD_FIELD_NAME;

        // System roles for backwards compatibility with 2.2 and prior
        static const std::string SYSTEM_ROLE_V0_READ;
        static const std::string SYSTEM_ROLE_V0_READ_WRITE;
        static const std::string SYSTEM_ROLE_V0_ADMIN_READ;
        static const std::string SYSTEM_ROLE_V0_ADMIN_READ_WRITE;

        // TODO: Make the following functions no longer static.

        /**
         * Sets whether or not we allow old style (pre v2.4) privilege documents for this whole
         * server.
         */
        static void setSupportOldStylePrivilegeDocuments(bool enabled);

        /**
         * Returns true if we allow old style privilege privilege documents for this whole server.
         */
        static bool getSupportOldStylePrivilegeDocuments();

        /**
         * Sets whether or not access control enforcement is enabled for this whole server.
         */
        static void setAuthEnabled(bool enabled);

        /**
         * Returns true if access control is enabled on this server.
         */
        static bool isAuthEnabled();

        AuthzManagerExternalState* getExternalState() const;

        // Gets the privilege information document for "userName".
        //
        // On success, returns Status::OK() and stores a shared-ownership copy of the document into
        // "result".
        Status getPrivilegeDocument(const UserName& userName, BSONObj* result) const;

        // Returns true if there exists at least one privilege document in the system.
        bool hasAnyPrivilegeDocuments() const;

        // Creates the given user object in the given database.
        Status insertPrivilegeDocument(const std::string& dbname, const BSONObj& userObj) const;

        // Updates the given user object with the given update modifier.
        Status updatePrivilegeDocument(const UserName& user, const BSONObj& updateObj) const;

        // Removes users for the given database matching the given query.
        Status removePrivilegeDocuments(const std::string& dbname, const BSONObj& query) const;

        // Checks to see if "doc" is a valid privilege document, assuming it is stored in the
        // "system.users" collection of database "dbname".
        //
        // Returns Status::OK() if the document is good, or Status(ErrorCodes::BadValue), otherwise.
        Status checkValidPrivilegeDocument(const StringData& dbname, const BSONObj& doc);

        // Given a database name and a readOnly flag return an ActionSet describing all the actions
        // that an old-style user with those attributes should be given.
        ActionSet getActionsForOldStyleUser(const std::string& dbname, bool readOnly) const;

        // Returns an ActionSet of all actions that can be be granted to users.  This does not
        // include internal-only actions.
        ActionSet getAllUserActions() const;

        /**
         *  Returns the User object for the given userName in the out parameter "acquiredUser".
         *  If the user cache already has a user object for this user, it increments the refcount
         *  on that object and gives out a pointer to it.  If no user object for this user name
         *  exists yet in the cache, reads the user's privilege document from disk, builds up
         *  a User object, sets the refcount to 1, and gives that out.  The returned user may
         *  be invalid by the time the caller gets access to it.
         *  The AuthorizationManager retains ownership of the returned User object.
         *  On non-OK Status return values, acquiredUser will not be modified.
         */
        Status acquireUser(const UserName& userName, User** acquiredUser);

        /**
         * Decrements the refcount of the given User object.  If the refcount has gone to zero,
         * deletes the User.  Caller must stop using its pointer to "user" after calling this.
         */
        void releaseUser(User* user);

        /**
         * Marks the given user as invalid and removes it from the user cache.
         */
        void invalidateUser(User* user);

        /**
         * Inserts the given user directly into the _userCache.  Used to add the internalSecurity
         * user into the cache at process startup.
         */
        void addInternalUser(User* user);

        /**
         * Initializes the user cache with User objects for every v0 and v1 user document in the
         * system, by reading the system.users collection of every database.  If this function
         * returns a non-ok Status, the _userCache should be considered corrupt and must be
         * discarded.  This function should be called once at startup (only if the system hasn't yet
         * been upgraded to V2 user data format) and never again after that.
         * TODO(spencer): This function will temporarily be called every time user data is changed
         * as part of the transition period to the new User data structures.  This should be changed
         * once we have all the code necessary to upgrade to the V2 user data format, as at that
         * point we'll only be able to user V1 user data as read-only.
         */
        Status initializeAllV1UserData();

        /**
         * Parses privDoc and fully initializes the user object (credentials, roles, and privileges)
         * with the information extracted from the privilege document.
         * This should never be called from outside the AuthorizationManager - the only reason it's
         * public instead of private is so it can be unit tested.
         */
        Status _initializeUserFromPrivilegeDocument(User* user,
                                                    const BSONObj& privDoc) const;

    private:

        /**
         * Invalidates all User objects in the cache and removes them from the cache.
         * Should only be called when already holding _lock.
         * TODO(spencer): This only exists because we're currently calling initializeAllV1UserData
         * every time user data is changed.  Once we only call that once at startup, this function
         * should be removed.
         */
        void _invalidateUserCache_inlock();


        static bool _doesSupportOldStylePrivileges;

        // True if access control enforcement is enabled on this node (ie it was started with
        // --auth or --keyFile).
        // This is a config setting, set at startup and not changing after initialization.
        static bool _authEnabled;

        // Integer that represents what format version the privilege documents in the system are.
        // The current version is 2.  When upgrading to v2.6 or later from v2.4 or prior, the
        // version is 1.  After running the upgrade process to upgrade to the new privilege document
        // format, the version will be 2.
        int _version;

        scoped_ptr<AuthzManagerExternalState> _externalState;

        /**
         * Caches User objects with information about user privileges, to avoid the need to
         * go to disk to read user privilege documents whenever possible.  Every User object
         * has a reference count - the AuthorizationManager must not delete a User object in the
         * cache unless its reference count is zero.
         */
        unordered_map<UserName, User*> _userCache;

        /**
         * Protects _userCache.
         */
        boost::mutex _lock;
    };

} // namespace mongo
