// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * A parsed view and entry point to read and modify on-disk catalog metadata (_mdb_catalog) from
 * outside the 'storage' layer.
 */
namespace durable_catalog {

/**
 * Scans the persisted catalog until an entry is found matching 'nss'. If present, returns a parsed
 * version of the catalog entry.
 */
[[MONGO_MOD_PRIVATE]] boost::optional<CatalogEntry> scanForCatalogEntryByNss(
    OperationContext* opCtx, const NamespaceString& nss, const MDBCatalog* mdbCatalog);

/**
 * Scans the persisted catalog until an entry is found matching 'uuid'. If present, returns a parsed
 * version of the catalog entry.
 */
[[MONGO_MOD_PRIVATE]] boost::optional<CatalogEntry> scanForCatalogEntryByUUID(
    OperationContext* opCtx, const UUID& uuid, const MDBCatalog* mdbCatalog);

/**
 * Parses the _mdb_catalog entry object at `catalogId` to common types. Returns boost::none if it
 * doesn't exist or if the entry is the feature document.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] boost::optional<CatalogEntry> getParsedCatalogEntry(
    OperationContext* opCtx, const RecordId& catalogId, const MDBCatalog* mdbCatalog);

[[MONGO_MOD_NEEDS_REPLACEMENT]] boost::optional<CatalogEntry> parseCatalogEntry(
    const RecordId& catalogId, const BSONObj& obj);

/**
 * Updates the catalog entry for the collection 'nss' with the fields specified in 'md'. If
 * 'md.indexes' contains a new index entry, then this method generates a new index ident and
 * adds it to the catalog entry.
 *
 * If `indexIdents` is supplied, the `idxIdents` entry in the catalog entry is replaced with the
 * given object. If not, `idxIdents` must already contain idents for all indexes in the metadata.
 * Any idents for indexes not present in the metadata will be discarded.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void putMetaData(
    OperationContext* opCtx,
    const RecordId& catalogId,
    const durable_catalog::CatalogEntryMetaData& md,
    MDBCatalog* mdbCatalog,
    boost::optional<BSONObj> indexIdents = boost::none);

/**
 * Persists a new collection in the catalog. The caller takes ownership of the newly created
 * RecordStore for the collection.
 */
[[MONGO_MOD_PRIVATE]] StatusWith<std::unique_ptr<RecordStore>> createCollection(
    OperationContext* opCtx,
    const RecordId& catalogId,
    const NamespaceString& nss,
    const std::string& ident,
    const CollectionOptions& options,
    MDBCatalog* mdbCatalog,
    bool recordIdsReplicated = false);

[[MONGO_MOD_PRIVATE]] Status createIndex(OperationContext* opCtx,
                                         const RecordId& catalogId,
                                         const NamespaceString& nss,
                                         const CollectionOptions& collectionOptions,
                                         const IndexConfig& indexConfig,
                                         std::string_view ident);

/**
 * Import a collection by inserting the given metadata into the durable catalog and instructing
 * the storage engine to import the corresponding idents. The metadata object should be a valid
 * catalog entry and contain the following fields:
 * "md": A document representing the durable_catalog::CatalogEntryMetaData of the collection.
 * "idxIdent": A document containing {<index_name>: <index_ident>} pairs for all indexes.
 * "nss": NamespaceString of the collection being imported.
 * "ident": Ident of the collection file.
 *
 * On success, returns an ImportResult structure containing the RecordId which identifies the
 * new record store in the durable catalog, ownership of the new RecordStore and the UUID of the
 * collection imported.
 *
 * The collection must be locked in MODE_X when calling this function.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] ImportResult {
    ImportResult(RecordId catalogId, std::unique_ptr<RecordStore> rs, UUID uuid)
        : catalogId(std::move(catalogId)), rs(std::move(rs)), uuid(uuid) {}
    RecordId catalogId;
    std::unique_ptr<RecordStore> rs;
    UUID uuid;
};
[[MONGO_MOD_NEEDS_REPLACEMENT]] StatusWith<ImportResult> importCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& metadata,
    const BSONObj& storageMetadata,
    bool generateNewUUID,
    MDBCatalog* mdbCatalog,
    bool panicOnCorruptWtMetadata = true,
    bool repair = false);

[[MONGO_MOD_PRIVATE]] Status renameCollection(OperationContext* opCtx,
                                              const RecordId& catalogId,
                                              const NamespaceString& toNss,
                                              durable_catalog::CatalogEntryMetaData& md,
                                              MDBCatalog* mdbCatalog);

/**
 * Deletes the persisted collection catalog entry identified by 'catalogId'.
 *
 * Expects (invariants) that all of the index catalog entries have been removed already via
 * removeIndex.
 */
[[MONGO_MOD_PRIVATE]] Status dropCollection(OperationContext* opCtx,
                                            const RecordId& catalogId,
                                            MDBCatalog* mdbCatalog);

/**
 * Drops the provided ident and recreates it as empty for use in resuming an index build.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status dropAndRecreateIndexIdentForResume(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& options,
    const IndexConfig& indexConfig,
    std::string_view ident);

[[MONGO_MOD_PRIVATE]] void getReadyIndexes(OperationContext* opCtx,
                                           RecordId catalogId,
                                           StringSet* names,
                                           const MDBCatalog* mdbCatalog);

[[MONGO_MOD_PRIVATE]] bool isIndexPresent(OperationContext* opCtx,
                                          const RecordId& catalogId,
                                          std::string_view indexName,
                                          const MDBCatalog* mdbCatalog);

[[MONGO_MOD_NEEDS_REPLACEMENT]] void sanitizeTimeseriesOptions(
    OperationContext* opCtx, durable_catalog::CatalogEntryMetaData& metadata);

/**
 * Holds functions for internal use from within the 'durable_catalog'. Exposed externally for
 * testing purposes.
 */
namespace internal {
/**
 * Generates 'durable_catalog::CatalogEntryMetaData' from 'collectionOptions' and
 * 'recordIdsReplicated'. Assumes it is for a new collection without no indexes present.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] durable_catalog::CatalogEntryMetaData
createMetaDataForNewCollection(const NamespaceString& nss,
                               const CollectionOptions& collectionOptions,
                               bool recordIdsReplicated = false);

[[MONGO_MOD_NEEDS_REPLACEMENT]] BSONObj buildRawMDBCatalogEntry(
    const std::string& ident,
    const BSONObj& idxIdent,
    const durable_catalog::CatalogEntryMetaData& md,
    const NamespaceString& nss);

}  // namespace internal

}  // namespace durable_catalog
}  // namespace mongo
