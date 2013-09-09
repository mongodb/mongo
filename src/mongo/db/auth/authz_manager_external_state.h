/*
*    Copyright (C) 2012 10gen Inc.
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

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

    /**
     * Public interface for a class that encapsulates all the information related to system
     * state not stored in AuthorizationManager.  This is primarily to make AuthorizationManager
     * easier to test as well as to allow different implementations for mongos and mongod.
     */
    class AuthzManagerExternalState {
        MONGO_DISALLOW_COPYING(AuthzManagerExternalState);

    public:

        virtual ~AuthzManagerExternalState();

        // Gets the privilege information document for "userName".  authzVersion indicates what
        // version of the privilege document format is being used, which is needed to know how to
        // query for the user's privilege document.
        //
        // On success, returns Status::OK() and stores a shared-ownership copy of the document into
        // "result".
        Status getPrivilegeDocument(const UserName& userName,
                                    int authzVersion,
                                    BSONObj* result);

        // Returns true if there exists at least one privilege document in the system.
        bool hasAnyPrivilegeDocuments();

        // Creates the given user object in the given database.
        // TODO(spencer): remove dbname argument once users are only written into the admin db
        virtual Status insertPrivilegeDocument(const std::string& dbname,
                                               const BSONObj& userObj,
                                               const BSONObj& writeConcern) = 0;

        // Updates the given user object with the given update modifier.
        virtual Status updatePrivilegeDocument(const UserName& user,
                                               const BSONObj& updateObj,
                                               const BSONObj& writeConcern) = 0;

        // Removes users for the given database matching the given query.
        // Writes into *numRemoved the number of user documents that were modified.
        virtual Status removePrivilegeDocuments(const BSONObj& query,
                                                const BSONObj& writeConcern,
                                                int* numRemoved) = 0;

        /**
         * Puts into the *dbnames vector the name of every database in the cluster.
         */
        virtual Status getAllDatabaseNames(std::vector<std::string>* dbnames) = 0;

        /**
         * Puts into the *privDocs vector every privilege document from the given database's
         * system.users collection.
         */
        virtual Status getAllV1PrivilegeDocsForDB(const std::string& dbname,
                                                  std::vector<BSONObj>* privDocs) = 0;

        /**
         * Finds a document matching "query" in "collectionName", and store a shared-ownership
         * copy into "result".
         *
         * Returns Status::OK() on success.  If no match is found, returns
         * ErrorCodes::NoMatchingDocument.  Other errors returned as appropriate.
         */
        virtual Status findOne(const NamespaceString& collectionName,
                               const BSONObj& query,
                               BSONObj* result) = 0;

        /**
         * Inserts "document" into "collectionName".
         */
        virtual Status insert(const NamespaceString& collectionName,
                              const BSONObj& document,
                              const BSONObj& writeConcern) = 0;

        /**
         * Update one document matching "query" according to "updatePattern" in "collectionName".
         *
         * If "upsert" is true and no document matches "query", inserts one using "query" as a
         * template.
         */
        virtual Status updateOne(const NamespaceString& collectionName,
                                 const BSONObj& query,
                                 const BSONObj& updatePattern,
                                 bool upsert,
                                 const BSONObj& writeConcern) = 0;

        /**
         * Removes all documents matching "query" from "collectionName".
         */
        virtual Status remove(const NamespaceString& collectionName,
                              const BSONObj& query,
                              const BSONObj& writeConcern) = 0;

        /**
         * Creates an index with the given pattern on "collectionName".
         */
        virtual Status createIndex(const NamespaceString& collectionName,
                                   const BSONObj& pattern,
                                   bool unique,
                                   const BSONObj& writeConcern) = 0;

        /**
         * Drops the named collection.
         */
        virtual Status dropCollection(const NamespaceString& collectionName,
                                      const BSONObj& writeConcern) = 0;

        /**
         * Renames collection "oldName" to "newName", possibly dropping the previous
         * collection named "newName".
         */
        virtual Status renameCollection(const NamespaceString& oldName,
                                        const NamespaceString& newName,
                                        const BSONObj& writeConcern) = 0;

        /**
         * Copies the contents of collection "fromName" into "toName".  Fails
         * if "toName" is already a collection.
         */
        virtual Status copyCollection(const NamespaceString& fromName,
                                      const NamespaceString& toName,
                                      const BSONObj& writeConcern) = 0;

        /**
         * Tries to acquire the global lock guarding modifications to all persistent data related
         * to authorization, namely the admin.system.users, admin.system.roles, and
         * admin.system.version collections.  This serializes all writers to the authorization
         * documents, but does not impact readers.
         * This can only be called when already in the AuthorizationManager's _lock.
         */
        virtual bool tryAcquireAuthzUpdateLock(const StringData& why) = 0;

        /**
         * Releases the lock guarding modifications to persistent authorization data, which must
         * already be held.
         * This can only be called when already in the AuthorizationManager's _lock.
         */
        virtual void releaseAuthzUpdateLock() = 0;

    protected:
        AuthzManagerExternalState(); // This class should never be instantiated directly.

        // Queries the userNamespace with the given query and returns the privilegeDocument found
        // in *result.  Returns Status::OK if it finds a document matching the query.  If it doesn't
        // find a document matching the query, returns a Status with code UserNotFound.  Other
        // errors may return other Status codes.
        virtual Status _findUser(const std::string& usersNamespace,
                                 const BSONObj& query,
                                 BSONObj* result) = 0;
    };

} // namespace mongo
