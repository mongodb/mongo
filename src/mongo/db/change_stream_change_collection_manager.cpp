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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/change_stream_change_collection_manager.h"

#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/logv2/log.h"

namespace mongo {

// Sharded clusters do not support serverless mode at present, but this failpoint allows us to
// nonetheless test the behaviour of change collections in a sharded environment.
MONGO_FAIL_POINT_DEFINE(forceEnableChangeCollectionsMode);

namespace {
const auto getChangeCollectionManager =
    ServiceContext::declareDecoration<boost::optional<ChangeStreamChangeCollectionManager>>();

/**
 * Creates a Document object from the supplied oplog entry, performs necessary modifications to it
 * and then returns it as a BSON object.
 */
BSONObj createChangeCollectionEntryFromOplog(const BSONObj& oplogEntry) {
    Document oplogDoc(oplogEntry);
    MutableDocument changeCollDoc(oplogDoc);
    changeCollDoc["_id"] = Value(oplogDoc["ts"]);

    auto readyChangeCollDoc = changeCollDoc.freeze();
    return readyChangeCollDoc.toBson();
}

/**
 * Helper to write insert statements to respective change collections based on tenant ids.
 */
class ChangeCollectionsWriter {
public:
    /**
     * Adds the insert statement for the provided tenant that will be written to the change
     * collection when the 'write()' method is called.
     */
    void add(const TenantId& tenantId, InsertStatement insertStatement) {
        if (_shouldAddEntry(insertStatement)) {
            _tenantStatementsMap[tenantId].push_back(std::move(insertStatement));
        }
    }

    /**
     * Writes the batch of insert statements for each change collection. Bails out further writes if
     * a failure is encountered in writing to a any change collection.
     */
    Status write(OperationContext* opCtx, OpDebug* opDebug) {
        for (auto&& [tenantId, insertStatements] : _tenantStatementsMap) {
            AutoGetChangeCollection tenantChangeCollection(
                opCtx, AutoGetChangeCollection::AccessMode::kWrite, boost::none /* tenantId */);

            // The change collection does not exist for a particular tenant because either the
            // change collection is not enabled or is in the process of enablement. Ignore this
            // insert for now.
            // TODO: SERVER-65950 move this check before inserting to the map
            // 'tenantToInsertStatements'.
            if (!tenantChangeCollection) {
                continue;
            }

            // Writes to the change collection should not be replicated.
            repl::UnreplicatedWritesBlock unReplBlock(opCtx);

            Status status = tenantChangeCollection->insertDocuments(opCtx,
                                                                    insertStatements.begin(),
                                                                    insertStatements.end(),
                                                                    opDebug,
                                                                    false /* fromMigrate */);
            if (!status.isOK()) {
                return Status(status.code(),
                              str::stream()
                                  << "Write to change collection: " << tenantChangeCollection->ns()
                                  << "failed, reason: " << status.reason());
            }
        }

        return Status::OK();
    }

private:
    bool _shouldAddEntry(const InsertStatement& insertStatement) {
        auto& oplogDoc = insertStatement.doc;

        // TODO SERVER-65950 retreive tenant from the oplog.
        // TODO SERVER-67170 avoid inspecting the oplog BSON object.

        if (auto nssFieldElem = oplogDoc[repl::OplogEntry::kNssFieldName];
            nssFieldElem && nssFieldElem.String() == "config.$cmd"_sd) {
            if (auto objectFieldElem = oplogDoc[repl::OplogEntry::kObjectFieldName]) {
                // The oplog entry might be a drop command on the change collection. Check if the
                // drop request is for the already deleted change collection, as such do not attempt
                // to write to the change collection if that is the case. This scenario is possible
                // because 'WriteUnitOfWork' will stage the changes and while committing the staged
                // 'CollectionImpl::insertDocuments' change the collection object might have already
                // been deleted.
                if (auto dropFieldElem = objectFieldElem["drop"_sd]) {
                    return dropFieldElem.String() != NamespaceString::kChangeCollectionName;
                }
            }
        }

        return true;
    }

    // Maps inserts statements for each tenant.
    stdx::unordered_map<TenantId, std::vector<InsertStatement>, TenantId::Hasher>
        _tenantStatementsMap;
};

}  // namespace

ChangeStreamChangeCollectionManager& ChangeStreamChangeCollectionManager::get(
    ServiceContext* service) {
    return *getChangeCollectionManager(service);
}

ChangeStreamChangeCollectionManager& ChangeStreamChangeCollectionManager::get(
    OperationContext* opCtx) {
    return *getChangeCollectionManager(opCtx->getServiceContext());
}

void ChangeStreamChangeCollectionManager::create(ServiceContext* service) {
    getChangeCollectionManager(service).emplace(service);
}

bool ChangeStreamChangeCollectionManager::isChangeCollectionsModeActive() {
    // If the force fail point is enabled then declare the change collection mode as active.
    if (MONGO_unlikely(forceEnableChangeCollectionsMode.shouldFail())) {
        return true;
    }

    // TODO SERVER-67267 guard with 'multitenancySupport' and 'isServerless' flag.
    return feature_flags::gFeatureFlagServerlessChangeStreams.isEnabled(
        serverGlobalParams.featureCompatibility);
}

bool ChangeStreamChangeCollectionManager::hasChangeCollection(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) const {
    auto catalog = CollectionCatalog::get(opCtx);
    return static_cast<bool>(catalog->lookupCollectionByNamespace(
        opCtx, NamespaceString::makeChangeCollectionNSS(tenantId)));
}

Status ChangeStreamChangeCollectionManager::createChangeCollection(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    // Make the change collection clustered by '_id'. The '_id' field will have the same value as
    // the 'ts' field of the oplog.
    CollectionOptions changeCollectionOptions;
    changeCollectionOptions.clusteredIndex.emplace(clustered_util::makeDefaultClusteredIdIndex());
    changeCollectionOptions.capped = true;

    auto status = createCollection(opCtx,
                                   NamespaceString::makeChangeCollectionNSS(tenantId),
                                   changeCollectionOptions,
                                   BSONObj());
    if (status.code() == ErrorCodes::NamespaceExists) {
        return Status::OK();
    }

    return status;
}

Status ChangeStreamChangeCollectionManager::dropChangeCollection(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    DropReply dropReply;
    return dropCollection(opCtx,
                          NamespaceString::makeChangeCollectionNSS(tenantId),
                          &dropReply,
                          DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
}

void ChangeStreamChangeCollectionManager::insertDocumentsToChangeCollection(
    OperationContext* opCtx,
    const std::vector<Record>& oplogRecords,
    const std::vector<Timestamp>& oplogTimestamps) {
    invariant(oplogRecords.size() == oplogTimestamps.size());

    // This method must be called within a 'WriteUnitOfWork'. The caller must be responsible for
    // commiting the unit of work.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    ChangeCollectionsWriter changeCollectionsWriter;

    for (size_t idx = 0; idx < oplogRecords.size(); idx++) {
        auto& record = oplogRecords[idx];
        auto& ts = oplogTimestamps[idx];

        // Create an insert statement that should be written at the timestamp 'ts' for a particular
        // tenant.
        auto changeCollDoc = createChangeCollectionEntryFromOplog(record.data.toBson());

        // TODO SERVER-65950 replace 'TenantId::kSystemTenantId' with the tenant id.
        changeCollectionsWriter.add(
            TenantId::kSystemTenantId,
            InsertStatement{std::move(changeCollDoc), ts, repl::OpTime::kUninitializedTerm});
    }

    // Write documents to change collections and throw exception in case of any failure.
    Status status = changeCollectionsWriter.write(opCtx, nullptr /* opDebug */);
    if (!status.isOK()) {
        LOGV2_FATAL(
            6612300, "Failed to write to change collection", "reason"_attr = status.reason());
    }
}

Status ChangeStreamChangeCollectionManager::insertDocumentsToChangeCollection(
    OperationContext* opCtx,
    std::vector<InsertStatement>::const_iterator beginOplogEntries,
    std::vector<InsertStatement>::const_iterator endOplogEntries,
    OpDebug* opDebug) {
    ChangeCollectionsWriter changeCollectionsWriter;

    // Transform oplog entries to change collections entries and group them by tenant id.
    for (auto oplogEntryIter = beginOplogEntries; oplogEntryIter != endOplogEntries;
         oplogEntryIter++) {
        auto& oplogDoc = oplogEntryIter->doc;

        // The initial seed oplog insertion is not timestamped as such the 'oplogSlot' is not
        // initialized. The corresponding change collection insertion will not be timestamped.
        auto oplogSlot = oplogEntryIter->oplogSlot;

        auto changeCollDoc = createChangeCollectionEntryFromOplog(oplogDoc);

        // TODO SERVER-65950 replace 'TenantId::kSystemTenantId' with the tenant id.
        changeCollectionsWriter.add(TenantId::kSystemTenantId,
                                    InsertStatement{std::move(changeCollDoc),
                                                    oplogSlot.getTimestamp(),
                                                    oplogSlot.getTerm()});
    }

    // Write documents to change collections.
    return changeCollectionsWriter.write(opCtx, opDebug);
}

}  // namespace mongo
