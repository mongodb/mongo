/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/system_index.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/client/index_spec.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

using namespace std::chrono_literals;

namespace mongo {

namespace {
BSONObj v1SystemUsersKeyPattern;
BSONObj v3SystemUsersKeyPattern;
BSONObj v3SystemRolesKeyPattern;
std::string v3SystemUsersIndexName;
std::string v3SystemRolesIndexName;
IndexSpec v3SystemUsersIndexSpec;
IndexSpec v3SystemRolesIndexSpec;

const NamespaceString sessionCollectionNamespace("config.system.sessions");

MONGO_INITIALIZER(AuthIndexKeyPatterns)(InitializerContext*) {
    v1SystemUsersKeyPattern = BSON("user" << 1 << "userSource" << 1);
    v3SystemUsersKeyPattern = BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                                   << 1 << AuthorizationManager::USER_DB_FIELD_NAME << 1);
    v3SystemRolesKeyPattern = BSON(AuthorizationManager::ROLE_NAME_FIELD_NAME
                                   << 1 << AuthorizationManager::ROLE_DB_FIELD_NAME << 1);
    v3SystemUsersIndexName =
        std::string(str::stream() << AuthorizationManager::USER_NAME_FIELD_NAME << "_1_"
                                  << AuthorizationManager::USER_DB_FIELD_NAME << "_1");
    v3SystemRolesIndexName =
        std::string(str::stream() << AuthorizationManager::ROLE_NAME_FIELD_NAME << "_1_"
                                  << AuthorizationManager::ROLE_DB_FIELD_NAME << "_1");

    v3SystemUsersIndexSpec.addKeys(v3SystemUsersKeyPattern);
    v3SystemUsersIndexSpec.unique();
    v3SystemUsersIndexSpec.name(v3SystemUsersIndexName);

    v3SystemRolesIndexSpec.addKeys(v3SystemRolesKeyPattern);
    v3SystemRolesIndexSpec.unique();
    v3SystemRolesIndexSpec.name(v3SystemRolesIndexName);

    return Status::OK();
}

void generateSystemIndexForExistingCollection(OperationContext* opCtx,
                                              UUID collectionUUID,
                                              const NamespaceString& ns,
                                              const IndexSpec& spec) {
    // Do not try to generate any system indexes on a secondary.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    uassert(ErrorCodes::NotMaster,
            "Not primary while creating authorization index",
            replCoord->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet ||
                replCoord->canAcceptWritesForDatabase(opCtx, ns.db()));

    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    try {
        auto indexSpecStatus = index_key_validate::validateIndexSpec(
            opCtx, spec.toBSON(), serverGlobalParams.featureCompatibility);
        BSONObj indexSpec = fassert(40452, indexSpecStatus);

        LOGV2(22488,
              "No authorization index detected on {ns} collection. Attempting to recover by "
              "creating an index with spec: {indexSpec}",
              "ns"_attr = ns,
              "indexSpec"_attr = indexSpec);

        auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
        auto fromMigrate = false;
        IndexBuildsCoordinator::get(opCtx)->createIndexes(
            opCtx, collectionUUID, {indexSpec}, indexConstraints, fromMigrate);
    } catch (const DBException& e) {
        LOGV2_FATAL_CONTINUE(22490,
                             "Failed to regenerate index for {ns}. Exception: {e_what}",
                             "ns"_attr = ns,
                             "e_what"_attr = e.what());
        throw;
    }
}

}  // namespace

Status verifySystemIndexes(OperationContext* opCtx) {
    // Do not try and generate any system indexes in read only mode.
    if (storageGlobalParams.readOnly) {
        LOGV2_WARNING(22489,
                      "Running in queryable backup mode. Unable to create authorization indexes");
        return Status::OK();
    }

    const NamespaceString& systemUsers = AuthorizationManager::usersCollectionNamespace;
    const NamespaceString& systemRoles = AuthorizationManager::rolesCollectionNamespace;

    // Create indexes for the admin.system.users collection.
    {
        AutoGetCollection autoColl(opCtx, systemUsers, MODE_X);

        if (Collection* collection = autoColl.getCollection()) {
            IndexCatalog* indexCatalog = collection->getIndexCatalog();
            invariant(indexCatalog);

            // Make sure the old unique index from v2.4 on system.users doesn't exist.
            std::vector<const IndexDescriptor*> indexes;
            indexCatalog->findIndexesByKeyPattern(opCtx, v1SystemUsersKeyPattern, false, &indexes);

            if (!indexes.empty()) {
                fassert(ErrorCodes::AmbiguousIndexKeyPattern, indexes.size() == 1);
                return Status(ErrorCodes::AuthSchemaIncompatible,
                              "Old 2.4 style user index identified. "
                              "The authentication schema needs to be updated by "
                              "running authSchemaUpgrade on a 2.6 server.");
            }

            // Ensure that system indexes exist for the user collection.
            indexCatalog->findIndexesByKeyPattern(opCtx, v3SystemUsersKeyPattern, false, &indexes);
            if (indexes.empty()) {
                try {
                    generateSystemIndexForExistingCollection(
                        opCtx, collection->uuid(), systemUsers, v3SystemUsersIndexSpec);
                } catch (...) {
                    return exceptionToStatus();
                }
            }
        }
    }

    // Create indexes for the admin.system.roles collection.
    {
        AutoGetCollection autoColl(opCtx, systemRoles, MODE_X);

        // Ensure that system indexes exist for the roles collection, if it exists.
        if (Collection* collection = autoColl.getCollection()) {
            IndexCatalog* indexCatalog = collection->getIndexCatalog();
            invariant(indexCatalog);

            std::vector<const IndexDescriptor*> indexes;
            indexCatalog->findIndexesByKeyPattern(opCtx, v3SystemRolesKeyPattern, false, &indexes);
            if (indexes.empty()) {
                try {
                    generateSystemIndexForExistingCollection(
                        opCtx, collection->uuid(), systemRoles, v3SystemRolesIndexSpec);
                } catch (...) {
                    return exceptionToStatus();
                }
            }
        }
    }

    return Status::OK();
}

void createSystemIndexes(OperationContext* opCtx, Collection* collection) {
    invariant(collection);
    const NamespaceString& ns = collection->ns();
    BSONObj indexSpec;
    if (ns == AuthorizationManager::usersCollectionNamespace) {
        indexSpec = fassert(
            40455,
            index_key_validate::validateIndexSpec(
                opCtx, v3SystemUsersIndexSpec.toBSON(), serverGlobalParams.featureCompatibility));

    } else if (ns == AuthorizationManager::rolesCollectionNamespace) {
        indexSpec = fassert(
            40457,
            index_key_validate::validateIndexSpec(
                opCtx, v3SystemRolesIndexSpec.toBSON(), serverGlobalParams.featureCompatibility));
    }
    if (!indexSpec.isEmpty()) {
        auto fromMigrate = false;
        try {
            IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                opCtx, collection->uuid(), {indexSpec}, fromMigrate);
        } catch (DBException& ex) {
            fassertFailedWithStatus(40456, ex.toStatus());
        }
    }
}

}  // namespace mongo
