/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/import_options.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/durable_catalog_entry.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {

class StorageEngineInterface;

/**
 * An interface to modify the on-disk catalog metadata.
 */
class DurableCatalog final {
    DurableCatalog(const DurableCatalog&) = delete;
    DurableCatalog& operator=(const DurableCatalog&) = delete;
    DurableCatalog(DurableCatalog&&) = delete;
    DurableCatalog& operator=(DurableCatalog&&) = delete;

public:
    static constexpr auto kIsFeatureDocumentFieldName = "isFeatureDoc"_sd;

    /**
     * `Entry` ties together the common identifiers of a single `_mdb_catalog` document.
     *
     * Idents can come in 4 forms depending on server parameters:
     * wtdfi    = --wiredTigerDirectoryForIndexes
     * dirperdb = --directoryperdb
     *
     * default:          <collection|index>-<counter>-<random number>
     * dirperdb:         <db>/<collection|index>-<counter>-<random number>
     * wtdfi:            <collection|index>/<counter>-<random number>
     * dirperdb & wtdfi: <db>/<collection|index>/<counter>-<random number>
     */
    struct EntryIdentifier {
        EntryIdentifier() {}
        EntryIdentifier(RecordId catalogId, std::string ident, NamespaceString nss)
            : catalogId(std::move(catalogId)), ident(std::move(ident)), nss(std::move(nss)) {}
        RecordId catalogId;
        std::string ident;
        NamespaceString nss;
    };

    DurableCatalog(RecordStore* rs,
                   bool directoryPerDb,
                   bool directoryForIndexes,
                   StorageEngineInterface* engine);
    DurableCatalog() = delete;


    static DurableCatalog* get(OperationContext* opCtx) {
        return opCtx->getServiceContext()->getStorageEngine()->getCatalog();
    }

    /**
     *  Allows featureDocuments to be checked with older versions.
     */
    static bool isFeatureDocument(const BSONObj& obj) {
        BSONElement firstElem = obj.firstElement();
        if (firstElem.fieldNameStringData() == kIsFeatureDocumentFieldName) {
            return firstElem.booleanSafe();
        }
        return false;
    }

    static bool isUserDataIdent(StringData ident) {
        // Indexes and collections are candidates for dropping when the storage engine's metadata
        // does not align with the catalog metadata.
        return ident.find("index-") != std::string::npos ||
            ident.find("index/") != std::string::npos || isCollectionIdent(ident);
    }

    static bool isInternalIdent(StringData ident) {
        return ident.find(_kInternalIdentPrefix) != std::string::npos;
    }

    static bool isResumableIndexBuildIdent(StringData ident) {
        invariant(isInternalIdent(ident), ident.toString());
        return ident.find(_kResumableIndexBuildIdentStem) != std::string::npos;
    }

    static bool isCollectionIdent(StringData ident) {
        // Internal idents prefixed "internal-" should not be considered collections, because
        // they are not eligible for orphan recovery through repair.
        return ident.find("collection-") != std::string::npos ||
            ident.find("collection/") != std::string::npos;
    }

    void init(OperationContext* opCtx);

    std::vector<EntryIdentifier> getAllCatalogEntries(OperationContext* opCtx) const;

    /**
     * Scans the persisted catalog until an entry is found matching 'nss'.
     */
    boost::optional<DurableCatalogEntry> scanForCatalogEntryByNss(OperationContext* opCtx,
                                                                  const NamespaceString& nss) const;

    /**
     * Scans the persisted catalog until an entry is found matching 'uuid'.
     */
    boost::optional<DurableCatalogEntry> scanForCatalogEntryByUUID(OperationContext* opCtx,
                                                                   const UUID& uuid) const;

    EntryIdentifier getEntry(const RecordId& catalogId) const;

    /**
     * First tries to return the in-memory entry. If not found, e.g. when collection is dropped
     * after the provided timestamp, loads the entry from the persisted catalog at the provided
     * timestamp.
     */
    NamespaceString getNSSFromCatalog(OperationContext* opCtx, const RecordId& catalogId) const;

    std::string getIndexIdent(OperationContext* opCtx,
                              const RecordId& id,
                              StringData idxName) const;

    std::vector<std::string> getIndexIdents(OperationContext* opCtx, const RecordId& id) const;

    /**
     * Get a raw catalog entry for catalogId as BSON.
     */
    BSONObj getCatalogEntry(OperationContext* opCtx, const RecordId& catalogId) const {
        auto cursor = _rs->getCursor(opCtx);
        return _findEntry(*cursor, catalogId).getOwned();
    }

    /**
     * Parses the catalog entry object at `catalogId` to common types. Returns boost::none if it
     * doesn't exist or if the entry is the feature document.
     */
    boost::optional<DurableCatalogEntry> getParsedCatalogEntry(OperationContext* opCtx,
                                                               const RecordId& catalogId) const;

    /**
     * Helper which constructs a DurableCatalogEntry given 'catalogId' and 'obj'.
     */
    boost::optional<DurableCatalogEntry> parseCatalogEntry(const RecordId& catalogId,
                                                           const BSONObj& obj) const;

    /**
     * Updates the catalog entry for the collection 'nss' with the fields specified in 'md'. If
     * 'md.indexes' contains a new index entry, then this method generates a new index ident and
     * adds it to the catalog entry.
     */
    void putMetaData(OperationContext* opCtx,
                     const RecordId& id,
                     BSONCollectionCatalogEntry::MetaData& md);

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const;

    RecordStore* getRecordStore() {
        return _rs;
    }

    /**
     * Create an entry in the catalog for an orphaned collection found in the
     * storage engine. Return the generated ns of the collection.
     * Note that this function does not recreate the _id index on the for non-clustered collections
     * because it does not have access to index catalog.
     */
    StatusWith<std::string> newOrphanedIdent(OperationContext* opCtx,
                                             std::string ident,
                                             const CollectionOptions& optionsWithUUID);

    std::string getFilesystemPathForDb(const DatabaseName& dbName) const;

    /**
     * Generate an internal ident name.
     */
    std::string newInternalIdent() {
        return _newInternalIdent("");
    }

    /**
     * Generates a new unique identifier for a new "thing".
     * @param nss - the containing namespace
     * @param kind - what this "thing" is, likely collection or index
     *
     * Warning: It's only unique as far as we know without checking every file on disk, but it is
     * possible that this ident collides with an existing one.
     */
    std::string generateUniqueIdent(NamespaceString nss, const char* kind);

    /**
     * Generate an internal resumable index build ident name.
     */
    std::string newInternalResumableIndexBuildIdent() {
        return _newInternalIdent(_kResumableIndexBuildIdentStem);
    }

    /**
     * On success, returns the RecordId which identifies the new record store in the durable catalog
     * in addition to ownership of the new RecordStore.
     */
    StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> createCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& options,
        bool allocateDefaultSpace);

    Status createIndex(OperationContext* opCtx,
                       const RecordId& catalogId,
                       const NamespaceString& nss,
                       const CollectionOptions& collOptions,
                       const IndexDescriptor* spec);

    /**
     * Import a collection by inserting the given metadata into the durable catalog and instructing
     * the storage engine to import the corresponding idents. The metadata object should be a valid
     * catalog entry and contain the following fields:
     * "md": A document representing the BSONCollectionCatalogEntry::MetaData of the collection.
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
                                              const ImportOptions& importOptions);

    Status renameCollection(OperationContext* opCtx,
                            const RecordId& catalogId,
                            const NamespaceString& toNss,
                            BSONCollectionCatalogEntry::MetaData& md);

    /**
     * Deletes the persisted collection catalog entry identified by 'catalogId'.
     *
     * Expects (invariants) that all of the index catalog entries have been removed already via
     * removeIndex.
     */
    Status dropCollection(OperationContext* opCtx, const RecordId& catalogId);

    /**
     * Drops the provided ident and recreates it as empty for use in resuming an index build.
     */
    Status dropAndRecreateIndexIdentForResume(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const CollectionOptions& collOptions,
                                              const IndexDescriptor* spec,
                                              StringData ident);

    void getReadyIndexes(OperationContext* opCtx, RecordId catalogId, StringSet* names) const;

    bool isIndexPresent(OperationContext* opCtx,
                        const RecordId& catalogId,
                        StringData indexName) const;

    /**
     * Returns true if the index identified by 'indexName' is multikey, and returns false otherwise.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information in the catalog, then this
     * function sets 'multikeyPaths' as the path components that cause this index to be multikey.
     *
     * In particular, if this function returns false and the index supports tracking path-level
     * multikey information, then 'multikeyPaths' is initialized as a vector with size equal to the
     * number of elements in the index key pattern of empty sets.
     */
    bool isIndexMultikey(OperationContext* opCtx,
                         const RecordId& catalogId,
                         StringData indexName,
                         MultikeyPaths* multikeyPaths) const;

    void setRand_forTest(const std::string& rand) {
        stdx::lock_guard<Latch> lk(_randLock);
        _rand = rand;
    }

    std::string getRand_forTest() const {
        stdx::lock_guard<Latch> lk(_randLock);
        return _rand;
    }

private:
    static constexpr auto _kInternalIdentPrefix = "internal-"_sd;
    static constexpr auto _kResumableIndexBuildIdentStem = "resumable-index-build-"_sd;

    class AddIdentChange;

    friend class StorageEngineImpl;
    friend class DurableCatalogTest;
    friend class StorageEngineTest;

    /**
     * Finds the durable catalog entry using the provided RecordStore cursor.
     * The returned BSONObj is unowned and is only valid while the cursor is positioned.
     */
    BSONObj _findEntry(SeekableRecordCursor& cursor, const RecordId& catalogId) const;
    StatusWith<EntryIdentifier> _addEntry(OperationContext* opCtx,
                                          NamespaceString nss,
                                          const CollectionOptions& options);
    StatusWith<EntryIdentifier> _importEntry(OperationContext* opCtx,
                                             NamespaceString nss,
                                             const BSONObj& metadata);
    Status _removeEntry(OperationContext* opCtx, const RecordId& catalogId);

    std::shared_ptr<BSONCollectionCatalogEntry::MetaData> _parseMetaData(
        const BSONElement& mdElement) const;


    std::string _newInternalIdent(StringData identStem);

    std::string _newRand();

    /**
     * The '_randLock' must be passed in.
     */
    bool _hasEntryCollidingWithRand(WithLock) const;

    RecordStore* _rs;  // not owned
    const bool _directoryPerDb;
    const bool _directoryForIndexes;

    // Protects '_rand' and '_next'.
    mutable stdx::mutex _randLock;
    std::string _rand;
    unsigned long long _next;

    absl::flat_hash_map<RecordId, EntryIdentifier, RecordId::Hasher> _catalogIdToEntryMap;
    mutable stdx::mutex _catalogIdToEntryMapLock;

    StorageEngineInterface* const _engine;
};
}  // namespace mongo
