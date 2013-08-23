/**
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/user_name.h"

namespace mongo {

    /**
     * The implementation of AuthzManagerExternalState functionality for mongos.
     */
    class AuthzManagerExternalStateMongos : public AuthzManagerExternalState{
        MONGO_DISALLOW_COPYING(AuthzManagerExternalStateMongos);

    public:
        AuthzManagerExternalStateMongos();
        virtual ~AuthzManagerExternalStateMongos();

        virtual Status insertPrivilegeDocument(const std::string& dbname,
                                               const BSONObj& userObj);

        virtual Status updatePrivilegeDocument(const UserName& user,
                                               const BSONObj& updateObj);

        virtual Status removePrivilegeDocuments(const std::string& dbname,
                                                const BSONObj& query);

        virtual Status getAllDatabaseNames(std::vector<std::string>* dbnames);

        virtual Status getAllV1PrivilegeDocsForDB(const std::string& dbname,
                                                  std::vector<BSONObj>* privDocs);

        virtual Status findOne(const NamespaceString& collectionName,
                               const BSONObj& query,
                               BSONObj* result);
        virtual Status insert(const NamespaceString& collectionName,
                              const BSONObj& document);
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

    protected:
        virtual Status _findUser(const string& usersNamespace,
                                 const BSONObj& query,
                                 BSONObj* result);
    };

} // namespace mongo
