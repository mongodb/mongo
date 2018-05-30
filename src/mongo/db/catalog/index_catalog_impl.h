/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
class IndexAccessMethod;
struct InsertDeleteOptions;

/**
 * how many: 1 per Collection.
 * lifecycle: attached to a Collection.
 */
class IndexCatalogImpl : public IndexCatalog::Impl {
public:
    explicit IndexCatalogImpl(IndexCatalog* this_,
                              Collection* collection,
                              int maxNumIndexesAllowed);
    ~IndexCatalogImpl() override;

    // must be called before used
    Status init(OperationContext* opCtx) override;

    bool ok() const override;

    // ---- accessors -----

    bool haveAnyIndexes() const override;
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

    IndexDescriptor* findIdIndex(OperationContext* opCtx) const override;

    /**
     * Find index by name.  The index name uniquely identifies an index.
     *
     * @return null if cannot find
     */
    IndexDescriptor* findIndexByName(OperationContext* opCtx,
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
    IndexDescriptor* findIndexByKeyPatternAndCollationSpec(
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
                                 std::vector<IndexDescriptor*>* matches) const override;

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
    IndexDescriptor* findShardKeyPrefixedIndex(OperationContext* opCtx,
                                               const BSONObj& shardKey,
                                               bool requireSingleKey) const override;

    void findIndexByType(OperationContext* opCtx,
                         const std::string& type,
                         std::vector<IndexDescriptor*>& matches,
                         bool includeUnfinishedIndexes = false) const override;


    /**
     * Reload the index definition for 'oldDesc' from the CollectionCatalogEntry.  'oldDesc'
     * must be a ready index that is already registered with the index catalog.  Returns an
     * unowned pointer to the descriptor for the new index definition.
     *
     * Use this method to notify the IndexCatalog that the spec for this index has changed.
     *
     * It is invalid to dereference 'oldDesc' after calling this method.  This method broadcasts
     * an invalidateAll() on the cursor manager to notify other users of the IndexCatalog that
     * this descriptor is now invalid.
     */
    const IndexDescriptor* refreshEntry(OperationContext* opCtx,
                                        const IndexDescriptor* oldDesc) override;

    // never returns NULL
    const IndexCatalogEntry* getEntry(const IndexDescriptor* desc) const override;

    IndexAccessMethod* getIndex(const IndexDescriptor* desc) override;
    const IndexAccessMethod* getIndex(const IndexDescriptor* desc) const override;

    /**
     * Returns a not-ok Status if there are any unfinished index builds. No new indexes should
     * be built when in this state.
     */
    Status checkUnfinished() const override;

    class IndexIteratorImpl : public IndexCatalog::IndexIterator::Impl {
    public:
        IndexIteratorImpl(OperationContext* opCtx,
                          const IndexCatalog* cat,
                          bool includeUnfinishedIndexes);

        bool more() override;
        IndexDescriptor* next() override;

        // returns the access method for the last return IndexDescriptor
        IndexAccessMethod* accessMethod(const IndexDescriptor* desc) override;

        // returns the IndexCatalogEntry for the last return IndexDescriptor
        IndexCatalogEntry* catalogEntry(const IndexDescriptor* desc) override;

    private:
        IndexIteratorImpl* clone_impl() const override;

        void _advance();

        bool _includeUnfinishedIndexes;

        OperationContext* const _opCtx;
        const IndexCatalog* _catalog;
        IndexCatalogEntryContainer::const_iterator _iterator;

        bool _start;  // only true before we've called next() or more()

        IndexCatalogEntry* _prev;
        IndexCatalogEntry* _next;

        friend class IndexCatalog;
    };

    using IndexIterator = IndexCatalog::IndexIterator;

    // ---- index set modifiers ------

    /**
     * Call this only on an empty collection from inside a WriteUnitOfWork. Index creation on an
     * empty collection can be rolled back as part of a larger WUOW. Returns the full specification
     * of the created index, as it is stored in this index catalog.
     */
    StatusWith<BSONObj> createIndexOnEmptyCollection(OperationContext* opCtx,
                                                     BSONObj spec) override;

    StatusWith<BSONObj> prepareSpecForCreate(OperationContext* opCtx,
                                             const BSONObj& original) const override;

    /**
     * Drops all indexes in the index catalog, optionally dropping the id index depending on the
     * 'includingIdIndex' parameter value. If the 'droppedIndexes' parameter is not null,
     * it is filled with the names and index info of the dropped indexes.
     */
    void dropAllIndexes(OperationContext* opCtx,
                        bool includingIdIndex,
                        stdx::function<void(const IndexDescriptor*)> onDropFn = nullptr) override;

    Status dropIndex(OperationContext* opCtx, IndexDescriptor* desc) override;

    /**
     * will drop all incompleted indexes and return specs
     * after this, the indexes can be rebuilt
     */
    std::vector<BSONObj> getAndClearUnfinishedIndexes(OperationContext* opCtx) override;


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

    // --- these probably become private?


    /**
     * disk creation order
     * 1) system.indexes entry
     * 2) collection's NamespaceDetails
     *    a) info + head
     *    b) _indexBuildsInProgress++
     * 3) indexes entry in .ns file
     * 4) system.namespaces entry for index ns
     */
    class IndexBuildBlock {
        MONGO_DISALLOW_COPYING(IndexBuildBlock);

    public:
        IndexBuildBlock(OperationContext* opCtx, Collection* collection, const BSONObj& spec);

        ~IndexBuildBlock();

        /**
         * Must be called from within a `WriteUnitOfWork`
         */
        Status init();

        /**
         * Must be called from within a `WriteUnitOfWork`
         */
        void success();

        /**
         * index build failed, clean up meta data
         *
         * Must be called from within a `WriteUnitOfWork`
         */
        void fail();

        IndexCatalogEntry* getEntry() {
            return _entry;
        }

        const std::string& getIndexName() const {
            return _indexName;
        }

        const BSONObj& getSpec() const {
            return _spec;
        }

    private:
        Collection* const _collection;
        IndexCatalog* const _catalog;
        const std::string _ns;

        BSONObj _spec;

        std::string _indexName;
        std::string _indexNamespace;

        IndexCatalogEntry* _entry;
        bool _inProgress;

        OperationContext* _opCtx;
    };

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
     * When 'keysDeletedOut' is not null, it will be set to the number of index keys removed by
     * this operation.
     */
    void unindexRecord(OperationContext* opCtx,
                       const BSONObj& obj,
                       const RecordId& loc,
                       bool noWarn,
                       int64_t* keysDeletedOut) override;

    // ------- temp internal -------

    inline std::string getAccessMethodName(OperationContext* opCtx,
                                           const BSONObj& keyPattern) override {
        return _getAccessMethodName(opCtx, keyPattern);
    }

    Status _upgradeDatabaseMinorVersionIfNeeded(OperationContext* opCtx,
                                                const std::string& newPluginName) override;

    // public static helpers

    static BSONObj fixIndexKey(const BSONObj& key);

    /**
     * Fills out 'options' in order to indicate whether to allow dups or relax
     * index constraints, as needed by replication.
     */
    static void prepareInsertDeleteOptions(OperationContext* opCtx,
                                           const IndexDescriptor* desc,
                                           InsertDeleteOptions* options);

private:
    static const BSONObj _idObj;  // { _id : 1 }

    bool _shouldOverridePlugin(OperationContext* opCtx, const BSONObj& keyPattern) const;

    /**
     * This differs from IndexNames::findPluginName in that returns the plugin name we *should*
     * use, not the plugin name inside of the provided key pattern.  To understand when these
     * differ, see shouldOverridePlugin.
     */
    std::string _getAccessMethodName(OperationContext* opCtx, const BSONObj& keyPattern) const;

    void _checkMagic() const;

    Status _indexFilteredRecords(OperationContext* opCtx,
                                 IndexCatalogEntry* index,
                                 const std::vector<BsonRecord>& bsonRecords,
                                 int64_t* keysInsertedOut);

    Status _indexRecords(OperationContext* opCtx,
                         IndexCatalogEntry* index,
                         const std::vector<BsonRecord>& bsonRecords,
                         int64_t* keysInsertedOut);

    Status _unindexRecord(OperationContext* opCtx,
                          IndexCatalogEntry* index,
                          const BSONObj& obj,
                          const RecordId& loc,
                          bool logIfError,
                          int64_t* keysDeletedOut);

    inline const IndexCatalogEntryContainer& _getEntries() const override {
        return this->_entries;
    }

    inline IndexCatalogEntryContainer& _getEntries() override {
        return this->_entries;
    }

    /**
     * this does no sanity checks
     */
    Status _dropIndex(OperationContext* opCtx, IndexCatalogEntry* entry) override;

    // just does disk hanges
    // doesn't change memory state, etc...
    void _deleteIndexFromDisk(OperationContext* opCtx,
                              const std::string& indexName,
                              const std::string& indexNamespace);

    // descriptor ownership passes to _setupInMemoryStructures
    // initFromDisk: Avoids registering a change to undo this operation when set to true.
    //               You must set this flag if calling this function outside of a UnitOfWork.
    IndexCatalogEntry* _setupInMemoryStructures(OperationContext* opCtx,
                                                std::unique_ptr<IndexDescriptor> descriptor,
                                                bool initFromDisk);

    // Apply a set of transformations to the user-provided index object 'spec' to make it
    // conform to the standard for insertion.  This function adds the 'v' field if it didn't
    // exist, removes the '_id' field if it exists, applies plugin-level transformations if
    // appropriate, etc.
    static StatusWith<BSONObj> _fixIndexSpec(OperationContext* opCtx,
                                             Collection* collection,
                                             const BSONObj& spec);

    Status _isSpecOk(OperationContext* opCtx, const BSONObj& spec) const;

    Status _doesSpecConflictWithExisting(OperationContext* opCtx, const BSONObj& spec) const;

    inline const Collection* _getCollection() const override {
        return this->_collection;
    }

    inline Collection* _getCollection() override {
        return this->_collection;
    }


    int _magic;
    Collection* const _collection;
    const int _maxNumIndexesAllowed;

    IndexCatalogEntryContainer _entries;

    // These are the index specs of indexes that were "leftover".
    // "Leftover" means they were unfinished when a mongod shut down.
    // Certain operations are prohibited until someone fixes.
    // Retrieve by calling getAndClearUnfinishedIndexes().
    std::vector<BSONObj> _unfinishedIndexes;

    IndexCatalog* const _this;


    inline static IndexCatalogEntry* _setupInMemoryStructures(
        IndexCatalog* const this_,
        OperationContext* const opCtx,
        std::unique_ptr<IndexDescriptor> descriptor,
        const bool initFromDisk) {
        return this_->_setupInMemoryStructures(opCtx, std::move(descriptor), initFromDisk);
    }

    inline static Status _dropIndex(IndexCatalog* const this_,
                                    OperationContext* const opCtx,
                                    IndexCatalogEntry* const desc) {
        return this_->_dropIndex(opCtx, desc);
    }
};
}  // namespace mongo
