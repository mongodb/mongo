/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/configsvr_run_restore_gen.h"
#include "mongo/db/global_catalog/known_collections.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

enum class ShouldRestoreDocument { kYes, kMaybe, kNo };

/**
 * Given the nss and/or UUID of a config collection document, returns whether the document should be
 * restored based on its presence in the local.system.collections_to_restore collection.
 *
 * If the given nss is referencing a database name only, returns maybe.
 */
ShouldRestoreDocument shouldRestoreDocument(OperationContext* opCtx,
                                            boost::optional<NamespaceString> nss,
                                            boost::optional<UUID> uuid) {
    if (nss && nss->coll().empty()) {
        return ShouldRestoreDocument::kMaybe;
    }

    auto findRequest = FindCommandRequest(NamespaceString::kConfigsvrRestoreNamespace);
    if (nss && uuid) {
        findRequest.setFilter(
            BSON("ns" << NamespaceStringUtil::serialize(*nss, SerializationContext::stateDefault())
                      << "uuid" << *uuid));
    } else if (nss) {
        findRequest.setFilter(BSON(
            "ns" << NamespaceStringUtil::serialize(*nss, SerializationContext::stateDefault())));
    } else if (uuid) {
        findRequest.setFilter(BSON("uuid" << *uuid));
    }

    findRequest.setLimit(1);

    DBDirectClient client(opCtx);
    auto resultCount = client.find(findRequest)->itcount();

    // Log in cases where the schema is not adhered to.
    if (resultCount == 0 && uuid) {
        auto schemaCheckFindRequest =
            FindCommandRequest(NamespaceString::kConfigsvrRestoreNamespace);
        auto collectionsToRestore = client.find(schemaCheckFindRequest);
        while (collectionsToRestore->more()) {
            auto doc = collectionsToRestore->next();
            try {
                (void)UUID::parse(doc);
            } catch (const AssertionException&) {
                uasserted(ErrorCodes::BadValue,
                          str::stream()
                              << "The uuid field of '" << doc.toString() << "' in '"
                              << NamespaceString::kConfigsvrRestoreNamespace.toStringForErrorMsg()
                              << "' needs to be of type UUID");
            }
        }
    }

    return resultCount > 0 ? ShouldRestoreDocument::kYes : ShouldRestoreDocument::kNo;
}

std::set<std::string> getDatabasesToRestore(OperationContext* opCtx) {
    auto findRequest = FindCommandRequest(NamespaceString::kConfigsvrRestoreNamespace);

    std::set<std::string> databasesToRestore;
    DBDirectClient client(opCtx);
    auto it = client.find(findRequest);
    while (it->more()) {
        const auto doc = it->next();
        if (!doc.hasField("ns")) {
            continue;
        }

        NamespaceString nss = NamespaceStringUtil::deserialize(
            boost::none, doc.getStringField("ns"), SerializationContext::stateDefault());
        databasesToRestore.emplace(nss.db_forSharding());
    }

    return databasesToRestore;
}

// Modifications to this map should add new testing in 'sharded_backup_restore.js'.
// { config collection namespace -> ( optional nss field name, optional UUID field name ) }
const stdx::unordered_map<NamespaceString,
                          std::pair<boost::optional<std::string>, boost::optional<std::string>>>
    kCollectionEntries = {
        {NamespaceString::kConfigsvrChunksNamespace,
         std::make_pair(boost::none, std::string("uuid"))},
        {NamespaceString::kConfigsvrCollectionsNamespace,
         std::make_pair(std::string("_id"), std::string("uuid"))},
        {NamespaceString::kMigrationCoordinatorsNamespace,
         std::make_pair(std::string("nss"), std::string("collectionUuid"))},
        {NamespaceString::kConfigsvrTagsNamespace, std::make_pair(std::string("ns"), boost::none)},
        {NamespaceString::kRangeDeletionNamespace,
         std::make_pair(std::string("nss"), std::string("collectionUuid"))},
        {NamespaceString::kShardingDDLCoordinatorsNamespace,
         std::make_pair(std::string("_id.namespace"), boost::none)}};


class ConfigSvrRunRestoreCommand : public TypedCommand<ConfigSvrRunRestoreCommand> {
public:
    using Request = ConfigsvrRunRestore;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandFailed,
                    "This command can only be used in standalone mode or for magicRestore",
                    !repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet() ||
                        storageGlobalParams.magicRestore);

            uassert(ErrorCodes::CommandFailed,
                    "This command can only be run during a restore procedure",
                    storageGlobalParams.restore);

            {
                // The "local.system.collections_to_restore" collection needs to exist prior to
                // running this command.
                const auto coll =
                    acquireCollection(opCtx,
                                      CollectionAcquisitionRequest(
                                          NamespaceString::kConfigsvrRestoreNamespace,
                                          PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                          repl::ReadConcernArgs::get(opCtx),
                                          AcquisitionPrerequisites::kRead),
                                      MODE_IS);
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream()
                            << "Collection "
                            << NamespaceString::kConfigsvrRestoreNamespace.toStringForErrorMsg()
                            << " is missing",
                        coll.exists());
            }

            DBDirectClient client(opCtx);

            if (TestingProctor::instance().isEnabled()) {
                // All collections in the config server must be defined in kConfigCollections.
                // Collections to restore should be defined in kCollectionEntries.
                auto collInfos = client.getCollectionInfos(DatabaseName::kConfig);
                for (auto&& info : collInfos) {
                    StringData collName = info.getStringField("name");
                    // Ignore cache collections as they will be dropped later in the restore
                    // procedure.
                    if (kConfigCollections.find(collName) == kConfigCollections.end() &&
                        !collName.starts_with("cache")) {
                        LOGV2_FATAL(6863300,
                                    "Identified unknown collection in config server.",
                                    "collName"_attr = collName);
                    }
                }
            }

            for (const auto& collectionEntry : kCollectionEntries) {
                const NamespaceString& nss = collectionEntry.first;
                boost::optional<std::string> nssFieldName = collectionEntry.second.first;
                boost::optional<std::string> uuidFieldName = collectionEntry.second.second;


                LOGV2(6261300, "1st Phase - Restoring collection entries", logAttrs(nss));

                const auto coll =
                    acquireCollection(opCtx,
                                      CollectionAcquisitionRequest(
                                          nss,
                                          PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                          repl::ReadConcernArgs::get(opCtx),
                                          AcquisitionPrerequisites::kWrite),
                                      MODE_IX);
                if (!coll.exists()) {
                    LOGV2(6261301, "Collection not found, skipping", logAttrs(nss));
                    continue;
                }

                auto findRequest = FindCommandRequest(nss);
                auto cursor = client.find(findRequest);

                while (cursor->more()) {
                    auto doc = cursor->next();

                    boost::optional<NamespaceString> docNss = boost::none;
                    boost::optional<UUID> docUUID = boost::none;

                    if (nssFieldName) {
                        const size_t dotPosition = nssFieldName->find(".");
                        if (dotPosition != std::string::npos) {
                            // Handles the "_id.namespace" case for collection
                            // "config.system.sharding_ddl_coordinators".
                            const auto obj =
                                doc.getField(nssFieldName->substr(0, dotPosition)).Obj();
                            docNss = NamespaceStringUtil::deserialize(
                                boost::none,
                                obj.getStringField(nssFieldName->substr(dotPosition + 1)),
                                SerializationContext::stateDefault());
                        } else {
                            docNss = NamespaceStringUtil::deserialize(
                                boost::none,
                                doc.getStringField(*nssFieldName),
                                SerializationContext::stateDefault());
                        }
                    }

                    if (uuidFieldName) {
                        auto swDocUUID = UUID::parse(doc.getField(*uuidFieldName));
                        uassertStatusOK(swDocUUID);

                        docUUID = swDocUUID.getValue();
                        LOGV2_DEBUG(6938701,
                                    1,
                                    "uuid found",
                                    "uuid"_attr = uuidFieldName,
                                    "docUUID"_attr = docUUID);
                    }

                    // Time-series bucket collection namespaces are reported using the view
                    // namespace in $backupCursor.
                    if (docNss && docNss->isTimeseriesBucketsCollection()) {
                        docNss = docNss->getTimeseriesViewNamespace();
                    }

                    ShouldRestoreDocument shouldRestore =
                        shouldRestoreDocument(opCtx, docNss, docUUID);

                    LOGV2_DEBUG(6261302,
                                1,
                                "Found document",
                                "doc"_attr = doc,
                                "shouldRestore"_attr = shouldRestore,
                                logAttrs(nss.dbName()),
                                "docNss"_attr = docNss);

                    if (shouldRestore == ShouldRestoreDocument::kYes ||
                        shouldRestore == ShouldRestoreDocument::kMaybe) {
                        continue;
                    }

                    // The collection for this document was not restored, delete it.
                    LOGV2_DEBUG(6938702,
                                1,
                                "Deleting collection that was not restored",
                                logAttrs(nss.dbName()),
                                "uuid"_attr = coll.uuid(),
                                "_id"_attr = doc.getField("_id"));
                    NamespaceStringOrUUID nssOrUUID(nss.dbName(), coll.uuid());
                    uassertStatusOK(repl::StorageInterface::get(opCtx)->deleteById(
                        opCtx, nssOrUUID, doc.getField("_id")));
                }
            }

            // Keeps track of database names for collections restored. Databases with no collections
            // restored will have their entries removed in the config collections.
            std::set<std::string> databasesRestored = getDatabasesToRestore(opCtx);

            {
                const std::vector<NamespaceString> databasesEntries = {
                    NamespaceString::kConfigDatabasesNamespace};

                // Remove database entries from the config collections if no collection for the
                // given database was restored.
                for (const NamespaceString& nss : databasesEntries) {
                    LOGV2(6261303, "2nd Phase - Restoring database entries", logAttrs(nss));

                    const auto coll = acquireCollection(
                        opCtx,
                        CollectionAcquisitionRequest(
                            nss,
                            PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                            repl::ReadConcernArgs::get(opCtx),
                            AcquisitionPrerequisites::kWrite),
                        MODE_IX);
                    if (!coll.exists()) {
                        LOGV2(6261304, "Collection not found, skipping", logAttrs(nss));
                        return;
                    }

                    DBDirectClient client(opCtx);
                    auto findRequest = FindCommandRequest(nss);
                    auto cursor = client.find(findRequest);

                    while (cursor->more()) {
                        auto doc = cursor->next();

                        const NamespaceString dbNss =
                            NamespaceStringUtil::deserialize(boost::none,
                                                             doc.getStringField("_id"),
                                                             SerializationContext::stateDefault());
                        if (!dbNss.coll().empty()) {
                            // We want to handle database only namespaces.
                            continue;
                        }

                        bool shouldRestore =
                            databasesRestored.find(std::string{dbNss.db_forSharding()}) !=
                            databasesRestored.end();

                        LOGV2_DEBUG(6261305,
                                    1,
                                    "Found document",
                                    "doc"_attr = doc,
                                    "shouldRestore"_attr = shouldRestore,
                                    logAttrs(nss.dbName()),
                                    "dbNss"_attr = dbNss);

                        if (shouldRestore) {
                            // This database had at least one collection restored.
                            continue;
                        }

                        // No collection for this database was restored, delete it.
                        LOGV2_DEBUG(6938703,
                                    1,
                                    "Deleting database that was not restored",
                                    logAttrs(nss.dbName()),
                                    "uuid"_attr = coll.uuid(),
                                    "_id"_attr = doc.getField("_id"));
                        NamespaceStringOrUUID nssOrUUID(nss.dbName(), coll.uuid());
                        uassertStatusOK(repl::StorageInterface::get(opCtx)->deleteById(
                            opCtx, nssOrUUID, doc.getField("_id")));
                    }
                }
            }
        }

    private:
        NamespaceString ns() const override {
            return {};
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool skipApiVersionCheck() const override {
        // Internal command used by the restore procedure.
        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigSvrRunRestoreCommand).forShard();

}  // namespace
}  // namespace mongo
