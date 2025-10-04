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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/functional.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObjBuilder;
class MatchExpression;
struct UpdateTicket;
struct InsertDeleteOptions;
class SortedDataIndexAccessMethod;
struct CollectionOptions;

namespace CollectionValidation {
class ValidationOptions;
}

/**
 * An IndexAccessMethod is the interface through which all the mutation, lookup, and
 * traversal of index entries is done. The class is designed so that the underlying index
 * data structure is opaque to the caller.
 *
 * IndexAccessMethods for existing indices are obtained through the system catalog.
 *
 * We assume the caller has whatever locks required.  This interface is not thread safe.
 *
 */
class IndexAccessMethod {
    IndexAccessMethod(const IndexAccessMethod&) = delete;
    IndexAccessMethod& operator=(const IndexAccessMethod&) = delete;

public:
    using ShouldRelaxConstraintsFn =
        std::function<bool(OperationContext* opCtx, const CollectionPtr& collection)>;
    using OnSuppressedErrorFn = unique_function<void(OperationContext* opCtx,
                                                     const IndexCatalogEntry* entry,
                                                     Status status,
                                                     const BSONObj& obj,
                                                     const boost::optional<RecordId>& loc)>;
    using KeyHandlerFn =
        unique_function<Status(const CollectionPtr& coll, const key_string::View&)>;
    using RecordIdHandlerFn = unique_function<Status(const RecordId&)>;
    using YieldFn = unique_function<std::pair<const CollectionPtr*, const IndexCatalogEntry*>(
        OperationContext*)>;

    IndexAccessMethod() = default;
    virtual ~IndexAccessMethod() = default;

    static std::unique_ptr<IndexAccessMethod> make(OperationContext* opCtx,
                                                   RecoveryUnit& ru,
                                                   const NamespaceString& nss,
                                                   const CollectionOptions& collectionOptions,
                                                   IndexCatalogEntry* entry,
                                                   StringData ident);

    /**
     * Equivalent to (but shorter and faster than): dynamic_cast<SortedDataIndexAccessMethod*>(this)
     */
    virtual SortedDataIndexAccessMethod* asSortedData() {
        return nullptr;
    }
    virtual const SortedDataIndexAccessMethod* asSortedData() const {
        return nullptr;
    }

    //
    // Lookup, traversal, and mutation support
    //

    /**
     * Informs the index of inserts, updates, and deletes of records from the indexed collection.
     */
    virtual Status insert(OperationContext* opCtx,
                          SharedBufferFragmentBuilder& pooledBufferBuilder,
                          const CollectionPtr& coll,
                          const IndexCatalogEntry* entry,
                          const std::vector<BsonRecord>& bsonRecords,
                          const InsertDeleteOptions& options,
                          int64_t* numInserted) = 0;

    virtual void remove(OperationContext* opCtx,
                        SharedBufferFragmentBuilder& pooledBufferBuilder,
                        const CollectionPtr& coll,
                        const IndexCatalogEntry* entry,
                        const BSONObj& obj,
                        const RecordId& loc,
                        bool logIfError,
                        const InsertDeleteOptions& options,
                        int64_t* numDeleted,
                        CheckRecordId checkRecordId) = 0;

    virtual Status update(OperationContext* opCtx,
                          RecoveryUnit& ru,
                          SharedBufferFragmentBuilder& pooledBufferBuilder,
                          const BSONObj& oldDoc,
                          const BSONObj& newDoc,
                          const RecordId& loc,
                          const CollectionPtr& coll,
                          const IndexCatalogEntry* entry,
                          const InsertDeleteOptions& options,
                          int64_t* numInserted,
                          int64_t* numDeleted) = 0;

    // ------ index level operations ------


    /**
     * initializes this index
     * only called once for the lifetime of the index
     * if called multiple times, is an error
     */
    virtual Status initializeAsEmpty() = 0;

    /**
     * Validates the index. If 'full' is false, only performs checks which do not traverse the
     * index. If 'full' is true, additionally traverses the index and validates its internal
     * structure.
     */
    virtual IndexValidateResults validate(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        const CollectionValidation::ValidationOptions& options) const = 0;

    /**
     * Returns the number of keys in the index, traversing the index to do so.
     */
    virtual int64_t numKeys(OperationContext* opCtx, RecoveryUnit& ru) const = 0;

    /**
     * Add custom statistics about this index to BSON object builder, for display.
     *
     * 'scale' is a scaling factor to apply to all byte statistics.
     *
     * Returns true if stats were appended.
     */
    virtual bool appendCustomStats(OperationContext* opCtx,
                                   RecoveryUnit& ru,
                                   BSONObjBuilder* result,
                                   double scale) const = 0;

    /**
     * @return The number of bytes consumed by this index.
     *         Exactly what is counted is not defined based on padding, re-use, etc...
     */
    virtual long long getSpaceUsedBytes(OperationContext* opCtx, RecoveryUnit& ru) const = 0;

    /**
     * The number of unused free bytes consumed by this index on disk.
     */
    virtual long long getFreeStorageBytes(OperationContext* opCtx, RecoveryUnit& ru) const = 0;

    /**
     * Attempt compaction to regain disk space if the indexed record store supports
     * compaction-in-place.
     */
    virtual StatusWith<int64_t> compact(OperationContext* opCtx,
                                        RecoveryUnit& ru,
                                        const CompactOptions& options) = 0;

    /**
     * Removes all entries from the index.
     */
    virtual Status truncate(OperationContext* opCtx, RecoveryUnit& ru) = 0;

    /**
     * Fetches the Ident for this index.
     */
    virtual std::shared_ptr<Ident> getSharedIdent() const = 0;

    /**
     * Sets the Ident for this index.
     */
    virtual void setIdent(std::shared_ptr<Ident> newIdent) = 0;

    virtual Status applyIndexBuildSideWrite(OperationContext* opCtx,
                                            const CollectionPtr& coll,
                                            const IndexCatalogEntry* entry,
                                            const BSONObj& operation,
                                            const InsertDeleteOptions& options,
                                            KeyHandlerFn&& onDuplicateKey,
                                            int64_t* keysInserted,
                                            int64_t* keysDeleted) = 0;

    //
    // Bulk operations support
    //

    class BulkBuilder {
    public:
        virtual ~BulkBuilder() = default;

        /**
         * Insert into the BulkBuilder as-if inserting into an IndexAccessMethod.
         */
        virtual Status insert(OperationContext* opCtx,
                              const CollectionPtr& collection,
                              const IndexCatalogEntry* entry,
                              const BSONObj& obj,
                              const RecordId& loc,
                              const InsertDeleteOptions& options,
                              const OnSuppressedErrorFn& onSuppressedError = nullptr,
                              const ShouldRelaxConstraintsFn& shouldRelaxConstraints = nullptr) = 0;

        /**
         * Call this when you are ready to finish your bulk work.
         * @param dupsAllowed - If false and 'dupRecords' is not null, append with the RecordIds of
         *                      the uninserted duplicates.
         * @param yieldIterations - The number of iterations run before each yielding. Will not
         * yield if zero.
         * @param onDuplicateKeyInserted - Will be called for each duplicate key inserted into the
         * index.
         * @param onDuplicateRecord - If not nullptr, will be called for each RecordId of uninserted
         * duplicate keys.
         * @param yieldFn - A function to invoke to request a yield and then restore. It returns the
         * new CollectionPtr* and IndexCatalogEntry* entry that shall be used from this point on.
         */
        virtual Status commit(OperationContext* opCtx,
                              RecoveryUnit& ru,
                              const CollectionPtr* collection,
                              const IndexCatalogEntry* entry,
                              bool dupsAllowed,
                              int32_t yieldIterations,
                              const KeyHandlerFn& onDuplicateKeyInserted,
                              const RecordIdHandlerFn& onDuplicateRecord,
                              const YieldFn& yieldFn) = 0;

        virtual const MultikeyPaths& getMultikeyPaths() const = 0;

        virtual bool isMultikey() const = 0;

        /**
         * Persists on disk the keys that have been inserted using this BulkBuilder.
         */
        virtual IndexStateInfo persistDataForShutdown() = 0;

    protected:
        static void countNewBuildInStats();
        static void countResumedBuildInStats();
        static SorterFileStats* bulkBuilderFileStats();
        static SorterTracker* bulkBuilderTracker();
    };

    /**
     * Starts a bulk operation.
     * You work on the returned BulkBuilder and then call bulk->commit().
     * This can return NULL, meaning bulk mode is not available.
     *
     * It is only legal to initiate bulk when the index is new and empty, or when resuming an index
     * build.
     *
     * maxMemoryUsageBytes: amount of memory consumed before the external sorter starts spilling to
     *                      disk
     * stateInfo: the information to use to resume the index build, or boost::none if starting a
     * new index build.
     */
    virtual std::unique_ptr<BulkBuilder> initiateBulk(
        const IndexCatalogEntry* entry,
        size_t maxMemoryUsageBytes,
        const boost::optional<IndexStateInfo>& stateInfo,
        const DatabaseName& dbName,
        const IndexBuildMethodEnum& method) = 0;
};

/**
 * Updates are two steps: verify that it's a valid update, and perform it.
 * prepareUpdate fills out the UpdateStatus and update actually applies it.
 */
struct UpdateTicket {
    bool _isValid{false};

    KeyStringSet oldKeys;
    KeyStringSet newKeys;

    KeyStringSet newMultikeyMetadataKeys;

    KeyStringSet removed;
    KeyStringSet added;

    RecordId loc;
    bool dupsAllowed;

    // Holds the path components that would cause this index to be multikey as a result of inserting
    // 'newKeys'. The 'newMultikeyPaths' vector remains empty if this index doesn't support
    // path-level multikey tracking.
    MultikeyPaths newMultikeyPaths;
};

/**
 * Flags we can set for inserts and deletes (and updates, which are kind of both).
 */
struct InsertDeleteOptions {
    // Are duplicate keys allowed in the index?
    bool dupsAllowed = false;

    /**
     * Specifies whether getKeys should relax the index constraints or not, in order of most
     * permissive to least permissive.
     */
    enum class ConstraintEnforcementMode {
        // Relax all constraints.
        kRelaxConstraints,
        // Relax constraints only if shouldRelaxConstraintsFn callback returns true.
        kRelaxConstraintsCallback,
        // Relax all constraints on documents that don't apply to a partial index.
        kRelaxConstraintsUnfiltered,
        // Enforce all constraints.
        kEnforceConstraints
    };

    // Should we relax the index constraints?
    ConstraintEnforcementMode getKeysMode = ConstraintEnforcementMode::kEnforceConstraints;
};

/**
 * Provides implementations for many functions in the IndexAccessMethod interface that will be
 * shared across concrete implementations.
 *
 * IndexCatalogEntry owns an instance of IndexAccessMethod; an IndexCatalogEntry is also required
 * for the initialization and core functionality of this abstract class. To avoid any circular
 * dependencies, it is important that IndexAccessMethod remain an interface.
 */
class SortedDataIndexAccessMethod : public IndexAccessMethod {
    SortedDataIndexAccessMethod(const SortedDataIndexAccessMethod&) = delete;
    SortedDataIndexAccessMethod& operator=(const SortedDataIndexAccessMethod&) = delete;

public:
    //
    // SortedData-specific functions
    //

    /**
     * Splits the sets 'left' and 'right' into two sets, the first containing the elements that
     * only appeared in 'left', and the second containing only elements that appeared in 'right'.
     *
     * Note this considers objects which are not identical as distinct objects. For example,
     * setDifference({BSON("a" << 0.0)}, {BSON("a" << 0LL)}) would result in the pair
     * ( {BSON("a" << 0.0)}, {BSON("a" << 0LL)} ).
     */
    static std::pair<KeyStringSet, KeyStringSet> setDifference(const KeyStringSet& left,
                                                               const KeyStringSet& right);

    SortedDataIndexAccessMethod(const IndexCatalogEntry* btreeState,
                                std::unique_ptr<SortedDataInterface> btree);

    /**
     * Specifies whether getKeys is being used in the context of creating new keys, deleting
     * or validating existing keys.
     */
    enum class GetKeysContext { kRemovingKeys, kAddingKeys, kValidatingKeys };

    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     * Based on 'mode', it will honor or ignore index constraints, e.g. duplicated key, key too
     * long, and geo index parsing errors. The ignoring of constraints is for replication due to
     * idempotency reasons. In those cases, the generated 'keys' will be empty.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information, then this function resizes
     * 'multikeyPaths' to have the same number of elements as the index key pattern and fills each
     * element with the prefixes of the indexed field that would cause this index to be multikey as
     * a result of inserting 'keys'.
     *
     * If the 'multikeyMetadataKeys' pointer is non-null, then the function will populate the
     * BSONObjSet with any multikey metadata keys generated while processing the document. These
     * keys are not associated with the document itself, but instead represent multi-key path
     * information that must be stored in a reserved keyspace within the index.
     *
     * If any key generation errors which should be suppressed due to the provided GetKeysMode are
     * encountered, 'onSuppressedErrorFn' is called if provided. The 'onSuppressedErrorFn'
     * return value indicates whether the error should finally suppressed. If not provided, it is as
     * if it returned true, and all suppressible errors are suppressed.
     */
    void getKeys(OperationContext* opCtx,
                 const CollectionPtr& collection,
                 const IndexCatalogEntry* entry,
                 SharedBufferFragmentBuilder& pooledBufferBuilder,
                 const BSONObj& obj,
                 InsertDeleteOptions::ConstraintEnforcementMode mode,
                 GetKeysContext context,
                 KeyStringSet* keys,
                 KeyStringSet* multikeyMetadataKeys,
                 MultikeyPaths* multikeyPaths,
                 const boost::optional<RecordId>& id,
                 const OnSuppressedErrorFn& onSuppressedError = nullptr,
                 const ShouldRelaxConstraintsFn& shouldRelaxConstraints = nullptr) const;

    /**
     * Inserts the specified keys into the index. Does not attempt to determine whether the
     * insertion of these keys should cause the index to become multikey. The 'numInserted' output
     * parameter, if non-nullptr, will be reset to the number of keys inserted by this function
     * call, or to zero in the case of either a non-OK return Status or an empty 'keys' argument.
     */
    Status insertKeys(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        const CollectionPtr& coll,
        const IndexCatalogEntry* entry,
        const KeyStringSet& keys,
        const InsertDeleteOptions& options,
        KeyHandlerFn&& onDuplicateKey,
        int64_t* numInserted,
        IncludeDuplicateRecordId includeDuplicateRecordId = IncludeDuplicateRecordId::kOff);

    /**
     * Inserts the specified keys into the index. and determines whether these keys should cause the
     * index to become multikey. If so, this method also handles the task of marking the index as
     * multikey in the catalog, and sets the path-level multikey information if applicable.
     */
    Status insertKeysAndUpdateMultikeyPaths(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        const CollectionPtr& coll,
        const IndexCatalogEntry* entry,
        const KeyStringSet& keys,
        const KeyStringSet& multikeyMetadataKeys,
        const MultikeyPaths& multikeyPaths,
        const InsertDeleteOptions& options,
        KeyHandlerFn&& onDuplicateKey,
        int64_t* numInserted,
        IncludeDuplicateRecordId includeDuplicateRecordId = IncludeDuplicateRecordId::kOff);

    /**
     * Analogous to insertKeys above, but remove the keys instead of inserting them.
     * 'numDeleted' will be set to the number of keys removed from the index for the provided keys.
     */
    Status removeKeys(OperationContext* opCtx,
                      RecoveryUnit& ru,
                      const IndexCatalogEntry* entry,
                      const KeyStringSet& keys,
                      const InsertDeleteOptions& options,
                      int64_t* numDeleted) const;

    /**
     * Gets the keys of the documents 'from' and 'to' and prepares them for the update.
     * Provides a ticket for actually performing the update.
     */
    void prepareUpdate(OperationContext* opCtx,
                       const CollectionPtr& collection,
                       const IndexCatalogEntry* entry,
                       const BSONObj& from,
                       const BSONObj& to,
                       const RecordId& loc,
                       const InsertDeleteOptions& options,
                       UpdateTicket* ticket) const;

    /**
     * Perform a validated update.  The keys for the 'from' object will be removed, and the keys
     * for the object 'to' will be added.  Returns OK if the update succeeded, failure if it did
     * not.  If an update does not succeed, the index will be unmodified, and the keys for
     * 'from' will remain.  Assumes that the index has not changed since prepareUpdate was
     * called.  If the index was changed, we may return an error, as our ticket may have been
     * invalidated.
     *
     * 'numInserted' will be set to the number of keys inserted into the index for the document.
     * 'numDeleted' will be set to the number of keys removed from the index for the document.
     */
    Status doUpdate(OperationContext* opCtx,
                    RecoveryUnit& ru,
                    const CollectionPtr& coll,
                    const IndexCatalogEntry* entry,
                    const UpdateTicket& ticket,
                    int64_t* numInserted,
                    int64_t* numDeleted);

    RecordId findSingle(OperationContext* opCtx,
                        RecoveryUnit& ru,
                        const CollectionPtr& collection,
                        const IndexCatalogEntry* entry,
                        const BSONObj& key) const;

    /**
     * Returns an unpositioned cursor over 'this' index.
     */
    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           RecoveryUnit& ru,
                                                           bool isForward = true) const;


    /**
     * Given the set of keys, multikeyMetadataKeys and multikeyPaths generated by a particular
     * document, return 'true' if the index should be marked as multikey and 'false' otherwise.
     */
    virtual bool shouldMarkIndexAsMultikey(size_t numberOfKeys,
                                           const KeyStringSet& multikeyMetadataKeys,
                                           const MultikeyPaths& multikeyPaths) const;

    /**
     * Provides direct access to the SortedDataInterface. This should not be used to insert
     * documents into an index, except for testing purposes.
     */
    SortedDataInterface* getSortedDataInterface() const {
        return _newInterface.get();
    }


    //
    // Implementations of general IndexAccessMethod API.
    //

    SortedDataIndexAccessMethod* asSortedData() final {
        return this;
    }
    const SortedDataIndexAccessMethod* asSortedData() const final {
        return this;
    }

    Status insert(OperationContext* opCtx,
                  SharedBufferFragmentBuilder& pooledBufferBuilder,
                  const CollectionPtr& coll,
                  const IndexCatalogEntry* entry,
                  const std::vector<BsonRecord>& bsonRecords,
                  const InsertDeleteOptions& options,
                  int64_t* numInserted) final;

    void remove(OperationContext* opCtx,
                SharedBufferFragmentBuilder& pooledBufferBuilder,
                const CollectionPtr& coll,
                const IndexCatalogEntry* entry,
                const BSONObj& obj,
                const RecordId& loc,
                bool logIfError,
                const InsertDeleteOptions& options,
                int64_t* numDeleted,
                CheckRecordId checkRecordId) final;

    Status update(OperationContext* opCtx,
                  RecoveryUnit& ru,
                  SharedBufferFragmentBuilder& pooledBufferBuilder,
                  const BSONObj& oldDoc,
                  const BSONObj& newDoc,
                  const RecordId& loc,
                  const CollectionPtr& coll,
                  const IndexCatalogEntry* entry,
                  const InsertDeleteOptions& options,
                  int64_t* numInserted,
                  int64_t* numDeleted) final;

    Status initializeAsEmpty() final;

    IndexValidateResults validate(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        const CollectionValidation::ValidationOptions& options) const final;

    int64_t numKeys(OperationContext* opCtx, RecoveryUnit& ru) const final;

    bool appendCustomStats(OperationContext* opCtx,
                           RecoveryUnit& ru,
                           BSONObjBuilder* result,
                           double scale) const final;

    long long getSpaceUsedBytes(OperationContext* opCtx, RecoveryUnit& ru) const final;

    long long getFreeStorageBytes(OperationContext* opCtx, RecoveryUnit& ru) const final;

    /**
     * Returns an estimated number of bytes when doing a dry run.
     */
    StatusWith<int64_t> compact(OperationContext* opCtx,
                                RecoveryUnit& ru,
                                const CompactOptions& options) final;

    Status truncate(OperationContext* opCtx, RecoveryUnit& ru) final;

    std::shared_ptr<Ident> getSharedIdent() const final;

    void setIdent(std::shared_ptr<Ident> newIdent) final;

    Status applyIndexBuildSideWrite(OperationContext* opCtx,
                                    const CollectionPtr& coll,
                                    const IndexCatalogEntry* entry,
                                    const BSONObj& operation,
                                    const InsertDeleteOptions& options,
                                    KeyHandlerFn&& onDuplicateKey,
                                    int64_t* keysInserted,
                                    int64_t* keysDeleted) final;

    std::unique_ptr<BulkBuilder> initiateBulk(const IndexCatalogEntry* entry,
                                              size_t maxMemoryUsageBytes,
                                              const boost::optional<IndexStateInfo>& stateInfo,
                                              const DatabaseName& dbName,
                                              const IndexBuildMethodEnum& method) final;

protected:
    /**
     * Perform some initial validation on the document to ensure it can be indexed before calling
     * the implementation-specific 'doGetKeys' method.
     */
    virtual void validateDocument(const CollectionPtr& collection,
                                  const BSONObj& obj,
                                  const BSONObj& keyPattern) const;

    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index type supports tracking path-level multikey information, then this function resizes
     * 'multikeyPaths' to have the same number of elements as the index key pattern and fills each
     * element with the prefixes of the indexed field that would cause this index to be multikey as
     * a result of inserting 'keys'.
     *
     * If the 'multikeyMetadataKeys' pointer is non-null, then the function will populate the
     * BSONObjSet with any multikey metadata keys generated while processing the document. These
     * keys are not associated with the document itself, but instead represent multi-key path
     * information that must be stored in a reserved keyspace within the index.
     */
    virtual void doGetKeys(OperationContext* opCtx,
                           const CollectionPtr& collection,
                           const IndexCatalogEntry* entry,
                           SharedBufferFragmentBuilder& pooledBufferBuilder,
                           const BSONObj& obj,
                           GetKeysContext context,
                           KeyStringSet* keys,
                           KeyStringSet* multikeyMetadataKeys,
                           MultikeyPaths* multikeyPaths,
                           const boost::optional<RecordId>& id) const = 0;

private:
    class BaseBulkBuilder;
    class PrimaryDrivenBulkBuilder;
    class HybridBulkBuilder;

    /**
     * Removes a single key from the index.
     *
     * Used by remove() only.
     */
    void removeOneKey(OperationContext* opCtx,
                      RecoveryUnit& ru,
                      const IndexCatalogEntry* entry,
                      const key_string::Value& keyString,
                      bool dupsAllowed) const;

    Status _indexKeysOrWriteToSideTable(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        const IndexCatalogEntry* entry,
                                        const KeyStringSet& keys,
                                        const KeyStringSet& multikeyMetadataKeys,
                                        const MultikeyPaths& multikeyPaths,
                                        const BSONObj& obj,
                                        const InsertDeleteOptions& options,
                                        int64_t* keysInsertedOut);

    void _unindexKeysOrWriteToSideTable(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const IndexCatalogEntry* entry,
                                        const KeyStringSet& keys,
                                        const BSONObj& obj,
                                        bool logIfError,
                                        int64_t* keysDeletedOut,
                                        InsertDeleteOptions options,
                                        CheckRecordId checkRecordId);

    const std::unique_ptr<SortedDataInterface> _newInterface;
};

}  // namespace mongo
