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
#include "mongo/client/index_spec.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
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
IndexSpec v3SystemUsersIndexSpec;
IndexSpec v3SystemRolesIndexSpec;

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

    v3SystemUsersIndexSpec.addKeys(v3SystemUsersKeyPattern);
    v3SystemUsersIndexSpec.unique();
    v3SystemUsersIndexSpec.name(v3SystemUsersIndexName);

    v3SystemRolesIndexSpec.addKeys(v3SystemRolesKeyPattern);
    v3SystemRolesIndexSpec.unique();
    v3SystemRolesIndexSpec.name(v3SystemRolesIndexName);

    return Status::OK();
}

void generateSystemIndexForExistingCollection(OperationContext* opCtx,
                                              Collection* collection,
                                              const NamespaceString& ns,
                                              const IndexSpec& spec) {
    try {
        auto indexSpecStatus = index_key_validate::validateIndexSpec(
            spec.toBSON(), ns, serverGlobalParams.featureCompatibility);
        BSONObj indexSpec = fassertStatusOK(40452, indexSpecStatus);

        log() << "No authorization index detected on " << ns
              << " collection. Attempting to recover by creating an index with spec: " << indexSpec;

        MultiIndexBlock indexer(opCtx, collection);

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            fassertStatusOK(40453, indexer.init(indexSpec));
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "authorization index regeneration", ns.ns());

        fassertStatusOK(40454, indexer.insertAllDocumentsInCollection());

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wunit(opCtx);

            indexer.commit();

            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "authorization index regeneration", ns.ns());

        log() << "Authorization index construction on " << ns << " is complete";
    } catch (const DBException& e) {
        severe() << "Failed to regenerate index for " << ns << ". Exception: " << e.what();
        throw;
    }
}

}  // namespace

Status verifySystemIndexes(OperationContext* opCtx) {
    const NamespaceString& systemUsers = AuthorizationManager::usersCollectionNamespace;
    const NamespaceString& systemRoles = AuthorizationManager::rolesCollectionNamespace;

    AutoGetDb autoDb(opCtx, systemUsers.db(), MODE_X);
    if (!autoDb.getDb()) {
        return Status::OK();
    }

    Collection* collection = autoDb.getDb()->getCollection(opCtx, systemUsers);
    if (collection) {
        IndexCatalog* indexCatalog = collection->getIndexCatalog();
        invariant(indexCatalog);

        // Make sure the old unique index from v2.4 on system.users doesn't exist.
        std::vector<IndexDescriptor*> indexes;
        indexCatalog->findIndexesByKeyPattern(opCtx, v1SystemUsersKeyPattern, false, &indexes);

        if (!indexes.empty()) {
            fassert(ErrorCodes::AmbiguousIndexKeyPattern, indexes.size() == 1);
            return Status(ErrorCodes::AuthSchemaIncompatible,
                          "Old 2.4 style user index identified. "
                          "The authentication schema needs to be updated by "
                          "running authSchemaUpgrade on a 2.6 server.");
        }

        // Ensure that system indexes exist for the user collection
        indexCatalog->findIndexesByKeyPattern(opCtx, v3SystemUsersKeyPattern, false, &indexes);
        if (indexes.empty()) {
            try {
                generateSystemIndexForExistingCollection(
                    opCtx, collection, systemUsers, v3SystemUsersIndexSpec);
            } catch (...) {
                return exceptionToStatus();
            }
        }
    }

    // Ensure that system indexes exist for the roles collection, if it exists.
    collection = autoDb.getDb()->getCollection(opCtx, systemRoles);
    if (collection) {
        IndexCatalog* indexCatalog = collection->getIndexCatalog();
        invariant(indexCatalog);

        std::vector<IndexDescriptor*> indexes;
        indexCatalog->findIndexesByKeyPattern(opCtx, v3SystemRolesKeyPattern, false, &indexes);
        if (indexes.empty()) {
            try {
                generateSystemIndexForExistingCollection(
                    opCtx, collection, systemRoles, v3SystemRolesIndexSpec);
            } catch (...) {
                return exceptionToStatus();
            }
        }
    }


    return Status::OK();
}

void createSystemIndexes(OperationContext* opCtx, Collection* collection) {
    invariant(collection);
    const NamespaceString& ns = collection->ns();
    if (ns == AuthorizationManager::usersCollectionNamespace) {
        auto indexSpec = fassertStatusOK(
            40455,
            index_key_validate::validateIndexSpec(
                v3SystemUsersIndexSpec.toBSON(), ns, serverGlobalParams.featureCompatibility));

        fassertStatusOK(
            40456, collection->getIndexCatalog()->createIndexOnEmptyCollection(opCtx, indexSpec));
    } else if (ns == AuthorizationManager::rolesCollectionNamespace) {
        auto indexSpec = fassertStatusOK(
            40457,
            index_key_validate::validateIndexSpec(
                v3SystemRolesIndexSpec.toBSON(), ns, serverGlobalParams.featureCompatibility));

        fassertStatusOK(
            40458, collection->getIndexCatalog()->createIndexOnEmptyCollection(opCtx, indexSpec));
    }
}

}  // namespace authindex
}  // namespace mongo
