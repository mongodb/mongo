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
*/

#pragma once

#include <string>
#include <map>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

    /**
     * Mock of the AuthzManagerExternalState class used only for testing.
     */
    class AuthzManagerExternalStateMock : public AuthzManagerExternalState {
        MONGO_DISALLOW_COPYING(AuthzManagerExternalStateMock);

    public:

        AuthzManagerExternalStateMock() {};

        virtual Status insertPrivilegeDocument(const std::string& dbname,
                                               const BSONObj& userObj);

        virtual Status updatePrivilegeDocument(const UserName& user,
                                               const BSONObj& updateObj);

        virtual Status removePrivilegeDocuments(const std::string& dbname,
                                                const BSONObj& query);

        void clearPrivilegeDocuments();

        virtual Status getAllDatabaseNames(std::vector<std::string>* dbnames);

        virtual Status getAllV1PrivilegeDocsForDB(const std::string& dbname,
                                                  std::vector<BSONObj>* privDocs);

        virtual Status _findUser(const std::string& usersNamespace,
                                 const BSONObj& query,
                                 BSONObj* result);

        virtual Status findOne(const NamespaceString& collectionName,
                               const BSONObj& query,
                               BSONObj* result);

        // This implementation does not understand uniqueness constraints.
        virtual Status insert(const NamespaceString& collectionName,
                              const BSONObj& document);

        // This implementation does not understand uniqueness constraints,
        // and only correctly handles some upsert behaviors.
        virtual Status updateOne(const NamespaceString& collectionName,
                                 const BSONObj& query,
                                 const BSONObj& updatePattern,
                                 bool upsert);
        virtual Status remove(const NamespaceString& collectionName,
                              const BSONObj& query);
        virtual Status createIndex(const NamespaceString& collectionName,
                                   const BSONObj& pattern,
                                   bool unique);
        virtual Status dropCollection(const NamespaceString& collectionName);
        virtual Status renameCollection(const NamespaceString& oldName,
                                        const NamespaceString& newName);
        virtual Status copyCollection(const NamespaceString& fromName,
                                      const NamespaceString& toName);
        virtual bool tryLockUpgradeProcess();
        virtual void unlockUpgradeProcess();

        std::vector<BSONObj> getCollectionContents(const NamespaceString& collectionName);

    private:
        typedef std::vector<BSONObj> BSONObjCollection;
        typedef std::map<NamespaceString, BSONObjCollection> NamespaceDocumentMap;

        Status _findOneIter(const NamespaceString& collectionName,
                            const BSONObj& query,
                            BSONObjCollection::iterator* result);

        NamespaceDocumentMap _documents; // Mock database.
    };

} // namespace mongo
