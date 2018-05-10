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

#include <memory>
#include <vector>

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/shim.h"
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
class IndexCatalog {
public:
    class IndexIterator {
    public:
        class Impl {
        public:
            virtual ~Impl() = 0;

        private:
            virtual Impl* clone_impl() const = 0;

        public:
            inline std::unique_ptr<Impl> clone() const {
                return std::unique_ptr<Impl>{this->clone_impl()};
            }

            virtual bool more() = 0;
            virtual IndexDescriptor* next() = 0;

            virtual IndexAccessMethod* accessMethod(const IndexDescriptor* desc) = 0;

            virtual IndexCatalogEntry* catalogEntry(const IndexDescriptor* desc) = 0;
        };

        static MONGO_DECLARE_SHIM((OperationContext * opCtx,
                                   const IndexCatalog* cat,
                                   bool includeUnfinishedIndexes,
                                   PrivateTo<IndexIterator>)
                                      ->std::unique_ptr<Impl>) makeImpl;

    private:
        explicit inline IndexIterator(OperationContext* const opCtx,
                                      const IndexCatalog* const cat,
                                      const bool includeUnfinishedIndexes)
            : _pimpl(makeImpl(opCtx, cat, includeUnfinishedIndexes, PrivateCall<IndexIterator>{})) {
        }

    public:
        inline ~IndexIterator() = default;

        inline IndexIterator(const IndexIterator& copy) = default;
        inline IndexIterator& operator=(const IndexIterator& copy) = default;

        inline IndexIterator(IndexIterator&& copy) = default;
        inline IndexIterator& operator=(IndexIterator&& copy) = default;

        inline bool more() {
            return this->_impl().more();
        }

        inline IndexDescriptor* next() {
            return this->_impl().next();
        }

        // Returns the access method for the last return IndexDescriptor.
        inline IndexAccessMethod* accessMethod(const IndexDescriptor* const desc) {
            return this->_impl().accessMethod(desc);
        }

        // Returns the IndexCatalogEntry for the last return IndexDescriptor.
        inline IndexCatalogEntry* catalogEntry(const IndexDescriptor* const desc) {
            return this->_impl().catalogEntry(desc);
        }

    private:
        // This structure exists to give us a customization point to decide how to force users of
        // this class to depend upon the corresponding `index_catalog.cpp` Translation Unit (TU).
        // All public forwarding functions call `_impl(), and `_impl` creates an instance of this
        // structure.
        struct TUHook {
            static void hook() noexcept;

            explicit inline TUHook() noexcept {
                if (kDebugBuild)
                    this->hook();
            }
        };

        inline const Impl& _impl() const {
            TUHook{};
            return *this->_pimpl;
        }

        inline Impl& _impl() {
            TUHook{};
            return *this->_pimpl;
        }

        clonable_ptr<Impl> _pimpl;

        friend IndexCatalog;
    };

    class Impl {
    public:
        virtual ~Impl() = 0;

        virtual Status init(OperationContext* opCtx) = 0;

        virtual bool ok() const = 0;

        virtual bool haveAnyIndexes() const = 0;

        virtual int numIndexesTotal(OperationContext* opCtx) const = 0;

        virtual int numIndexesReady(OperationContext* opCtx) const = 0;

        virtual bool haveIdIndex(OperationContext* opCtx) const = 0;

        virtual BSONObj getDefaultIdIndexSpec() const = 0;

        virtual IndexDescriptor* findIdIndex(OperationContext* opCtx) const = 0;

        virtual IndexDescriptor* findIndexByName(OperationContext* opCtx,
                                                 StringData name,
                                                 bool includeUnfinishedIndexes) const = 0;

        virtual IndexDescriptor* findIndexByKeyPatternAndCollationSpec(
            OperationContext* opCtx,
            const BSONObj& key,
            const BSONObj& collationSpec,
            bool includeUnfinishedIndexes) const = 0;

        virtual void findIndexesByKeyPattern(OperationContext* opCtx,
                                             const BSONObj& key,
                                             bool includeUnfinishedIndexes,
                                             std::vector<IndexDescriptor*>* matches) const = 0;

        virtual IndexDescriptor* findShardKeyPrefixedIndex(OperationContext* opCtx,
                                                           const BSONObj& shardKey,
                                                           bool requireSingleKey) const = 0;

        virtual void findIndexByType(OperationContext* opCtx,
                                     const std::string& type,
                                     std::vector<IndexDescriptor*>& matches,
                                     bool includeUnfinishedIndexes) const = 0;

        virtual const IndexDescriptor* refreshEntry(OperationContext* opCtx,
                                                    const IndexDescriptor* oldDesc) = 0;

        virtual const IndexCatalogEntry* getEntry(const IndexDescriptor* desc) const = 0;

        virtual IndexAccessMethod* getIndex(const IndexDescriptor* desc) = 0;

        virtual const IndexAccessMethod* getIndex(const IndexDescriptor* desc) const = 0;

        virtual Status checkUnfinished() const = 0;

        virtual StatusWith<BSONObj> createIndexOnEmptyCollection(OperationContext* opCtx,
                                                                 BSONObj spec) = 0;

        virtual StatusWith<BSONObj> prepareSpecForCreate(OperationContext* opCtx,
                                                         const BSONObj& original) const = 0;

        virtual void dropAllIndexes(
            OperationContext* opCtx,
            bool includingIdIndex,
            stdx::function<void(const IndexDescriptor*)> onDropFn = nullptr) = 0;

        virtual Status dropIndex(OperationContext* opCtx, IndexDescriptor* desc) = 0;

        virtual std::vector<BSONObj> getAndClearUnfinishedIndexes(OperationContext* opCtx) = 0;

        virtual bool isMultikey(OperationContext* opCtx, const IndexDescriptor* idx) = 0;

        virtual MultikeyPaths getMultikeyPaths(OperationContext* opCtx,
                                               const IndexDescriptor* idx) = 0;

        virtual Status indexRecords(OperationContext* opCtx,
                                    const std::vector<BsonRecord>& bsonRecords,
                                    int64_t* keysInsertedOut) = 0;

        virtual void unindexRecord(OperationContext* opCtx,
                                   const BSONObj& obj,
                                   const RecordId& loc,
                                   bool noWarn,
                                   int64_t* keysDeletedOut) = 0;

        virtual std::string getAccessMethodName(OperationContext* opCtx,
                                                const BSONObj& keyPattern) = 0;

        virtual Status _upgradeDatabaseMinorVersionIfNeeded(OperationContext* opCtx,
                                                            const std::string& newPluginName) = 0;

    private:
        virtual const Collection* _getCollection() const = 0;
        virtual Collection* _getCollection() = 0;

        virtual IndexCatalogEntry* _setupInMemoryStructures(
            OperationContext* opCtx,
            std::unique_ptr<IndexDescriptor> descriptor,
            bool initFromDisk) = 0;
        virtual Status _dropIndex(OperationContext* opCtx, IndexCatalogEntry* entry) = 0;

        virtual const IndexCatalogEntryContainer& _getEntries() const = 0;
        virtual IndexCatalogEntryContainer& _getEntries() = 0;

        virtual void _deleteIndexFromDisk(OperationContext* const opCtx,
                                          const std::string& indexName,
                                          const std::string& indexNamespace) = 0;

        friend IndexCatalog;
    };

public:
    static MONGO_DECLARE_SHIM((IndexCatalog * this_,
                               Collection* collection,
                               int maxNumIndexesAllowed,
                               PrivateTo<IndexCatalog>)
                                  ->std::unique_ptr<Impl>) makeImpl;

    inline ~IndexCatalog() = default;

    explicit inline IndexCatalog(Collection* const collection, const int maxNumIndexesAllowed)
        : _pimpl(makeImpl(this, collection, maxNumIndexesAllowed, PrivateCall<IndexCatalog>{})) {}

    inline IndexCatalog(IndexCatalog&&) = delete;
    inline IndexCatalog& operator=(IndexCatalog&&) = delete;

    // Must be called before used.
    inline Status init(OperationContext* const opCtx) {
        return this->_impl().init(opCtx);
    }

    inline bool ok() const {
        return this->_impl().ok();
    }

    // ---- accessors -----

    inline bool haveAnyIndexes() const {
        return this->_impl().haveAnyIndexes();
    }

    inline int numIndexesTotal(OperationContext* const opCtx) const {
        return this->_impl().numIndexesTotal(opCtx);
    }

    inline int numIndexesReady(OperationContext* const opCtx) const {
        return this->_impl().numIndexesReady(opCtx);
    }

    inline int numIndexesInProgress(OperationContext* const opCtx) const {
        return numIndexesTotal(opCtx) - numIndexesReady(opCtx);
    }

    /**
     * this is in "alive" until the Collection goes away
     * in which case everything from this tree has to go away.
     */

    inline bool haveIdIndex(OperationContext* const opCtx) const {
        return this->_impl().haveIdIndex(opCtx);
    }

    /**
     * Returns the spec for the id index to create by default for this collection.
     */
    inline BSONObj getDefaultIdIndexSpec() const {
        return this->_impl().getDefaultIdIndexSpec();
    }

    inline IndexDescriptor* findIdIndex(OperationContext* const opCtx) const {
        return this->_impl().findIdIndex(opCtx);
    }

    /**
     * Find index by name.  The index name uniquely identifies an index.
     *
     * @return null if cannot find
     */
    inline IndexDescriptor* findIndexByName(OperationContext* const opCtx,
                                            const StringData name,
                                            const bool includeUnfinishedIndexes = false) const {
        return this->_impl().findIndexByName(opCtx, name, includeUnfinishedIndexes);
    }

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
    inline IndexDescriptor* findIndexByKeyPatternAndCollationSpec(
        OperationContext* const opCtx,
        const BSONObj& key,
        const BSONObj& collationSpec,
        const bool includeUnfinishedIndexes = false) const {
        return this->_impl().findIndexByKeyPatternAndCollationSpec(
            opCtx, key, collationSpec, includeUnfinishedIndexes);
    }

    /**
     * Find indexes with a matching key pattern, putting them into the vector 'matches'.  The key
     * pattern alone does not uniquely identify an index.
     *
     * Consider using 'findIndexByName' if expecting to match one index.
     */
    inline void findIndexesByKeyPattern(OperationContext* const opCtx,
                                        const BSONObj& key,
                                        const bool includeUnfinishedIndexes,
                                        std::vector<IndexDescriptor*>* const matches) const {
        return this->_impl().findIndexesByKeyPattern(opCtx, key, includeUnfinishedIndexes, matches);
    }

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
    inline IndexDescriptor* findShardKeyPrefixedIndex(OperationContext* const opCtx,
                                                      const BSONObj& shardKey,
                                                      const bool requireSingleKey) const {
        return this->_impl().findShardKeyPrefixedIndex(opCtx, shardKey, requireSingleKey);
    }

    inline void findIndexByType(OperationContext* const opCtx,
                                const std::string& type,
                                std::vector<IndexDescriptor*>& matches,
                                const bool includeUnfinishedIndexes = false) const {
        return this->_impl().findIndexByType(opCtx, type, matches, includeUnfinishedIndexes);
    }

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
    inline const IndexDescriptor* refreshEntry(OperationContext* const opCtx,
                                               const IndexDescriptor* const oldDesc) {
        return this->_impl().refreshEntry(opCtx, oldDesc);
    }

    // never returns NULL
    const IndexCatalogEntry* getEntry(const IndexDescriptor* const desc) const {
        return this->_impl().getEntry(desc);
    }

    inline IndexAccessMethod* getIndex(const IndexDescriptor* const desc) {
        return this->_impl().getIndex(desc);
    }

    inline const IndexAccessMethod* getIndex(const IndexDescriptor* const desc) const {
        return this->_impl().getIndex(desc);
    }

    /**
     * Returns a not-ok Status if there are any unfinished index builds. No new indexes should
     * be built when in this state.
     */
    inline Status checkUnfinished() const {
        return this->_impl().checkUnfinished();
    }

    inline IndexIterator getIndexIterator(OperationContext* const opCtx,
                                          const bool includeUnfinishedIndexes) const {
        return IndexIterator(opCtx, this, includeUnfinishedIndexes);
    };

    // ---- index set modifiers ------

    /**
     * Call this only on an empty collection from inside a WriteUnitOfWork. Index creation on an
     * empty collection can be rolled back as part of a larger WUOW. Returns the full specification
     * of the created index, as it is stored in this index catalog.
     */
    inline StatusWith<BSONObj> createIndexOnEmptyCollection(OperationContext* const opCtx,
                                                            const BSONObj spec) {
        return this->_impl().createIndexOnEmptyCollection(opCtx, spec);
    }

    inline StatusWith<BSONObj> prepareSpecForCreate(OperationContext* const opCtx,
                                                    const BSONObj& original) const {
        return this->_impl().prepareSpecForCreate(opCtx, original);
    }

    /**
     * Drops all indexes in the index catalog, optionally dropping the id index depending on the
     * 'includingIdIndex' parameter value. If 'onDropFn' is provided, it will be called before each
     * index is dropped to allow timestamping each individual drop.
     */
    inline void dropAllIndexes(OperationContext* opCtx,
                               bool includingIdIndex,
                               stdx::function<void(const IndexDescriptor*)> onDropFn = nullptr) {
        this->_impl().dropAllIndexes(opCtx, includingIdIndex, onDropFn);
    }

    inline Status dropIndex(OperationContext* const opCtx, IndexDescriptor* const desc) {
        return this->_impl().dropIndex(opCtx, desc);
    }

    /**
     * will drop all incompleted indexes and return specs
     * after this, the indexes can be rebuilt.
     */
    inline std::vector<BSONObj> getAndClearUnfinishedIndexes(OperationContext* const opCtx) {
        return this->_impl().getAndClearUnfinishedIndexes(opCtx);
    }

    // ---- modify single index

    /**
     * Returns true if the index 'idx' is multikey, and returns false otherwise.
     */
    inline bool isMultikey(OperationContext* const opCtx, const IndexDescriptor* const idx) {
        return this->_impl().isMultikey(opCtx, idx);
    }

    /**
     * Returns the path components that cause the index 'idx' to be multikey if the index supports
     * path-level multikey tracking, and returns an empty vector if path-level multikey tracking
     * isn't supported.
     *
     * If the index supports path-level multikey tracking but isn't multikey, then this function
     * returns a vector with size equal to the number of elements in the index key pattern where
     * each element in the vector is an empty set.
     */
    inline MultikeyPaths getMultikeyPaths(OperationContext* const opCtx,
                                          const IndexDescriptor* const idx) {
        return this->_impl().getMultikeyPaths(opCtx, idx);
    }

    // --- these probably become private?

    // ----- data modifiers ------

    /**
     * When 'keysInsertedOut' is not null, it will be set to the number of index keys inserted by
     * this operation.
     *
     * This method may throw.
     */
    inline Status indexRecords(OperationContext* const opCtx,
                               const std::vector<BsonRecord>& bsonRecords,
                               int64_t* const keysInsertedOut) {
        return this->_impl().indexRecords(opCtx, bsonRecords, keysInsertedOut);
    }

    /**
     * When 'keysDeletedOut' is not null, it will be set to the number of index keys removed by
     * this operation.
     */
    inline void unindexRecord(OperationContext* const opCtx,
                              const BSONObj& obj,
                              const RecordId& loc,
                              const bool noWarn,
                              int64_t* const keysDeletedOut) {
        return this->_impl().unindexRecord(opCtx, obj, loc, noWarn, keysDeletedOut);
    }

    // ------- temp internal -------

    inline std::string getAccessMethodName(OperationContext* const opCtx,
                                           const BSONObj& keyPattern) {
        return this->_impl().getAccessMethodName(opCtx, keyPattern);
    }

    // public helpers

    /**
     * Returns length of longest index name.
     * This includes unfinished indexes.
     */
    std::string::size_type getLongestIndexNameLength(OperationContext* opCtx) const;

    // public static helpers

    static MONGO_DECLARE_SHIM((const BSONObj& key)->BSONObj) fixIndexKey;

    /**
     * Fills out 'options' in order to indicate whether to allow dups or relax
     * index constraints, as needed by replication.
     */
    static MONGO_DECLARE_SHIM(
        (OperationContext * opCtx, const IndexDescriptor* desc, InsertDeleteOptions* options)->void)
        prepareInsertDeleteOptions;

private:
    inline const Collection* _getCollection() const {
        return this->_impl()._getCollection();
    }

    inline Collection* _getCollection() {
        return this->_impl()._getCollection();
    }

    IndexCatalogEntry* _setupInMemoryStructures(OperationContext* opCtx,
                                                std::unique_ptr<IndexDescriptor> descriptor,
                                                bool initFromDisk);

    inline Status _dropIndex(OperationContext* const opCtx, IndexCatalogEntry* const desc) {
        return this->_impl()._dropIndex(opCtx, desc);
    }

    inline Status _upgradeDatabaseMinorVersionIfNeeded(OperationContext* const opCtx,
                                                       const std::string& newPluginName) {
        return this->_impl()._upgradeDatabaseMinorVersionIfNeeded(opCtx, newPluginName);
    }

    inline const IndexCatalogEntryContainer& _getEntries() const {
        return this->_impl()._getEntries();
    }

    inline IndexCatalogEntryContainer& _getEntries() {
        return this->_impl()._getEntries();
    }

    inline static IndexCatalogEntryContainer& _getEntries(IndexCatalog* const this_) {
        return this_->_getEntries();
    }

    inline static const IndexCatalogEntryContainer& _getEntries(const IndexCatalog* const this_) {
        return this_->_getEntries();
    }

    inline void _deleteIndexFromDisk(OperationContext* const opCtx,
                                     const std::string& indexName,
                                     const std::string& indexNamespace) {
        return this->_impl()._deleteIndexFromDisk(opCtx, indexName, indexNamespace);
    }

    inline static void _deleteIndexFromDisk(IndexCatalog* const this_,
                                            OperationContext* const opCtx,
                                            const std::string& indexName,
                                            const std::string& indexNamespace) {
        return this_->_deleteIndexFromDisk(opCtx, indexName, indexNamespace);
    }

    // This structure exists to give us a customization point to decide how to force users of this
    // class to depend upon the corresponding `index_catalog.cpp` Translation Unit (TU).  All public
    // forwarding functions call `_impl(), and `_impl` creates an instance of this structure.
    struct TUHook {
        static void hook() noexcept;

        explicit inline TUHook() noexcept {
            if (kDebugBuild)
                this->hook();
        }
    };

    inline const Impl& _impl() const {
        TUHook{};
        return *this->_pimpl;
    }

    inline Impl& _impl() {
        TUHook{};
        return *this->_pimpl;
    }

    std::unique_ptr<Impl> _pimpl;

    friend IndexCatalogEntry;
    friend class IndexCatalogImpl;
    friend class MultiIndexBlockImpl;
};
}  // namespace mongo
