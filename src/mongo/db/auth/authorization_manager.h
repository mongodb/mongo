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

#pragma once

#include <boost/scoped_ptr.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/user_name_hash.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/stdx/functional.h"

namespace mongo {

    class AuthzManagerExternalState;
    class UserDocumentParser;
    class OperationContext;

    /**
     * Internal secret key info.
     */
    struct AuthInfo {
        User* user;
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

        static const std::string USER_NAME_FIELD_NAME;
        static const std::string USER_DB_FIELD_NAME;
        static const std::string ROLE_NAME_FIELD_NAME;
        static const std::string ROLE_SOURCE_FIELD_NAME; // TODO: rename to ROLE_DB_FIELD_NAME
        static const std::string PASSWORD_FIELD_NAME;
        static const std::string V1_USER_NAME_FIELD_NAME;
        static const std::string V1_USER_SOURCE_FIELD_NAME;

        static const NamespaceString adminCommandNamespace;
        static const NamespaceString rolesCollectionNamespace;
        static const NamespaceString usersAltCollectionNamespace;
        static const NamespaceString usersBackupCollectionNamespace;
        static const NamespaceString usersCollectionNamespace;
        static const NamespaceString versionCollectionNamespace;
        static const NamespaceString defaultTempUsersCollectionNamespace; // for mongorestore
        static const NamespaceString defaultTempRolesCollectionNamespace; // for mongorestore

        /**
         * Query to match the auth schema version document in the versionCollectionNamespace.
         */
        static const BSONObj versionDocumentQuery;

        /**
         * Name of the field in the auth schema version document containing the current schema
         * version.
         */
        static const std::string schemaVersionFieldName;

        /**
         * Value used to represent that the schema version is not cached or invalid.
         */
        static const int schemaVersionInvalid = 0;

        /**
         * Auth schema version for MongoDB v2.4 and prior.
         */
        static const int schemaVersion24 = 1;

        /**
         * Auth schema version for MongoDB v2.6 during the upgrade process.  Same as
         * schemaVersion26Final, except that user documents are found in admin.new.users, and user
         * management commands are disabled.
         */
        static const int schemaVersion26Upgrade = 2;

        /**
         * Auth schema version for MongoDB 2.6.  Users are stored in admin.system.users,
         * roles in admin.system.roles.
         */
        static const int schemaVersion26Final = 3;

        // TODO: Make the following functions no longer static.

        /**
         * Takes a vector of privileges and fills the output param "resultArray" with a BSON array
         * representation of the privileges.
         */
        static Status getBSONForPrivileges(const PrivilegeVector& privileges,
                                           mutablebson::Element resultArray);

        /**
         * Takes a role name and a role graph and fills the output param "result" with a BSON
         * representation of the role object.
         * This function does no locking - it is up to the caller to synchronize access to the
         * role graph.
         * Note: The passed in RoleGraph can't be marked const because some of its accessors can
         * actually modify it internally (to set up built-in roles).
         */
        static Status getBSONForRole(/*const*/ RoleGraph* graph,
                                     const RoleName& roleName,
                                     mutablebson::Element result);


        /**
         * Sets whether or not access control enforcement is enabled for this manager.
         */
        void setAuthEnabled(bool enabled);

        /**
         * Returns true if access control is enabled for this manager .
         */
        bool isAuthEnabled() const;

        /**
         * Returns via the output parameter "version" the version number of the authorization
         * system.  Returns Status::OK() if it was able to successfully fetch the current
         * authorization version.  If it has problems fetching the most up to date version it
         * returns a non-OK status.  When returning a non-OK status, *version will be set to
         * schemaVersionInvalid (0).
         */
        Status getAuthorizationVersion(OperationContext* txn, int* version);

        /**
         * Returns the user cache generation identifier.
         */
        OID getCacheGeneration();

        // Returns true if there exists at least one privilege document in the system.
        bool hasAnyPrivilegeDocuments(OperationContext* txn) const;

        /**
         * Updates the auth schema version document to reflect that the system is upgraded to
         * schemaVersion26Final.
         *
         * Do not call if getAuthorizationVersion() reports a value other than schemaVersion26Final.
         */
        Status writeAuthSchemaVersionIfNeeded();

        /**
         * Creates the given user object in the given database.
         * 'writeConcern' contains the arguments to be passed to getLastError to block for
         * successful completion of the write.
         */
        Status insertPrivilegeDocument(const std::string& dbname,
                                       const BSONObj& userObj,
                                       const BSONObj& writeConcern) const;

        /**
         * Updates the given user object with the given update modifier.
         * 'writeConcern' contains the arguments to be passed to getLastError to block for
         * successful completion of the write.
         */
        Status updatePrivilegeDocument(const UserName& user,
                                       const BSONObj& updateObj,
                                       const BSONObj& writeConcern) const;

        /*
         * Removes users for the given database matching the given query.
         * Writes into *numRemoved the number of user documents that were modified.
         * 'writeConcern' contains the arguments to be passed to getLastError to block for
         * successful completion of the write.
         */
        Status removePrivilegeDocuments(const BSONObj& query,
                                        const BSONObj& writeConcern,
                                        int* numRemoved) const;

        /**
         * Creates the given role object in the given database.
         * 'writeConcern' contains the arguments to be passed to getLastError to block for
         * successful completion of the write.
         */
        Status insertRoleDocument(const BSONObj& roleObj, const BSONObj& writeConcern) const;

        /**
         * Updates the given role object with the given update modifier.
         * 'writeConcern' contains the arguments to be passed to getLastError to block for
         * successful completion of the write.
         */
        Status updateRoleDocument(const RoleName& role,
                                  const BSONObj& updateObj,
                                  const BSONObj& writeConcern) const;

        /**
         * Updates documents matching "query" according to "updatePattern" in "collectionName".
         * Should only be called on collections with authorization documents in them
         * (ie admin.system.users and admin.system.roles).
         */
        Status updateAuthzDocuments(const NamespaceString& collectionName,
                                    const BSONObj& query,
                                    const BSONObj& updatePattern,
                                    bool upsert,
                                    bool multi,
                                    const BSONObj& writeConcern,
                                    int* nMatched) const;

        /*
         * Removes roles matching the given query.
         * Writes into *numRemoved the number of role documents that were modified.
         * 'writeConcern' contains the arguments to be passed to getLastError to block for
         * successful completion of the write.
         */
        Status removeRoleDocuments(const BSONObj& query,
                                   const BSONObj& writeConcern,
                                   int* numRemoved) const;

        /**
         * Finds all documents matching "query" in "collectionName".  For each document returned,
         * calls the function resultProcessor on it.
         * Should only be called on collections with authorization documents in them
         * (ie admin.system.users and admin.system.roles).
         */
        Status queryAuthzDocument(const NamespaceString& collectionName,
                                  const BSONObj& query,
                                  const BSONObj& projection,
                                  const stdx::function<void(const BSONObj&)>& resultProcessor);

        // Checks to see if "doc" is a valid privilege document, assuming it is stored in the
        // "system.users" collection of database "dbname".
        //
        // Returns Status::OK() if the document is good, or Status(ErrorCodes::BadValue), otherwise.
        Status checkValidPrivilegeDocument(const StringData& dbname, const BSONObj& doc);

        // Given a database name and a readOnly flag return an ActionSet describing all the actions
        // that an old-style user with those attributes should be given.
        ActionSet getActionsForOldStyleUser(const std::string& dbname, bool readOnly) const;

        /**
         * Writes into "result" a document describing the named user and returns Status::OK().  The
         * description includes the user credentials and customData, if present, the user's role
         * membership and delegation information, a full list of the user's privileges, and a full
         * list of the user's roles, including those roles held implicitly through other roles
         * (indirect roles).  In the event that some of this information is inconsistent, the
         * document will contain a "warnings" array, with std::string messages describing
         * inconsistencies.
         *
         * If the user does not exist, returns ErrorCodes::UserNotFound.
         */
        Status getUserDescription(OperationContext* txn, const UserName& userName, BSONObj* result);

        /**
         * Writes into "result" a document describing the named role and returns Status::OK().  The
         * description includes the roles in which the named role has membership and a full list of
         * the roles of which the named role is a member, including those roles memberships held
         * implicitly through other roles (indirect roles). If "showPrivileges" is true, then the
         * description documents will also include a full list of the role's privileges.
         * In the event that some of this information is inconsistent, the document will contain a
         * "warnings" array, with std::string messages describing inconsistencies.
         *
         * If the role does not exist, returns ErrorCodes::RoleNotFound.
         */
        Status getRoleDescription(const RoleName& roleName, bool showPrivileges, BSONObj* result);

        /**
         * Writes into "result" documents describing the roles that are defined on the given
         * database. Each role description document includes the other roles in which the role has
         * membership and a full list of the roles of which the named role is a member,
         * including those roles memberships held implicitly through other roles (indirect roles).
         * If showPrivileges is true, then the description documents will also include a full list
         * of the role's privileges.  If showBuiltinRoles is true, then the result array will
         * contain description documents for all the builtin roles for the given database, if it
         * is false the result will just include user defined roles.
         * In the event that some of the information in a given role description is inconsistent,
         * the document will contain a "warnings" array, with std::string messages describing
         * inconsistencies.
         */
        Status getRoleDescriptionsForDB(const std::string dbname,
                                        bool showPrivileges,
                                        bool showBuiltinRoles,
                                        std::vector<BSONObj>* result);

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
        Status acquireUser(OperationContext* txn, const UserName& userName, User** acquiredUser);

        /**
         * Decrements the refcount of the given User object.  If the refcount has gone to zero,
         * deletes the User.  Caller must stop using its pointer to "user" after calling this.
         */
        void releaseUser(User* user);

        /**
         * Marks the given user as invalid and removes it from the user cache.
         */
        void invalidateUserByName(const UserName& user);

        /**
         * Invalidates all users who's source is "dbname" and removes them from the user cache.
         */
        void invalidateUsersFromDB(const std::string& dbname);

        /**
         * Initializes the authorization manager.  Depending on what version the authorization
         * system is at, this may involve building up the user cache and/or the roles graph.
         * Call this function at startup and after resynchronizing a slave/secondary.
         */
        Status initialize();

        /**
         * Invalidates all of the contents of the user cache.
         */
        void invalidateUserCache();

        /**
         * Parses privDoc and fully initializes the user object (credentials, roles, and privileges)
         * with the information extracted from the privilege document.
         * This should never be called from outside the AuthorizationManager - the only reason it's
         * public instead of private is so it can be unit tested.
         */
        Status _initializeUserFromPrivilegeDocument(User* user, const BSONObj& privDoc);

        /**
         * Tries to acquire the global lock guarding modifications to all persistent data related
         * to authorization, namely the admin.system.users, admin.system.roles, and
         * admin.system.version collections.  This serializes all writers to the authorization
         * documents, but does not impact readers.
         */
        bool tryAcquireAuthzUpdateLock(const StringData& why);

        /**
         * Releases the lock guarding modifications to persistent authorization data, which must
         * already be held.
         */
        void releaseAuthzUpdateLock();

        /**
         * Performs one step in the process of upgrading the stored authorization data to the
         * newest schema.
         *
         * On success, returns Status::OK(), and *isDone will indicate whether there are more
         * steps to perform.
         *
         * If the authorization data is already fully upgraded, returns Status::OK and sets *isDone
         * to true, so this is safe to call on a fully upgraded system.
         *
         * On failure, returns a status other than Status::OK().  In this case, is is typically safe
         * to try again.
         */
        Status upgradeSchemaStep(
                        OperationContext* txn, const BSONObj& writeConcern, bool* isDone);

        /**
         * Performs up to maxSteps steps in the process of upgrading the stored authorization data
         * to the newest schema.  Behaves as if by repeatedly calling upgradeSchemaStep up to
         * maxSteps times until either it completes the upgrade or returns a non-OK status.
         *
         * Invalidates the user cache before the first step and after each attempted step.
         *
         * Returns Status::OK() to indicate that the upgrade process has completed successfully.
         * Returns ErrorCodes::OperationIncomplete to indicate that progress was made, but that more
         * steps must be taken to complete the process.  Other returns indicate a failure to make
         * progress performing the upgrade, and the specific code and message in the returned status
         * may provide additional information.
         */
        Status upgradeSchema(OperationContext* txn, int maxSteps, const BSONObj& writeConcern);

        /**
         * Hook called by replication code to let the AuthorizationManager observe changes
         * to relevant collections.
         */
        void logOp(const char* opstr,
                   const char* ns,
                   const BSONObj& obj,
                   BSONObj* patt,
                   bool* b);

    private:
        /**
         * Type used to guard accesses and updates to the user cache.
         */
        class CacheGuard;
        friend class AuthorizationManager::CacheGuard;

        /**
         * Invalidates all User objects in the cache and removes them from the cache.
         * Should only be called when already holding _cacheMutex.
         */
        void _invalidateUserCache_inlock();

        /**
         * Given the objects describing an oplog entry that affects authorization data, invalidates
         * the portion of the user cache that is affected by that operation.  Should only be called
         * with oplog entries that have been pre-verified to actually affect authorization data.
         */
        void _invalidateRelevantCacheData(const char* op,
                                          const char* ns,
                                          const BSONObj& o,
                                          const BSONObj* o2);

        /**
         * Updates _cacheGeneration to a new OID
         */
        void _updateCacheGeneration_inlock();

        /**
         * Fetches user information from a v2-schema user document for the named user,
         * and stores a pointer to a new user object into *acquiredUser on success.
         */
        Status _fetchUserV2(OperationContext* txn,
                            const UserName& userName,
                            std::auto_ptr<User>* acquiredUser);

        /**
         * True if access control enforcement is enabled in this AuthorizationManager.
         *
         * Defaults to false.  Changes to its value are not synchronized, so it should only be set
         * at initalization-time.
         */
        bool _authEnabled;

        scoped_ptr<AuthzManagerExternalState> _externalState;

        /**
         * Cached value of the authorization schema version.
         *
         * May be set by acquireUser() and getAuthorizationVersion().  Invalidated by
         * invalidateUserCache().
         *
         * Reads and writes guarded by CacheGuard.
         */
        int _version;

        /**
         * Caches User objects with information about user privileges, to avoid the need to
         * go to disk to read user privilege documents whenever possible.  Every User object
         * has a reference count - the AuthorizationManager must not delete a User object in the
         * cache unless its reference count is zero.
         */
        unordered_map<UserName, User*> _userCache;

        /**
         * Current generation of cached data.  Updated every time part of the cache gets
         * invalidated.  Protected by CacheGuard.
         */
        OID _cacheGeneration;

        /**
         * True if there is an update to the _userCache in progress, and that update is currently in
         * the "fetch phase", during which it does not hold the _cacheMutex.
         *
         * Manipulated via CacheGuard.
         */
        bool _isFetchPhaseBusy;

        /**
         * Protects _userCache, _cacheGeneration, _version and _isFetchPhaseBusy.  Manipulated
         * via CacheGuard.
         */
        boost::mutex _cacheMutex;

        /**
         * Condition used to signal that it is OK for another CacheGuard to enter a fetch phase.
         * Manipulated via CacheGuard.
         */
        boost::condition_variable _fetchPhaseIsReady;
    };

} // namespace mongo
