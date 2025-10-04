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

#include "mongo/db/local_catalog/database_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/audit.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/index_builds/index_build_block.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_options_gen.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "mongo/db/local_catalog/uncommitted_catalog_updates.h"
#include "mongo/db/local_catalog/virtual_collection_impl.h"
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/system_index.h"
#include "mongo/db/timeseries/viewless_timeseries_collection_creation_helpers.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(throwWCEDuringTxnCollCreate);
MONGO_FAIL_POINT_DEFINE(hangBeforeLoggingCreateCollection);
MONGO_FAIL_POINT_DEFINE(hangAfterParsingValidator);
MONGO_FAIL_POINT_DEFINE(overrideRecordIdsReplicatedDefault);
MONGO_FAIL_POINT_DEFINE(hangAndFailAfterCreateCollectionReservesOpTime);
MONGO_FAIL_POINT_DEFINE(openCreateCollectionWindowFp);
// Allows creating a buckets NS without timeseries options, as could ocurr on FCV 7.x and earlier,
// for example due to SERVER-87678, or due to a drop concurrent to direct inserts on the buckets NS.
MONGO_FAIL_POINT_DEFINE(skipCreateTimeseriesBucketsWithoutOptionsCheck);


Status validateDBNameForWindows(StringData dbname) {
    const std::vector<std::string> windowsReservedNames = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

    std::string lower{dbname};
    std::transform(
        lower.begin(), lower.end(), lower.begin(), [](char c) { return ctype::toLower(c); });

    if (std::count(windowsReservedNames.begin(), windowsReservedNames.end(), lower))
        return Status(ErrorCodes::BadValue,
                      str::stream() << "db name \"" << dbname << "\" is a reserved name");
    return Status::OK();
}

void assertNoMovePrimaryInProgress(OperationContext* opCtx, NamespaceString const& nss) {
    const auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquire(opCtx, nss.dbName());
    if (scopedDss->isMovePrimaryInProgress()) {
        LOGV2(4909100, "assertNoMovePrimaryInProgress", logAttrs(nss));

        uasserted(ErrorCodes::MovePrimaryInProgress,
                  "movePrimary is in progress for namespace " + nss.toStringForErrorMsg());
    }
}

RecordId acquireCatalogId(
    OperationContext* opCtx,
    const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
    MDBCatalog* mdbCatalog) {
    auto& rss = rss::ReplicatedStorageService::get(opCtx);
    if (rss.getPersistenceProvider().shouldUseReplicatedCatalogIdentifiers() &&
        createCollCatalogIdentifier.has_value()) {
        return createCollCatalogIdentifier->catalogId;
    }
    return mdbCatalog->reserveCatalogId(opCtx);
}

std::shared_ptr<Collection> makeCollectionInstance(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& collectionOptions,
    const CreateCollCatalogIdentifier& catalogIdentifier) {
    if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)) {
        throwWriteConflictException(str::stream() << "Namespace '" << nss.toStringForErrorMsg()
                                                  << "' is already in use.");
    }

    auto mdbCatalog = opCtx->getServiceContext()->getStorageEngine()->getMDBCatalog();
    const auto& catalogId = catalogIdentifier.catalogId;
    auto createResult = durable_catalog::createCollection(
        opCtx, catalogId, nss, catalogIdentifier.ident, collectionOptions, mdbCatalog);
    if (createResult == ErrorCodes::ObjectAlreadyExists) {
        // Each new ident must uniquely identify the collection's underlying table in the storage
        // engine. A scenario where the ident collides with a pre-existing ident should never happen
        // given it was just generated.
        LOGV2_FATAL(10173100,
                    "Generated ident cannot uniquely identify a new collection's storage table. "
                    "The ident maps to an occupied file",
                    "nss"_attr = nss.toStringForErrorMsg(),
                    "ident"_attr = catalogIdentifier.ident,
                    "catalogId"_attr = catalogId);
    }

    auto recordStore = uassertStatusOK(std::move(createResult));
    auto catalogEntry = durable_catalog::getParsedCatalogEntry(opCtx, catalogId, mdbCatalog);
    invariant(catalogEntry);
    auto& metadata = catalogEntry->metadata;
    return Collection::Factory::get(opCtx)->make(
        opCtx, nss, catalogId, metadata, std::move(recordStore));
}

// Acquires the final set of identifiers to use when creating a new collection in the catalog.
CreateCollCatalogIdentifier acquireCatalogIdentifierForCreate(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<CreateCollCatalogIdentifier>& providedIdentifier) {
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    CreateCollCatalogIdentifier catalogIdentifiers;

    if (providedIdentifier) {
        catalogIdentifiers.ident = providedIdentifier->ident;
        catalogIdentifiers.idIndexIdent = providedIdentifier->idIndexIdent;
    } else {
        catalogIdentifiers.ident = storageEngine->generateNewCollectionIdent(nss.dbName());
        catalogIdentifiers.idIndexIdent = storageEngine->generateNewIndexIdent(nss.dbName());
    }

    // The acquired catalogId can be different than one specified in the 'providedIdentifier' unless
    // disaggregated storage is enabled.
    catalogIdentifiers.catalogId =
        acquireCatalogId(opCtx, providedIdentifier, storageEngine->getMDBCatalog());
    return catalogIdentifiers;
}

}  // namespace

Status DatabaseImpl::validateDBName(const DatabaseName& dbName) {
    const auto dbname = dbName.serializeWithoutTenantPrefix_UNSAFE();
    if (dbname.size() <= 0)
        return Status(ErrorCodes::BadValue, "db name is empty");

    if (dbname.size() > DatabaseName::kMaxDatabaseNameLength)
        return Status(ErrorCodes::BadValue, "db name is too long");

    if (dbname.find('.') != std::string::npos)
        return Status(ErrorCodes::BadValue, "db name cannot contain a .");

    if (dbname.find(' ') != std::string::npos)
        return Status(ErrorCodes::BadValue, "db name cannot contain a space");

#ifdef _WIN32
    return validateDBNameForWindows(dbname);
#endif

    return Status::OK();
}

DatabaseImpl::DatabaseImpl(const DatabaseName& dbName)
    : _name(dbName), _viewsName(NamespaceString::makeSystemDotViewsNamespace(_name)) {}

void DatabaseImpl::init(OperationContext* const opCtx) {
    Status status = validateDBName(_name);

    if (!status.isOK()) {
        LOGV2_WARNING(20325, "Tried to open invalid db", logAttrs(_name));
        uasserted(10028, status.toString());
    }

    auto catalog = CollectionCatalog::get(opCtx);
    for (const auto& uuid : catalog->getAllCollectionUUIDsFromDb(_name)) {
        const Collection* collection = catalog->lookupCollectionByUUID(opCtx, uuid);
        invariant(collection);
        // If this is called from the repair path, the collection is already initialized.
        if (!collection->isInitialized()) {
            WriteUnitOfWork wuow(opCtx);
            CollectionWriter writer{opCtx, uuid};
            writer.getWritableCollection(opCtx)->init(opCtx);
            wuow.commit();
        }
    }

    // When in restore mode, views created on collections that weren't restored will be removed. We
    // only do this during startup when the global lock is held.
    if (storageGlobalParams.restore && shard_role_details::getLocker(opCtx)->isW()) {
        // Refresh our copy of the catalog, since we may have modified it above.
        catalog = CollectionCatalog::get(opCtx);
        try {
            struct ViewToDrop {
                NamespaceString name;
                NamespaceString viewOn;
                NamespaceString resolvedNs;
            };
            std::vector<ViewToDrop> viewsToDrop;
            catalog->iterateViews(opCtx, _name, [&](const ViewDefinition& view) {
                auto swResolvedView =
                    view_catalog_helpers::resolveView(opCtx, catalog, view.name(), boost::none);
                if (!swResolvedView.isOK()) {
                    LOGV2_WARNING(6260802,
                                  "Could not resolve view during restore",
                                  "view"_attr = view.name(),
                                  "viewOn"_attr = view.viewOn(),
                                  "reason"_attr = swResolvedView.getStatus().reason());
                    return true;
                }

                // The name of the most resolved namespace, which is a collection.
                auto resolvedNs = swResolvedView.getValue().getNamespace();

                if (catalog->lookupCollectionByNamespace(opCtx, resolvedNs)) {
                    // The collection exists for this view.
                    return true;
                }

                // Defer the view to drop so we don't do it while iterating. In case we're updating
                // the catalog inplace, we cannot modify the same data structure as we're iterating
                // on.
                viewsToDrop.push_back({view.name(), view.viewOn(), resolvedNs});

                return true;
            });

            // Drop all collected views from above.
            for (const auto& view : viewsToDrop) {
                LOGV2(6260803,
                      "Removing view on collection not restored",
                      "view"_attr = view.name,
                      "viewOn"_attr = view.viewOn,
                      "resolvedNs"_attr = view.resolvedNs);

                WriteUnitOfWork wuow(opCtx);
                Status status = catalog->dropView(opCtx, view.name);
                if (!status.isOK()) {
                    LOGV2_WARNING(6260804,
                                  "Failed to remove view on unrestored collection",
                                  "view"_attr = view.name,
                                  "viewOn"_attr = view.viewOn,
                                  "resolvedNs"_attr = view.resolvedNs,
                                  "reason"_attr = status.reason());
                    continue;
                }
                wuow.commit();
            }
        } catch (const ExceptionFor<ErrorCodes::InvalidViewDefinition>& e) {
            LOGV2_WARNING(6260805,
                          "Failed to access the view catalog during restore",
                          logAttrs(_name),
                          "reason"_attr = e.reason());
        }
    }
}

void DatabaseImpl::getStats(OperationContext* opCtx,
                            DBStats* output,
                            bool includeFreeStorage,
                            double scale) const {

    long long nCollections = 0;
    long long nViews = 0;
    long long objects = 0;
    long long size = 0;
    long long storageSize = 0;
    long long freeStorageSize = 0;
    long long indexes = 0;
    long long indexSize = 0;
    long long indexFreeStorageSize = 0;

    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(name(), MODE_IS));
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);

    catalog::forEachCollectionFromDb(
        opCtx, name(), MODE_IS, [&](const Collection* collection) -> bool {
            nCollections += 1;
            objects += collection->numRecords(opCtx);
            size += collection->dataSize(opCtx);

            BSONObjBuilder temp;
            storageSize += collection->getRecordStore()->storageSize(ru, &temp);

            indexes += collection->getIndexCatalog()->numIndexesTotal();
            indexSize += collection->getIndexSize(opCtx);

            if (includeFreeStorage) {
                freeStorageSize += collection->getRecordStore()->freeStorageSize(ru);
                indexFreeStorageSize += collection->getIndexFreeStorageBytes(opCtx);
            }

            return true;
        });


    CollectionCatalog::get(opCtx)->iterateViews(opCtx, name(), [&](const ViewDefinition& view) {
        nViews += 1;
        return true;
    });

    // Make sure that the same fields are returned for non-existing dbs
    // in `DBStats::errmsgRun`
    output->setCollections(nCollections);
    output->setViews(nViews);
    output->setObjects(objects);
    output->setAvgObjSize(objects ? (double(size) / double(objects)) : 0);
    output->setDataSize(size / scale);
    output->setStorageSize(storageSize / scale);

    output->setIndexes(indexes);
    output->setIndexSize(indexSize / scale);
    output->setTotalSize((storageSize + indexSize) / scale);
    if (includeFreeStorage) {
        output->setFreeStorageSize(freeStorageSize / scale);
        output->setIndexFreeStorageSize(indexFreeStorageSize / scale);
        output->setTotalFreeStorageSize((freeStorageSize + indexFreeStorageSize) / scale);
    }
    output->setScaleFactor(scale);

    if (!opCtx->getServiceContext()->getStorageEngine()->isEphemeral()) {
        boost::filesystem::path dbpath(
            opCtx->getServiceContext()->getStorageEngine()->getFilesystemPathForDb(_name));
        boost::system::error_code ec;
        boost::filesystem::space_info spaceInfo = boost::filesystem::space(dbpath, ec);
        if (!ec) {
            output->setFsUsedSize((spaceInfo.capacity - spaceInfo.available) / scale);
            output->setFsTotalSize(spaceInfo.capacity / scale);
        } else {
            output->setFsUsedSize(-1);
            output->setFsTotalSize(-1);
            LOGV2(20312,
                  "Failed to query filesystem disk stats",
                  "error"_attr = ec.message(),
                  "errorCode"_attr = ec.value());
        }
    }
}

Status DatabaseImpl::dropView(OperationContext* opCtx, NamespaceString viewName) const {
    dassert(shard_role_details::getLocker(opCtx)->isDbLockedForMode(name(), MODE_IX));
    dassert(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(viewName, MODE_IX));
    dassert(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
        NamespaceString(_viewsName), MODE_X));

    Status status = CollectionCatalog::get(opCtx)->dropView(opCtx, viewName);
    Top::getDecoration(opCtx).collectionDropped(viewName);
    return status;
}

Status DatabaseImpl::dropCollection(OperationContext* opCtx,
                                    NamespaceString nss,
                                    repl::OpTime dropOpTime,
                                    bool markFromMigrate) const {
    // Cannot drop uncommitted collections.
    invariant(!UncommittedCatalogUpdates::isCreatedCollection(opCtx, nss));

    auto catalog = CollectionCatalog::get(opCtx);

    if (!catalog->lookupCollectionByNamespace(opCtx, nss)) {
        // Collection doesn't exist so don't bother validating if it can be dropped.
        return Status::OK();
    }

    invariant(nss.dbName() == _name);

    if (auto droppable = isDroppableCollection(opCtx, nss); !droppable.isOK()) {
        return droppable;
    }

    assertNoMovePrimaryInProgress(opCtx, nss);

    return dropCollectionEvenIfSystem(opCtx, nss, dropOpTime, markFromMigrate);
}

Status DatabaseImpl::dropCollectionEvenIfSystem(OperationContext* opCtx,
                                                NamespaceString nss,
                                                repl::OpTime dropOpTime,
                                                bool markFromMigrate) const {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X));

    LOGV2_DEBUG(20313, 1, "dropCollection", logAttrs(nss));

    // A valid 'dropOpTime' is not allowed when writes are replicated.
    if (!dropOpTime.isNull() && opCtx->writesAreReplicated()) {
        return Status(
            ErrorCodes::BadValue,
            "dropCollection() cannot accept a valid drop optime when writes are replicated.");
    }

    CollectionWriter collection(opCtx, nss);

    if (!collection) {
        return Status::OK();  // Post condition already met.
    }

    auto numRecords = collection->numRecords(opCtx);

    auto uuid = collection->uuid();

    // Make sure no indexes builds are in progress.
    // Use massert() to be consistent with IndexCatalog::dropAllIndexes().
    auto numIndexesInProgress = collection->getIndexCatalog()->numIndexesInProgress();
    massert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            str::stream() << "cannot drop collection " << nss.toStringForErrorMsg() << " (" << uuid
                          << ") when " << numIndexesInProgress << " index builds in progress.",
            numIndexesInProgress == 0);

    audit::logDropCollection(opCtx->getClient(), nss);

    auto serviceContext = opCtx->getServiceContext();
    Top::getDecoration(opCtx).collectionDropped(nss);

    // Drop unreplicated collections immediately.
    // If 'dropOpTime' is provided, we should proceed to rename the collection.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto opObserver = serviceContext->getOpObserver();
    auto isOplogDisabledForNamespace = replCoord->isOplogDisabledFor(opCtx, nss);
    if (dropOpTime.isNull() && isOplogDisabledForNamespace) {
        _dropCollectionIndexes(opCtx, nss, collection.getWritableCollection(opCtx));
        opObserver->onDropCollection(opCtx, nss, uuid, numRecords, markFromMigrate);
        return _finishDropCollection(opCtx, nss, collection.getWritableCollection(opCtx));
    }

    // Replicated collections should be dropped in two phases.

    // New two-phase drop: Starting in 4.2, pending collection drops will be maintained in the
    // storage engine and will no longer be visible at the catalog layer with 3.6-style
    // <db>.system.drop.* namespaces.
    _dropCollectionIndexes(opCtx, nss, collection.getWritableCollection(opCtx));

    auto commitTimestamp = shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp();
    LOGV2(20314,
          "dropCollection: storage engine will take ownership of drop-pending "
          "collection",
          logAttrs(nss),
          "uuid"_attr = uuid,
          "dropOpTime"_attr = dropOpTime,
          "commitTimestamp"_attr = commitTimestamp);
    if (dropOpTime.isNull()) {
        // Log oplog entry for collection drop and remove the UUID.
        dropOpTime = opObserver->onDropCollection(opCtx, nss, uuid, numRecords, markFromMigrate);
        invariant(!dropOpTime.isNull());
    } else {
        // If we are provided with a valid 'dropOpTime', it means we are dropping this
        // collection in the context of applying an oplog entry on a secondary.
        auto opTime = opObserver->onDropCollection(opCtx, nss, uuid, numRecords, markFromMigrate);
        // OpObserver::onDropCollection should not be writing to the oplog on the secondary.
        invariant(opTime.isNull(),
                  str::stream() << "OpTime is not null. OpTime: " << opTime.toString());
    }

    return _finishDropCollection(opCtx, nss, collection.getWritableCollection(opCtx));
}

void DatabaseImpl::_dropCollectionIndexes(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          Collection* collection) const {
    invariant(_name == nss.dbName());
    LOGV2_DEBUG(20316, 1, "dropCollection: {namespace} - dropAllIndexes start", logAttrs(nss));
    collection->getIndexCatalog()->dropAllIndexes(opCtx, collection, true, {});

    invariant(collection->getTotalIndexCount() == 0);
    LOGV2_DEBUG(20317, 1, "dropCollection: {namespace} - dropAllIndexes done", logAttrs(nss));
}

Status DatabaseImpl::_finishDropCollection(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           Collection* collection) const {
    UUID uuid = collection->uuid();

    // Reduce log verbosity for virtual collections
    auto debugLevel = collection->getSharedIdent() ? 0 : 1;

    LOGV2_DEBUG(20318, debugLevel, "Finishing collection drop", logAttrs(nss), "uuid"_attr = uuid);

    // A virtual collection does not have a durable catalog entry.
    if (auto sharedIdent = collection->getSharedIdent()) {
        auto status = catalog::dropCollection(
            opCtx, collection->ns(), collection->getCatalogId(), sharedIdent);
        if (!status.isOK())
            return status;
    }

    CollectionCatalog::get(opCtx)->dropCollection(opCtx, collection);

    return Status::OK();
}

Status DatabaseImpl::renameCollection(OperationContext* opCtx,
                                      NamespaceString fromNss,
                                      NamespaceString toNss,
                                      bool stayTemp) const {
    audit::logRenameCollection(opCtx->getClient(), fromNss, toNss);

    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(fromNss, MODE_X));
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(toNss, MODE_X));

    invariant(fromNss.dbName() == _name);
    invariant(toNss.dbName() == _name);
    if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, toNss)) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "Cannot rename '" << fromNss.toStringForErrorMsg()
                                    << "' to '" << toNss.toStringForErrorMsg()
                                    << "' because the destination namespace already exists");
    }

    CollectionWriter collToRename(opCtx, fromNss);
    if (!collToRename) {
        return Status(ErrorCodes::NamespaceNotFound, "collection not found to rename");
    }

    assertNoMovePrimaryInProgress(opCtx, fromNss);

    LOGV2(20319,
          "renameCollection",
          "uuid"_attr = collToRename->uuid(),
          "fromName"_attr = fromNss,
          "toName"_attr = toNss);

    Top::getDecoration(opCtx).collectionDropped(fromNss);

    // Set the namespace of 'collToRename' from within the CollectionCatalog. This is necessary
    // because the CollectionCatalog manages the necessary isolation for this Collection until the
    // WUOW commits.
    auto writableCollection = collToRename.getWritableCollection(opCtx);
    Status status = writableCollection->rename(opCtx, toNss, stayTemp);
    if (!status.isOK())
        return status;

    CollectionCatalog::get(opCtx)->onCollectionRename(opCtx, writableCollection, fromNss);
    return status;
}

void DatabaseImpl::_checkCanCreateCollection(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& options) const {
    if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)) {
        if (options.isView()) {
            uasserted(ErrorCodes::NamespaceExists,
                      str::stream() << "Cannot create collection " << nss.toStringForErrorMsg()
                                    << " - collection already exists.");
        } else {
            throwWriteConflictException(str::stream()
                                        << "Collection namespace '" << nss.toStringForErrorMsg()
                                        << "' is already in use.");
        }
    }

    if (MONGO_unlikely(throwWCEDuringTxnCollCreate.shouldFail()) &&
        opCtx->inMultiDocumentTransaction()) {
        LOGV2(4696600,
              "Throwing WriteConflictException due to failpoint 'throwWCEDuringTxnCollCreate'");
        throwWriteConflictException(str::stream() << "Hit failpoint '"
                                                  << throwWCEDuringTxnCollCreate.getName() << "'.");
    }

    uassert(28838, "cannot create a non-capped oplog collection", options.capped || !nss.isOplog());
    uassert(ErrorCodes::DatabaseDropPending,
            str::stream() << "Cannot create collection " << nss.toStringForErrorMsg()
                          << " - database is in the process of being dropped.",
            !CollectionCatalog::get(opCtx)->isDropPending(nss.dbName()));
}

Status DatabaseImpl::createView(OperationContext* opCtx,
                                const NamespaceString& viewName,
                                const CollectionOptions& options) const {
    dassert(shard_role_details::getLocker(opCtx)->isDbLockedForMode(name(), MODE_IX));
    dassert(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(viewName, MODE_IX));
    dassert(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
        NamespaceString(_viewsName), MODE_X));

    invariant(options.isView());

    const auto viewOnNss = NamespaceStringUtil::deserialize(viewName.dbName(), options.viewOn);
    _checkCanCreateCollection(opCtx, viewName, options);

    BSONArray pipeline(options.pipeline);
    auto status = Status::OK();
    if (viewName.isOplog()) {
        status = {ErrorCodes::InvalidNamespace,
                  str::stream() << "invalid namespace name for a view: " +
                          viewName.toStringForErrorMsg()};
    } else {
        status = CollectionCatalog::get(opCtx)->createView(opCtx,
                                                           viewName,
                                                           viewOnNss,
                                                           pipeline,
                                                           view_catalog_helpers::validatePipeline,
                                                           options.collation);
    }

    audit::logCreateView(opCtx->getClient(), viewName, viewOnNss, pipeline, status.code());
    return status;
}

Collection* DatabaseImpl::createCollection(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const CollectionOptions& options,
                                           bool createIdIndex,
                                           const BSONObj& idIndex,
                                           bool fromMigrate) const {

    return _createCollection(
        opCtx,
        nss,
        options,
        acquireCatalogIdentifierForCreate(opCtx, nss, /*providedIdentifier=*/boost::none),
        createIdIndex,
        idIndex,
        fromMigrate);
}

Collection* DatabaseImpl::createVirtualCollection(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const CollectionOptions& opts,
                                                  const VirtualCollectionOptions& vopts) const {
    return _createCollection(opCtx,
                             nss,
                             opts,
                             vopts,
                             /*createIdIndex=*/false,
                             /*idIndex=*/BSONObj(),
                             /*fromMigrate=*/false);
}

Collection* DatabaseImpl::_createCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& options,
    const std::variant<VirtualCollectionOptions, CreateCollCatalogIdentifier>&
        voptsOrCatalogIdentifier,
    bool createIdIndex,
    const BSONObj& idIndex,
    bool fromMigrate) const {
    invariant(!options.isView());

    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX));

    auto coordinator = repl::ReplicationCoordinator::get(opCtx);
    bool canAcceptWrites =
        (!coordinator->getSettings().isReplSet()) || coordinator->canAcceptWritesFor(opCtx, nss);

    CollectionOptions optionsWithUUID = options;
    bool generatedUUID = false;
    if (!optionsWithUUID.uuid) {
        if (!canAcceptWrites) {
            LOGV2_ERROR_OPTIONS(20329,
                                {logv2::UserAssertAfterLog(ErrorCodes::InvalidOptions)},
                                "Attempted to create a new collection without a UUID",
                                logAttrs(nss));
        } else {
            optionsWithUUID.uuid.emplace(UUID::gen());
            generatedUUID = true;
        }
    }

    // Because writing the oplog entry depends on having the full spec for the _id index, which is
    // not available until the collection is actually created, we can't write the oplog entry until
    // after we have created the collection.  In order to make the storage timestamp for the
    // collection create always correct even when other operations are present in the same storage
    // transaction, we reserve an opTime before the collection creation, then pass it to the
    // opObserver.  Reserving the optime automatically sets the storage timestamp.
    // In order to ensure isolation of multi-document transactions, createCollection should only
    // reserve oplog slots here if it is run outside of a multi-document transaction. Multi-
    // document transactions reserve the appropriate oplog slots at commit time.
    OplogSlot createOplogSlot;
    if (canAcceptWrites && !coordinator->isOplogDisabledFor(opCtx, nss) &&
        !opCtx->inMultiDocumentTransaction()) {
        createOplogSlot = repl::getNextOpTime(opCtx);
    }

    // If we generated a UUID for the collection, then it MUST be the case
    // that we are creating the collection for the first time - i.e. the collection
    // isn't being copied over / migrated as in initial sync or a chunk migration.
    //
    // Therefore, since we are sure that we are creating the collection for the first
    // time, set recordIdsReplicated:true on all non-internal collections. We don't set
    // recordIdsReplicated:true on internal collections because many of these collections
    // are partially implicitly replicated (for example, on config.image_collection inserts
    // are implicitly replicated while deletes are not) and this makes it hard to ensure
    // that they have the same recordIds.
    //
    // Additionally, we do not set the recordIdsReplicated:true option on timeseries and
    // clustered collections because in those cases the recordId is the _id, or on capped
    // collections which utilizes a separate mechanism for ensuring uniform recordIds.
    const bool collectionTypeSupportsReplicatedRecordIds =
        !optionsWithUUID.timeseries && !optionsWithUUID.clusteredIndex && !optionsWithUUID.capped;
    const auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    if (generatedUUID && !nss.isOnInternalDb() && collectionTypeSupportsReplicatedRecordIds &&
        gFeatureFlagRecordIdsReplicated.isEnabledUseLastLTSFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        overrideRecordIdsReplicatedDefault.shouldFail()) {
        LOGV2_DEBUG(8700501,
                    0,
                    "Collection will use recordIdsReplicated:true.",
                    "oldValue"_attr = optionsWithUUID.recordIdsReplicated);
        optionsWithUUID.recordIdsReplicated = true;
    } else if (provider.shouldUseReplicatedRecordIds() && nss.isReplicated() &&
               !nss.isImplicitlyReplicated() && collectionTypeSupportsReplicatedRecordIds) {
        tassert(10985561,
                str::stream() << "Replicated record IDs must be enabled with " << provider.name(),
                gFeatureFlagRecordIdsReplicated.isEnabledUseLatestFCVWhenUninitialized(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
        LOGV2_DEBUG(10985560,
                    2,
                    "Collection will use recordIdsReplicated:true",
                    "provider"_attr = provider.name(),
                    "oldValue"_attr = optionsWithUUID.recordIdsReplicated);
        optionsWithUUID.recordIdsReplicated = true;
    }

    uassert(ErrorCodes::CommandNotSupported,
            fmt::format("Capped collection '{}' can't use recordIdsReplicated:true",
                        nss.toStringForErrorMsg()),
            !(optionsWithUUID.recordIdsReplicated && optionsWithUUID.capped));

    hangAndFailAfterCreateCollectionReservesOpTime.executeIf(
        [&](const BSONObj&) {
            hangAndFailAfterCreateCollectionReservesOpTime.pauseWhileSet(opCtx);
            uasserted(51267, "hangAndFailAfterCreateCollectionReservesOpTime fail point enabled");
        },
        [&](const BSONObj& data) {
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "nss"_sd);
            return fpNss.isEmpty() || fpNss == nss;
        });

    _checkCanCreateCollection(opCtx, nss, optionsWithUUID);
    assertNoMovePrimaryInProgress(opCtx, nss);
    audit::logCreateCollection(opCtx->getClient(), nss);

    // Reduce log verbosity for virtual collections
    int debugLevel = std::holds_alternative<VirtualCollectionOptions>(voptsOrCatalogIdentifier);

    LOGV2_DEBUG(20320,
                debugLevel,
                "createCollection",
                logAttrs(nss),
                "uuidDisposition"_attr = (generatedUUID ? "generated" : "provided"),
                "uuid"_attr = optionsWithUUID.uuid.value(),
                "options"_attr = options);

    boost::optional<CreateCollCatalogIdentifier> catalogIdentifierForColl;
    auto ownedCollection = std::visit(
        OverloadedVisitor{
            [&](const VirtualCollectionOptions& vopts) {
                return VirtualCollectionImpl::make(opCtx, nss, optionsWithUUID, vopts);
            },
            [&](const CreateCollCatalogIdentifier& catalogIdentifier) {
                catalogIdentifierForColl = catalogIdentifier;
                return makeCollectionInstance(opCtx, nss, optionsWithUUID, catalogIdentifier);
            }},
        voptsOrCatalogIdentifier);

    auto collection = ownedCollection.get();
    ownedCollection->init(opCtx);

    CollectionCatalog::get(opCtx)->onCreateCollection(opCtx, std::move(ownedCollection));
    openCreateCollectionWindowFp.executeIf(
        [&](const BSONObj& data) { sleepsecs(3); },
        [&](const BSONObj& data) {
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "collectionNS"_sd);
            return fpNss.isEmpty() || nss == fpNss;
        });

    BSONObj fullIdIndexSpec;

    if (createIdIndex && collection->requiresIdIndex()) {
        if (optionsWithUUID.autoIndexId != CollectionOptions::NO) {
            // Creating an index requires touching disk, which means there must be catalog
            // identifiers associated with the newly created collection.
            invariant(catalogIdentifierForColl);
            invariant(catalogIdentifierForColl->idIndexIdent);

            // The instance of the Collection is owned by this function and the initialization of
            // the CollectionPtr is therefore safe.
            auto collectionPtr = CollectionPtr::CollectionPtr_UNSAFE(collection);
            auto* ic = collection->getIndexCatalog();
            fullIdIndexSpec = uassertStatusOK(ic->prepareSpecForCreate(
                opCtx,
                collectionPtr,
                !idIndex.isEmpty() ? idIndex : ic->getDefaultIdIndexSpec(collectionPtr),
                boost::none));
            IndexBuildInfo indexBuildInfo(fullIdIndexSpec, catalogIdentifierForColl->idIndexIdent);
            indexBuildInfo.setInternalIdents(*opCtx->getServiceContext()->getStorageEngine(),
                                             VersionContext::getDecoration(opCtx));
            uassertStatusOK(IndexBuildBlock::buildEmptyIndex(opCtx, collection, indexBuildInfo));
        } else {
            // autoIndexId: false is only allowed on unreplicated collections.
            uassert(50001,
                    str::stream() << "autoIndexId:false is not allowed for collection "
                                  << nss.toStringForErrorMsg() << " because it can be replicated",
                    !nss.isReplicated());
        }
    }

    hangBeforeLoggingCreateCollection.pauseWhileSet();

    opCtx->getServiceContext()->getOpObserver()->onCreateCollection(opCtx,
                                                                    nss,
                                                                    optionsWithUUID,
                                                                    fullIdIndexSpec,
                                                                    createOplogSlot,
                                                                    catalogIdentifierForColl,
                                                                    fromMigrate);

    // It is necessary to create the system index *after* running the onCreateCollection so that
    // the storage timestamp for the index creation is after the storage timestamp for the
    // collection creation, and the opTimes for the corresponding oplog entries are the same as the
    // storage timestamps.  This way both primary and any secondaries will see the index created
    // after the collection is created.
    if (canAcceptWrites && createIdIndex && nss.isSystem()) {
        CollectionWriter collWriter(collection);
        createSystemIndexes(opCtx, collWriter, fromMigrate);
    }

    return collection;
}

StatusWith<std::unique_ptr<CollatorInterface>> DatabaseImpl::validateCollator(
    OperationContext* opCtx, CollectionOptions& opts) const {
    std::unique_ptr<CollatorInterface> collator;
    if (!opts.collation.isEmpty()) {
        auto collatorWithStatus =
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(opts.collation);

        if (!collatorWithStatus.isOK()) {
            return collatorWithStatus.getStatus();
        }

        collator = std::move(collatorWithStatus.getValue());

        // If the collator factory returned a non-null collator, set the collation option to the
        // result of serializing the collator's spec back into BSON. We do this in order to fill in
        // all options that the user omitted.
        //
        // If the collator factory returned a null collator (representing the "simple" collation),
        // we simply unset the "collation" from the collection options. This ensures that
        // collections created on versions which do not support the collation feature have the same
        // format for representing the simple collation as collections created on this version.
        opts.collation = collator ? collator->getSpec().toBSON() : BSONObj();
    }

    return {std::move(collator)};
}

Status DatabaseImpl::userCreateNS(
    OperationContext* opCtx,
    const NamespaceString& nss,
    CollectionOptions collectionOptions,
    bool createDefaultIndexes,
    const BSONObj& idIndex,
    bool fromMigrate,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier) const {
    LOGV2_DEBUG(20324,
                1,
                "create collection {namespace} {collectionOptions}",
                logAttrs(nss),
                "collectionOptions"_attr = collectionOptions.toBSON());
    if (!NamespaceString::validCollectionComponent(nss))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid ns: " << nss.toStringForErrorMsg());

    // Validate the collation, if there is one.
    auto swCollator = validateCollator(opCtx, collectionOptions);
    if (!swCollator.isOK()) {
        return swCollator.getStatus();
    }

    if (nss.isTimeseriesBucketsCollection() && !collectionOptions.timeseries &&
        !MONGO_unlikely(skipCreateTimeseriesBucketsWithoutOptionsCheck.shouldFail())) {
        return Status(ErrorCodes::IllegalOperation,
                      "Creation of a timeseries bucket collection without timeseries "
                      "options is not allowed");
    }

    tassert(10619100,
            "Timeseries collections must have a clustered index",
            !collectionOptions.timeseries || collectionOptions.clusteredIndex);

    if (!collectionOptions.validator.isEmpty()) {
        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(opCtx)
                          .collator(std::move(swCollator.getValue()))
                          .ns(nss)
                          // The match expression parser needs to know that we're parsing an
                          // expression for a validator to apply some additional checks.
                          .isParsingCollectionValidator(true)
                          .build();

        // If the validation action is printing logs or the level is "moderate", or if the user has
        // defined some encrypted fields in the collection options, then disallow any encryption
        // keywords. This is to prevent any plaintext data from showing up in the logs.
        auto allowedFeatures = MatchExpressionParser::kDefaultSpecialFeatures;

        if (collectionOptions.validationAction == ValidationActionEnum::warn ||
            collectionOptions.validationAction == ValidationActionEnum::errorAndLog ||
            collectionOptions.validationLevel == ValidationLevelEnum::moderate ||
            collectionOptions.encryptedFieldConfig.has_value())
            allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

        auto statusWithMatcher = MatchExpressionParser::parse(collectionOptions.validator,
                                                              std::move(expCtx),
                                                              ExtensionsCallbackNoop(),
                                                              allowedFeatures);

        // Increment counters to track the usage of schema validators.
        validatorCounters.incrementCounters(
            "create", collectionOptions.validator, statusWithMatcher.isOK());

        // We check the status of the parse to see if there are any banned features, but we don't
        // actually need the result for now.
        if (!statusWithMatcher.isOK()) {
            return statusWithMatcher.getStatus();
        }

        hangAfterParsingValidator.pauseWhileSet();
    }

    Status status = validateStorageOptions(
        opCtx->getServiceContext(),
        collectionOptions.storageEngine,
        [](const auto& x, const auto& y) { return x->validateCollectionStorageOptions(y); });

    if (!status.isOK())
        return status;

    if (auto storageEngineOptions = collectionOptions.indexOptionDefaults.getStorageEngine()) {
        status = validateStorageOptions(
            opCtx->getServiceContext(), *storageEngineOptions, [](const auto& x, const auto& y) {
                return x->validateIndexStorageOptions(y);
            });

        if (!status.isOK()) {
            return status;
        }
    }

    if (collectionOptions.isView()) {
        if (nss.isSystem())
            return Status(
                ErrorCodes::InvalidNamespace,
                "View name cannot start with 'system.', which is reserved for system namespaces");
        return createView(opCtx, nss, collectionOptions);
    } else {
        invariant(
            _createCollection(opCtx,
                              nss,
                              collectionOptions,
                              acquireCatalogIdentifierForCreate(opCtx, nss, catalogIdentifier),
                              createDefaultIndexes,
                              idIndex,
                              fromMigrate),
            str::stream() << "Collection creation failed after validating options: "
                          << nss.toStringForErrorMsg()
                          << ". Options: " << collectionOptions.toBSON());
    }

    return Status::OK();
}

Status DatabaseImpl::userCreateVirtualNS(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         CollectionOptions opts,
                                         const VirtualCollectionOptions& vopts) const {
    LOGV2_DEBUG(6968505,
                1,
                "create collection {namespace} {collectionOptions}",
                logAttrs(nss),
                "collectionOptions"_attr = opts.toBSON());
    if (!NamespaceString::validCollectionComponent(nss))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid ns: " << nss.toStringForErrorMsg());

    // Validate the collation, if there is one.
    if (auto swCollator = validateCollator(opCtx, opts); !swCollator.isOK()) {
        return swCollator.getStatus();
    }

    invariant(_createCollection(opCtx,
                                nss,
                                opts,
                                vopts,
                                /*createDefaultIndexes=*/false,
                                /*idIndex=*/BSONObj(),
                                /*fromMigrate=*/false),
              str::stream() << "Collection creation failed after validating options: "
                            << nss.toStringForErrorMsg() << ". Options: " << opts.toBSON());

    return Status::OK();
}

}  // namespace mongo
