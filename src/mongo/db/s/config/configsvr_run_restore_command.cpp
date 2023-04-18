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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

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
        findRequest.setFilter(BSON("ns" << nss->toString() << "uuid" << *uuid));
    } else if (nss) {
        findRequest.setFilter(BSON("ns" << nss->toString()));
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
                          str::stream() << "The uuid field of '" << doc.toString() << "' in '"
                                        << NamespaceString::kConfigsvrRestoreNamespace.toString()
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

        NamespaceString nss(doc.getStringField("ns"));
        databasesToRestore.emplace(nss.db());
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

class ConfigSvrRunRestoreCommand : public BasicCommand {
public:
    ConfigSvrRunRestoreCommand() : BasicCommand("_configsvrRunRestore") {}

    bool skipApiVersionCheck() const override {
        // Internal command used by the restore procedure.
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::CommandFailed,
                "This command can only be used in standalone mode",
                !repl::ReplicationCoordinator::get(opCtx)->getSettings().usingReplSets());

        uassert(ErrorCodes::CommandFailed,
                "This command can only be run during a restore procedure",
                storageGlobalParams.restore);

        {
            // The "local.system.collections_to_restore" collection needs to exist prior to running
            // this command.
            CollectionPtr restoreColl(CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
                opCtx, NamespaceString::kConfigsvrRestoreNamespace));
            uassert(
                ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection "
                              << NamespaceString::kConfigsvrRestoreNamespace.toStringForErrorMsg()
                              << " is missing",
                restoreColl);
        }

        for (const auto& collectionEntry : kCollectionEntries) {
            const NamespaceString& nss = collectionEntry.first;
            boost::optional<std::string> nssFieldName = collectionEntry.second.first;
            boost::optional<std::string> uuidFieldName = collectionEntry.second.second;


            LOGV2(6261300, "1st Phase - Restoring collection entries", logAttrs(nss));
            CollectionPtr coll(
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));
            if (!coll) {
                LOGV2(6261301, "Collection not found, skipping", logAttrs(nss));
                continue;
            }

            DBDirectClient client(opCtx);
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
                        const auto obj = doc.getField(nssFieldName->substr(0, dotPosition)).Obj();
                        docNss = NamespaceString(
                            obj.getStringField(nssFieldName->substr(dotPosition + 1)));
                    } else {
                        docNss = NamespaceString(doc.getStringField(*nssFieldName));
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

                ShouldRestoreDocument shouldRestore = shouldRestoreDocument(opCtx, docNss, docUUID);

                LOGV2_DEBUG(6261302,
                            1,
                            "Found document",
                            "doc"_attr = doc,
                            "shouldRestore"_attr = shouldRestore,
                            logAttrs(coll->ns().dbName()),
                            "docNss"_attr = docNss);

                if (shouldRestore == ShouldRestoreDocument::kYes ||
                    shouldRestore == ShouldRestoreDocument::kMaybe) {
                    continue;
                }

                // The collection for this document was not restored, delete it.
                LOGV2_DEBUG(6938702,
                            1,
                            "Deleting collection that was not restored",
                            logAttrs(coll->ns().dbName()),
                            "uuid"_attr = coll->uuid(),
                            "_id"_attr = doc.getField("_id"));
                NamespaceStringOrUUID nssOrUUID(coll->ns().db().toString(), coll->uuid());
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

            // Remove database entries from the config collections if no collection for the given
            // database was restored.
            for (const NamespaceString& nss : databasesEntries) {
                LOGV2(6261303, "2nd Phase - Restoring database entries", logAttrs(nss));
                CollectionPtr coll(
                    CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));
                if (!coll) {
                    LOGV2(6261304, "Collection not found, skipping", logAttrs(nss));
                    return true;
                }

                DBDirectClient client(opCtx);
                auto findRequest = FindCommandRequest(nss);
                auto cursor = client.find(findRequest);

                while (cursor->more()) {
                    auto doc = cursor->next();

                    const NamespaceString dbNss = NamespaceString(doc.getStringField("_id"));
                    if (!dbNss.coll().empty()) {
                        // We want to handle database only namespaces.
                        continue;
                    }

                    bool shouldRestore =
                        databasesRestored.find(dbNss.db().toString()) != databasesRestored.end();

                    LOGV2_DEBUG(6261305,
                                1,
                                "Found document",
                                "doc"_attr = doc,
                                "shouldRestore"_attr = shouldRestore,
                                logAttrs(coll->ns().dbName()),
                                "dbNss"_attr = dbNss);

                    if (shouldRestore) {
                        // This database had at least one collection restored.
                        continue;
                    }

                    // No collection for this database was restored, delete it.
                    LOGV2_DEBUG(6938703,
                                1,
                                "Deleting database that was not restored",
                                logAttrs(coll->ns().dbName()),
                                "uuid"_attr = coll->uuid(),
                                "_id"_attr = doc.getField("_id"));
                    NamespaceStringOrUUID nssOrUUID(coll->ns().db().toString(), coll->uuid());
                    uassertStatusOK(repl::StorageInterface::get(opCtx)->deleteById(
                        opCtx, nssOrUUID, doc.getField("_id")));
                }
            }
        }

        return true;
    }

} runRestoreCmd;

}  // namespace
}  // namespace mongo
