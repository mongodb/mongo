// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/system_index.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/index_spec.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <chrono>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


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
}

void generateSystemIndexForExistingCollection(OperationContext* opCtx,
                                              UUID collectionUUID,
                                              const NamespaceString& ns,
                                              const IndexSpec& spec) {
    // Do not try to generate any system indexes on a secondary.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    uassert(ErrorCodes::NotWritablePrimary,
            "Not primary while creating authorization index",
            !replCoord->getSettings().isReplSet() ||
                replCoord->canAcceptWritesForDatabase(opCtx, ns.dbName()));

    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    try {
        auto indexSpecStatus = index_key_validate::validateIndexSpec(opCtx, spec.toBSON());
        BSONObj indexSpec = fassert(40452, indexSpecStatus);

        LOGV2(22488,
              "No authorization index detected. Attempting to recover by "
              "creating an index",
              logAttrs(ns),
              "indexSpec"_attr = indexSpec);

        auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
        auto fromMigrate = false;
        IndexBuildsCoordinator::get(opCtx)->createIndex(
            opCtx, collectionUUID, indexSpec, indexConstraints, fromMigrate);
    } catch (const DBException& e) {
        LOGV2_FATAL_CONTINUE(
            22490, "Failed to regenerate index", logAttrs(ns), "error"_attr = e.what());
        throw;
    }
}

}  // namespace

Status verifySystemIndexes(OperationContext* opCtx, BSONObjBuilder* startupTimeElapsedBuilder) {
    const NamespaceString& systemUsers = NamespaceString::kAdminUsersNamespace;
    const NamespaceString& systemRoles = NamespaceString::kAdminRolesNamespace;

    auto options = auto_get_collection::Options{}.globalLockOptions(Lock::GlobalLockOptions{
        .explicitIntent = rss::consensus::IntentRegistry::Intent::LocalWrite});

    // Create indexes for the admin.system.users collection.
    {
        AutoGetCollection collection(opCtx, systemUsers, MODE_X, options);

        if (collection) {
            SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                           TimedSectionId::createSystemUsersIndex,
                                           startupTimeElapsedBuilder);
            const IndexCatalog* indexCatalog = collection->getIndexCatalog();
            invariant(indexCatalog);

            // Make sure the old unique index from v2.4 on system.users doesn't exist.
            std::vector<const IndexCatalogEntry*> indexes;
            indexCatalog->findIndexesByKeyPattern(
                opCtx, v1SystemUsersKeyPattern, IndexCatalog::InclusionPolicy::kReady, &indexes);

            if (!indexes.empty()) {
                fassert(9097915, indexes.size() == 1);
                return Status(ErrorCodes::AuthSchemaIncompatible,
                              "Old 2.4 style user index identified. "
                              "The authentication schema needs to be updated by "
                              "running authSchemaUpgrade on a 2.6 server.");
            }

            // Ensure that system indexes exist for the user collection.
            indexCatalog->findIndexesByKeyPattern(
                opCtx, v3SystemUsersKeyPattern, IndexCatalog::InclusionPolicy::kReady, &indexes);
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
        AutoGetCollection collection(opCtx, systemRoles, MODE_X, options);

        // Ensure that system indexes exist for the roles collection, if it exists.
        if (collection) {
            SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                           TimedSectionId::createSystemRolesIndex,
                                           startupTimeElapsedBuilder);
            const IndexCatalog* indexCatalog = collection->getIndexCatalog();
            invariant(indexCatalog);

            std::vector<const IndexCatalogEntry*> indexes;
            indexCatalog->findIndexesByKeyPattern(
                opCtx, v3SystemRolesKeyPattern, IndexCatalog::InclusionPolicy::kReady, &indexes);
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

void createSystemIndexes(OperationContext* opCtx, CollectionWriter& collection, bool fromMigrate) {
    invariant(collection);
    const NamespaceString& ns = collection->ns();
    BSONObj indexSpec;
    if (ns == NamespaceString::kAdminUsersNamespace) {
        indexSpec = fassert(
            40455, index_key_validate::validateIndexSpec(opCtx, v3SystemUsersIndexSpec.toBSON()));

    } else if (ns == NamespaceString::kAdminRolesNamespace) {
        indexSpec = fassert(
            40457, index_key_validate::validateIndexSpec(opCtx, v3SystemRolesIndexSpec.toBSON()));
    }
    if (!indexSpec.isEmpty()) {
        try {
            IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                opCtx, collection, {indexSpec}, fromMigrate);
        } catch (const StorageUnavailableException&) {
            throw;
        } catch (const DBException& ex) {
            if (!opCtx->checkForInterruptNoAssert().isOK()) {
                throw;
            }
            fassertFailedWithStatus(40456, ex.toStatus());
        }
    }
}

}  // namespace mongo
