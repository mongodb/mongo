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

#include "mongo/db/auth/auth_index_d.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace authindex {

namespace {
    BSONObj v1SystemUsersKeyPattern;
    BSONObj v3SystemUsersKeyPattern;
    BSONObj v3SystemRolesKeyPattern;
    std::string v3SystemUsersIndexName;
    std::string v3SystemRolesIndexName;

    MONGO_INITIALIZER(AuthIndexKeyPatterns)(InitializerContext*) {
        v1SystemUsersKeyPattern = BSON("user" << 1 << "userSource" << 1);
        v3SystemUsersKeyPattern = BSON(AuthorizationManager::USER_NAME_FIELD_NAME << 1 <<
                                       AuthorizationManager::USER_DB_FIELD_NAME << 1);
        v3SystemRolesKeyPattern = BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME << 1 <<
                                       AuthorizationManager::ROLE_DB_FIELD_NAME << 1);
        v3SystemUsersIndexName = std::string(
                str::stream() <<
                        AuthorizationManager::USER_NAME_FIELD_NAME << "_1_" <<
                        AuthorizationManager::USER_DB_FIELD_NAME << "_1");
        v3SystemRolesIndexName = std::string(
                str::stream() <<
                        AuthorizationManager::ROLE_NAME_FIELD_NAME << "_1_" <<
                        AuthorizationManager::ROLE_DB_FIELD_NAME << "_1");

        return Status::OK();
    }

}  // namespace

    void configureSystemIndexes(OperationContext* txn, const StringData& dbname) {
        int authzVersion;
        Status status = getGlobalAuthorizationManager()->getAuthorizationVersion(
                                                                txn, &authzVersion);
        if (!status.isOK()) {
            return;
        }

        if (dbname == "admin" && authzVersion == AuthorizationManager::schemaVersion26Final) {
            NamespaceString systemUsers(dbname, "system.users");

            // Make sure the old unique index from v2.4 on system.users doesn't exist.
            Client::WriteContext wctx(txn, systemUsers);
            Collection* collection = wctx.ctx().db()->getCollection(txn,
                                                                    NamespaceString(systemUsers));
            if (!collection) {
                return;
            }
            IndexCatalog* indexCatalog = collection->getIndexCatalog();
            IndexDescriptor* oldIndex = NULL;
            while ((oldIndex = indexCatalog->findIndexByKeyPattern(txn, v1SystemUsersKeyPattern))) {
                indexCatalog->dropIndex(txn, oldIndex);
            }
            wctx.commit();
        }
    }

    void createSystemIndexes(OperationContext* txn, Collection* collection) {
        invariant( collection );
        const NamespaceString& ns = collection->ns();
        if (ns == AuthorizationManager::usersCollectionNamespace) {
            try {
                Helpers::ensureIndex(txn,
                                     collection,
                                     v3SystemUsersKeyPattern,
                                     true,  // unique
                                     v3SystemUsersIndexName.c_str());
            } catch (const DBException& e) {
                if (e.getCode() == ASSERT_ID_DUPKEY) {
                    log() << "Duplicate key exception while trying to build unique index on " <<
                            ns << ".  This is likely due to problems during the upgrade process " <<
                            endl;
                }
                throw;
            }
        } else if (ns == AuthorizationManager::rolesCollectionNamespace) {
            try {
                Helpers::ensureIndex(txn,
                                     collection,
                                     v3SystemRolesKeyPattern,
                                     true,  // unique
                                     v3SystemRolesIndexName.c_str());
            } catch (const DBException& e) {
                if (e.getCode() == ASSERT_ID_DUPKEY) {
                    log() << "Duplicate key exception while trying to build unique index on " <<
                            ns << "." << endl;
                }
                throw;
            }
        }
    }

}  // namespace authindex
}  // namespace mongo
