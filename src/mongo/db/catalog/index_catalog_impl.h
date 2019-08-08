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

#pragma once

#include <vector>

#include "mongo/db/catalog/index_catalog.h"

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class Client;
class Collection;

class IndexDescriptor;
struct InsertDeleteOptions;

/**
 * how many: 1 per Collection.
 * lifecycle: attached to a Collection.
 */
class IndexCatalogImpl : public IndexCatalog {
public:
    explicit IndexCatalogImpl(Collection* collection);
    ~IndexCatalogImpl() override;

    // must be called before used
    Status init(OperationContext* opCtx) override;

    bool ok() const override;

    // ---- accessors -----

    bool haveAnyIndexes() const override;
    bool haveAnyIndexesInProgress() const override;
    int numIndexesTotal(OperationContext* opCtx) const override;
    int numIndexesReady(OperationContext* opCtx) const override;
    int numIndexesInProgress(OperationContext* opCtx) const {
        return numIndexesTotal(opCtx) - numIndexesReady(opCtx);
    }

    /**
     * this is in "alive" until the Collection goes away
     * in which case everything from this tree has to go away.
     */

    bool haveIdIndex(OperationContext* opCtx) const override;

    /**
     * Returns the spec for the id index to create by default for this collection.
     */
    BSONObj getDefaultIdIndexSpec() const override;

    const IndexDescriptor* findIdIndex(OperationContext* opCtx) const override;

    /**
     * Find index by name.  The index name uniquely identifies an index.
     *
     * @return null if cannot find
     */
    const IndexDescriptor* findIndexByName(OperationContext* opCtx,
                                           StringData name,
                                           bool includeUnfinishedIndexes = false) const override;

    /**
     * Find index by matching key pattern and collation spec.  The key pattern and collation spec
     * uniquely identify an index.
     *
     * Collation is specified as a normalized collation spec as returned by
     * CollationInterface::getSpec.  An empty object indicates the simple collation.
     *
     * @return null if cannot find index, otherwise the index with a matching key pattern and
     * collation.
     */
    const IndexDescriptor* findIndexByKeyPatternAndCollationSpec(
        OperationContext* opCtx,
        const BSONObj& key,
        const BSONObj& collationSpec,
        bool includeUnfinishedIndexes = false) const override;

    /**
     * Find indexes with a matching key pattern, putting them into the vector 'matches'.  The key
     * pattern alone does not uniquely identify an index.
     *
     * Consider using 'findIndexByName' if expecting to match one index.
     */
    void findIndexesByKeyPattern(OperationContext* opCtx,
                                 const BSONObj& key,
                                 bool includeUnfinishedIndexes,
                                 std::vector<const IndexDescriptor*>* matches) const override;

    /**
     * Returns an index suitable for shard key range scans.
     *
     * This index:
     * - must be prefixed by 'shardKey', and
     * - must not be a partial index.
     * - must have the simple collation.
     *
     * If the parameter 'requireSingleKey' is true, then this index additionally must not be
     * multi-key.
     *
     * If no such index exists, returns NULL.
     */
    const IndexDescriptor* findShardKeyPrefixedIndex(OperationContext* opCtx,
                                                     const BSONObj& shardKey,
                                                     bool requireSingleKey) const override;

    void findIndexByType(OperationContext* opCtx,
                         const std::string& type,
                         std::vector<const IndexDescriptor*>& matches,
                         bool includeUnfinishedIndexes = false) const override;


    /**
     * Reload the index definition for 'oldDesc' from the CollectionCatalogEntry.  'oldDesc'
     * must be a ready index that is already registered with the index catalog.  Returns an
     * unowned pointer to the descriptor for the new index definition.
     *
     * Use this method to notify the IndexCatalog that the spec for this index has changed.
     *
     * It is invalid to dereference 'oldDesc' after calling this method.
     *
     * The caller must hold the collection X lock and ensure no index builds are in progress
     * on the collection.
     */
    const IndexDescriptor* refreshEntry(OperationContext* opCtx,
                                        const IndexDescriptor* oldDesc) override;

    const IndexCatalogEntry* getEntry(const IndexDescriptor* desc) const override;

    std::shared_ptr<const IndexCatalogEntry> getEntryShared(const IndexDescriptor*) const override;

    std::vector<std::shared_ptr<const IndexCatalogEntry>> getAllReadyEntriesShared() const override;

    using IndexIterator = IndexCatalog::IndexIterator;
    std::unique_ptr<IndexIterator> getIndexIterator(
        OperationContext* const opCtx, const bool includeUnfinishedIndexes) const override;

    // ---- index set modifiers ------

    IndexCatalogEntry* createIndexEntry(OperationContext* opCtx,
                                        std::unique_ptr<IndexDescriptor> descriptor,
                                        bool initFromDisk,
                                        bool isReadyIndex) override;

    /**
     * Call this only on an empty collection from inside a WriteUnitOfWork. Index creation on an
     * empty collection can be rolled back as part of a larger WUOW. Returns the full specification
     * of the created index, as it is stored in this index catalog.
     */
    StatusWith<BSONObj> createIndexOnEmptyCollection(OperationContext* opCtx,
                                                     BSONObj spec) override;

    StatusWith<BSONObj> prepareSpecForCreate(OperationContext* opCtx,
                                             const BSONObj& original) const override;

    std::vector<BSONObj> removeExistingIndexes(OperationContext* const opCtx,
                                               const std::vector<BSONObj>& indexSpecsToBuild,
                                               const bool removeIndexBuildsToo) const override;

    std::vector<BSONObj> removeExistingIndexesNoChecks(
        OperationContext* const opCtx,
        const std::vector<BSONObj>& indexSpecsToBuild) const override;

    /**
     * Drops all indexes in the index catalog, optionally dropping the id index depending on the
     * 'includingIdIndex' parameter value. If the 'droppedIndexes' parameter is not null,
     * it is filled with the names and index info of the dropped indexes.
     */
    void dropAllIndexes(OperationContext* opCtx,
                        bool includingIdIndex,
                        std::function<void(const IndexDescriptor*)> onDropFn) override;
    void dropAllIndexes(OperationContext* opCtx, bool includingIdIndex) override;


    Status dropIndex(OperationContext* opCtx, const IndexDescriptor* desc) override;


    Status dropIndexEntry(OperationContext* opCtx, IndexCatalogEntry* entry) override;


    void deleteIndexFromDisk(OperationContext* opCtx, const std::string& indexName) override;

    struct IndexKillCriteria {
        std::string ns;
        std::string name;
        BSONObj key;
    };

    // ---- modify single index

    /**
     * Returns true if the index 'idx' is multikey, and returns false otherwise.
     */
    bool isMultikey(OperationContext* opCtx, const IndexDescriptor* idx) override;

    /**
     * Returns the path components that cause the index 'idx' to be multikey if the index supports
     * path-level multikey tracking, and returns an empty vector if path-level multikey tracking
     * isn't supported.
     *
     * If the index supports path-level multikey tracking but isn't multikey, then this function
     * returns a vector with size equal to the number of elements in the index key pattern where
     * each element in the vector is an empty set.
     */
    MultikeyPaths getMultikeyPaths(OperationContext* opCtx, const IndexDescriptor* idx) override;

    void setMultikeyPaths(OperationContext* const opCtx,
                          const IndexDescriptor* desc,
                          const MultikeyPaths& multikeyPaths) override;

    // ----- data modifiers ------

    /**
     * When 'keysInsertedOut' is not null, it will be set to the number of index keys inserted by
     * this operation.
     *
     * This method may throw.
     */
    Status indexRecords(OperationContext* opCtx,
                        const std::vector<BsonRecord>& bsonRecords,
                        int64_t* keysInsertedOut) override;

    /**
     * See IndexCatalog::updateRecord
     */
    Status updateRecord(OperationContext* const opCtx,
                        const BSONObj& oldDoc,
                        const BSONObj& newDoc,
                        const RecordId& recordId,
                        int64_t* const keysInsertedOut,
                        int64_t* const keysDeletedOut) override;
    /**
     * When 'keysDeletedOut' is not null, it will be set to the number of index keys removed by
     * this operation.
     */
    void unindexRecord(OperationContext* opCtx,
                       const BSONObj& obj,
                       const RecordId& loc,
                       bool noWarn,
                       int64_t* keysDeletedOut) override;

    Status compactIndexes(OperationContext* opCtx) override;

    inline std::string getAccessMethodName(const BSONObj& keyPattern) override {
        return _getAccessMethodName(keyPattern);
    }

    std::string::size_type getLongestIndexNameLength(OperationContext* opCtx) const override;

    // public static helpers

    BSONObj fixIndexKey(const BSONObj& key) const override;

    /**
     * Fills out 'options' in order to indicate whether to allow dups or relax
     * index constraints, as needed by replication.
     */
    void prepareInsertDeleteOptions(OperationContext* opCtx,
                                    const IndexDescriptor* desc,
                                    InsertDeleteOptions* options) const override;

    void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) override;

private:
    static const BSONObj _idObj;  // { _id : 1 }

    /**
     * In addition to IndexNames::findPluginName, validates that it is a known index type.
     * If all you need is to check for a certain type, just use IndexNames::findPluginName.
     *
     * Uasserts if the index type is unknown.
     */
    std::string _getAccessMethodName(const BSONObj& keyPattern) const;

    void _checkMagic() const;

    Status _indexKeys(OperationContext* opCtx,
                      IndexCatalogEntry* index,
                      const std::vector<KeyString::Value>& keys,
                      const KeyStringSet& multikeyMetadataKeys,
                      const MultikeyPaths& multikeyPaths,
                      const BSONObj& obj,
                      RecordId loc,
                      const InsertDeleteOptions& options,
                      int64_t* keysInsertedOut);

    Status _indexFilteredRecords(OperationContext* opCtx,
                                 IndexCatalogEntry* index,
                                 const std::vector<BsonRecord>& bsonRecords,
                                 int64_t* keysInsertedOut);

    Status _indexRecords(OperationContext* opCtx,
                         IndexCatalogEntry* index,
                         const std::vector<BsonRecord>& bsonRecords,
                         int64_t* keysInsertedOut);

    Status _updateRecord(OperationContext* const opCtx,
                         IndexCatalogEntry* index,
                         const BSONObj& oldDoc,
                         const BSONObj& newDoc,
                         const RecordId& recordId,
                         int64_t* const keysInsertedOut,
                         int64_t* const keysDeletedOut);

    void _unindexKeys(OperationContext* opCtx,
                      IndexCatalogEntry* index,
                      const std::vector<KeyString::Value>& keys,
                      const BSONObj& obj,
                      RecordId loc,
                      bool logIfError,
                      int64_t* const keysDeletedOut);

    void _unindexRecord(OperationContext* opCtx,
                        IndexCatalogEntry* entry,
                        const BSONObj& obj,
                        const RecordId& loc,
                        bool logIfError,
                        int64_t* keysDeletedOut);

    /**
     * Applies a set of transformations to the user-provided index object 'spec' to make it
     * conform to the standard for insertion.  Removes the '_id' field if it exists, applies
     * plugin-level transformations if appropriate, etc.
     */
    StatusWith<BSONObj> _fixIndexSpec(OperationContext* opCtx,
                                      Collection* collection,
                                      const BSONObj& spec) const;

    Status _isSpecOk(OperationContext* opCtx, const BSONObj& spec) const;

    /**
     * Validates the 'original' index specification, alters any legacy fields and does plugin-level
     * transformations for text and geo indexes. Returns a clean spec ready to be built, or an
     * error.
     */
    StatusWith<BSONObj> _validateAndFixIndexSpec(OperationContext* opCtx,
                                                 const BSONObj& original) const;

    /**
     * Checks whether there are any spec conflicts with existing ready indexes or in-progress index
     * builds. Also checks whether any limits set on this server would be exceeded by building the
     * index. 'includeUnfinishedIndexes' dictates whether in-progress index builds are checked for
     * conflicts, along with ready indexes.
     *
     * Returns IndexAlreadyExists for both ready and in-progress index builds. Can also return other
     * errors.
     */
    Status _doesSpecConflictWithExisting(OperationContext* opCtx,
                                         const BSONObj& spec,
                                         const bool includeUnfinishedIndexes) const;

    /**
     * Returns true if the replica set member's config has {buildIndexes:false} set, which means
     * we are not allowed to build non-_id indexes on this server, AND this index spec is for a
     * non-_id index.
     */
    Status _isNonIDIndexAndNotAllowedToBuild(OperationContext* opCtx, const BSONObj& spec) const;

    void _logInternalState(OperationContext* opCtx,
                           long long numIndexesInCollectionCatalogEntry,
                           const std::vector<std::string>& indexNamesToDrop,
                           bool haveIdIndex);

    int _magic;
    Collection* const _collection;

    IndexCatalogEntryContainer _readyIndexes;
    IndexCatalogEntryContainer _buildingIndexes;
};
}  // namespace mongo
