/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/durable_catalog_entry.h"
#include "mongo/db/local_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
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
boost::optional<CatalogEntry> scanForCatalogEntryByNss(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const MDBCatalog* mdbCatalog);

/**
 * Scans the persisted catalog until an entry is found matching 'uuid'. If present, returns a parsed
 * version of the catalog entry.
 */
boost::optional<CatalogEntry> scanForCatalogEntryByUUID(OperationContext* opCtx,
                                                        const UUID& uuid,
                                                        const MDBCatalog* mdbCatalog);

/**
 * Parses the _mdb_catalog entry object at `catalogId` to common types. Returns boost::none if it
 * doesn't exist or if the entry is the feature document.
 */
boost::optional<CatalogEntry> getParsedCatalogEntry(OperationContext* opCtx,
                                                    const RecordId& catalogId,
                                                    const MDBCatalog* mdbCatalog);

boost::optional<CatalogEntry> parseCatalogEntry(const RecordId& catalogId, const BSONObj& obj);

/**
 * Updates the catalog entry for the collection 'nss' with the fields specified in 'md'. If
 * 'md.indexes' contains a new index entry, then this method generates a new index ident and
 * adds it to the catalog entry.
 *
 * If `indexIdents` is supplied, the `idxIdents` entry in the catalog entry is replaced with the
 * given object. If not, `idxIdents` must already contain idents for all indexes in the metadata.
 * Any idents for indexes not present in the metadata will be discarded.
 */
void putMetaData(OperationContext* opCtx,
                 const RecordId& catalogId,
                 durable_catalog::CatalogEntryMetaData& md,
                 MDBCatalog* mdbCatalog,
                 boost::optional<BSONObj> indexIdents = boost::none);

/**
 * Persists a new collection in the catalog. The caller takes ownership of the newly created
 * RecordStore for the collection.
 */
StatusWith<std::unique_ptr<RecordStore>> createCollection(OperationContext* opCtx,
                                                          const RecordId& catalogId,
                                                          const NamespaceString& nss,
                                                          const std::string& ident,
                                                          const CollectionOptions& options,
                                                          MDBCatalog* mdbCatalog);

Status createIndex(OperationContext* opCtx,
                   const RecordId& catalogId,
                   const NamespaceString& nss,
                   const CollectionOptions& collectionOptions,
                   const IndexConfig& indexConfig,
                   StringData ident);

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
struct ImportResult {
    ImportResult(RecordId catalogId, std::unique_ptr<RecordStore> rs, UUID uuid)
        : catalogId(std::move(catalogId)), rs(std::move(rs)), uuid(uuid) {}
    RecordId catalogId;
    std::unique_ptr<RecordStore> rs;
    UUID uuid;
};
StatusWith<ImportResult> importCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const BSONObj& metadata,
                                          const BSONObj& storageMetadata,
                                          bool generateNewUUID,
                                          MDBCatalog* mdbCatalog,
                                          bool panicOnCorruptWtMetadata = true,
                                          bool repair = false);

Status renameCollection(OperationContext* opCtx,
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
Status dropCollection(OperationContext* opCtx, const RecordId& catalogId, MDBCatalog* mdbCatalog);

/**
 * Drops the provided ident and recreates it as empty for use in resuming an index build.
 */
Status dropAndRecreateIndexIdentForResume(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const CollectionOptions& options,
                                          const IndexConfig& indexConfig,
                                          StringData ident);

void getReadyIndexes(OperationContext* opCtx,
                     RecordId catalogId,
                     StringSet* names,
                     const MDBCatalog* mdbCatalog);

bool isIndexPresent(OperationContext* opCtx,
                    const RecordId& catalogId,
                    StringData indexName,
                    const MDBCatalog* mdbCatalog);

/**
 * Holds functions for internal use from within the 'durable_catalog'. Exposed externally for
 * testing purposes.
 */
namespace internal {
/**
 * Generates 'durable_catalog::CatalogEntryMetaData' from 'collectionOptions'. Assumes it is for a
 * new collection without no indexes present.
 */
durable_catalog::CatalogEntryMetaData createMetaDataForNewCollection(
    const NamespaceString& nss, const CollectionOptions& collectionOptions);

BSONObj buildRawMDBCatalogEntry(const std::string& ident,
                                const BSONObj& idxIdent,
                                const durable_catalog::CatalogEntryMetaData& md,
                                const std::string& ns);

}  // namespace internal

}  // namespace durable_catalog
}  // namespace mongo
