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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/auth_index_d.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;

namespace authindex {

namespace {
BSONObj v1SystemUsersKeyPattern;
BSONObj v3SystemUsersKeyPattern;
BSONObj v3SystemRolesKeyPattern;
std::string v3SystemUsersIndexName;
std::string v3SystemRolesIndexName;

MONGO_INITIALIZER(AuthIndexKeyPatterns)(InitializerContext*) {
    v1SystemUsersKeyPattern = BSON("user" << 1 << "userSource" << 1);
    v3SystemUsersKeyPattern = BSON(
        AuthorizationManager::USER_NAME_FIELD_NAME << 1 << AuthorizationManager::USER_DB_FIELD_NAME
                                                   << 1);
    v3SystemRolesKeyPattern = BSON(
        AuthorizationManager::ROLE_NAME_FIELD_NAME << 1 << AuthorizationManager::ROLE_DB_FIELD_NAME
                                                   << 1);
    v3SystemUsersIndexName =
        std::string(str::stream() << AuthorizationManager::USER_NAME_FIELD_NAME << "_1_"
                                  << AuthorizationManager::USER_DB_FIELD_NAME
                                  << "_1");
    v3SystemRolesIndexName =
        std::string(str::stream() << AuthorizationManager::ROLE_NAME_FIELD_NAME << "_1_"
                                  << AuthorizationManager::ROLE_DB_FIELD_NAME
                                  << "_1");

    return Status::OK();
}

}  // namespace

Status verifySystemIndexes(OperationContext* txn) {
    const NamespaceString systemUsers = AuthorizationManager::usersCollectionNamespace;

    // Make sure the old unique index from v2.4 on system.users doesn't exist.
    ScopedTransaction scopedXact(txn, MODE_IX);
    AutoGetDb autoDb(txn, systemUsers.db(), MODE_X);
    if (!autoDb.getDb()) {
        return Status::OK();
    }

    Collection* collection = autoDb.getDb()->getCollection(NamespaceString(systemUsers));
    if (!collection) {
        return Status::OK();
    }

    IndexCatalog* indexCatalog = collection->getIndexCatalog();
    IndexDescriptor* oldIndex = NULL;

    if (indexCatalog &&
        (oldIndex = indexCatalog->findIndexByKeyPattern(txn, v1SystemUsersKeyPattern))) {
        return Status(ErrorCodes::AuthSchemaIncompatible,
                      "Old 2.4 style user index identified. "
                      "The authentication schema needs to be updated by "
                      "running authSchemaUpgrade on a 2.6 server.");
    }

    return Status::OK();
}

void createSystemIndexes(OperationContext* txn, Collection* collection) {
    invariant(collection);
    const NamespaceString& ns = collection->ns();
    if (ns == AuthorizationManager::usersCollectionNamespace) {
        collection->getIndexCatalog()->createIndexOnEmptyCollection(
            txn,
            BSON("name" << v3SystemUsersIndexName << "ns" << collection->ns().ns() << "key"
                        << v3SystemUsersKeyPattern
                        << "unique"
                        << true));
    } else if (ns == AuthorizationManager::rolesCollectionNamespace) {
        collection->getIndexCatalog()->createIndexOnEmptyCollection(
            txn,
            BSON("name" << v3SystemRolesIndexName << "ns" << collection->ns().ns() << "key"
                        << v3SystemRolesKeyPattern
                        << "unique"
                        << true));
    }
}

}  // namespace authindex
}  // namespace mongo
