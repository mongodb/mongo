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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_build_interceptor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/resumable_index_builds_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class Client;
class Collection;
class CollectionPtr;
class IndexDescriptor;
struct InsertDeleteOptions;

/**
 * IndexCatalogImpl is stored as a member of CollectionImpl. When the Collection is cloned this is
 * cloned with it by making shallow copies of the contained IndexCatalogEntry. The IndexCatalogEntry
 * instances are shared across multiple Collection instances.
 */
class IndexCatalogImpl : public IndexCatalog {
public:
    /**
     * Creates a cloned IndexCatalogImpl. Will make shallow copies of IndexCatalogEntryContainers so
     * the IndexCatalogEntry will be shared across IndexCatalogImpl instances'
     */
    std::unique_ptr<IndexCatalog> clone() const override;

    void init(OperationContext* opCtx,
              Collection* collection,
              bool isPointInTimeRead = false) override;

    bool haveAnyIndexes() const override;

    bool haveAnyIndexesInProgress() const override;

    int numIndexesTotal() const override;

    int numIndexesReady() const override;

    int numIndexesInProgress() const override;

    bool haveIdIndex(OperationContext* opCtx) const override;

    BSONObj getDefaultIdIndexSpec(const CollectionPtr& collection) const override;

    const IndexDescriptor* findIdIndex(OperationContext* opCtx) const override;

    const IndexDescriptor* findIndexByName(
        OperationContext* opCtx,
        StringData name,
        InclusionPolicy inclusionPolicy = InclusionPolicy::kReady) const override;

    const IndexDescriptor* findIndexByKeyPatternAndOptions(
        OperationContext* opCtx,
        const BSONObj& key,
        const BSONObj& indexSpec,
        InclusionPolicy inclusionPolicy = InclusionPolicy::kReady) const override;

    void findIndexesByKeyPattern(OperationContext* opCtx,
                                 const BSONObj& key,
                                 InclusionPolicy inclusionPolicy,
                                 std::vector<const IndexDescriptor*>* matches) const override;

    void findIndexByType(OperationContext* opCtx,
                         const std::string& type,
                         std::vector<const IndexDescriptor*>& matches,
                         InclusionPolicy inclusionPolicy = InclusionPolicy::kReady) const override;

    const IndexDescriptor* findIndexByIdent(
        OperationContext* opCtx,
        StringData ident,
        InclusionPolicy inclusionPolicy = InclusionPolicy::kReady) const override;

    const IndexDescriptor* refreshEntry(OperationContext* opCtx,
                                        Collection* collection,
                                        const IndexDescriptor* oldDesc,
                                        CreateIndexEntryFlags flags) override;

    const IndexCatalogEntry* getEntry(const IndexDescriptor* desc) const override;

    IndexCatalogEntry* getWritableEntryByName(
        OperationContext* opCtx,
        StringData name,
        InclusionPolicy inclusionPolicy = InclusionPolicy::kReady) override;

    IndexCatalogEntry* getWritableEntryByKeyPatternAndOptions(
        OperationContext* opCtx,
        const BSONObj& key,
        const BSONObj& indexSpec,
        InclusionPolicy inclusionPolicy = InclusionPolicy::kReady) override;

    std::shared_ptr<const IndexCatalogEntry> getEntryShared(const IndexDescriptor*) const override;

    std::vector<std::shared_ptr<const IndexCatalogEntry>> getAllReadyEntriesShared() const override;

    using IndexIterator = IndexCatalog::IndexIterator;
    std::unique_ptr<IndexIterator> getIndexIterator(OperationContext* opCtx,
                                                    InclusionPolicy inclusionPolicy) const override;

    IndexCatalogEntry* createIndexEntry(OperationContext* opCtx,
                                        Collection* collection,
                                        IndexDescriptor&& descriptor,
                                        CreateIndexEntryFlags flags) override;

    StatusWith<BSONObj> createIndexOnEmptyCollection(OperationContext* opCtx,
                                                     Collection* collection,
                                                     BSONObj spec) override;

    StatusWith<BSONObj> prepareSpecForCreate(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const BSONObj& original,
        const boost::optional<ResumeIndexInfo>& resumeInfo = boost::none) const override;

    std::vector<BSONObj> removeExistingIndexes(OperationContext* opCtx,
                                               const CollectionPtr& collection,
                                               const std::vector<BSONObj>& indexSpecsToBuild,
                                               bool removeIndexBuildsToo) const override;

    std::vector<BSONObj> removeExistingIndexesNoChecks(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const std::vector<BSONObj>& indexSpecsToBuild,
        bool removeInProgressIndexBuilds) const override;

    void dropIndexes(OperationContext* opCtx,
                     Collection* collection,
                     std::function<bool(const IndexDescriptor*)> matchFn,
                     std::function<void(const IndexDescriptor*)> onDropFn) override;

    void dropAllIndexes(OperationContext* opCtx,
                        Collection* collection,
                        bool includingIdIndex,
                        std::function<void(const IndexDescriptor*)> onDropFn) override;

    Status resetUnfinishedIndexForRecovery(OperationContext* opCtx,
                                           Collection* collection,
                                           IndexCatalogEntry* entry) override;

    Status dropUnfinishedIndex(OperationContext* opCtx,
                               Collection* collection,
                               IndexCatalogEntry* entry) override;

    Status dropIndexEntry(OperationContext* opCtx,
                          Collection* collection,
                          IndexCatalogEntry* entry) override;

    void deleteIndexFromDisk(OperationContext* opCtx,
                             Collection* collection,
                             const std::string& indexName) override;

    struct IndexKillCriteria {
        std::string ns;
        std::string name;
        BSONObj key;
    };

    void setMultikeyPaths(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          const IndexDescriptor* desc,
                          const KeyStringSet& multikeyMetadataKeys,
                          const MultikeyPaths& multikeyPaths) const override;

    Status indexRecords(OperationContext* opCtx,
                        const CollectionPtr& coll,
                        const std::vector<BsonRecord>& bsonRecords,
                        int64_t* keysInsertedOut) const override;

    Status updateRecord(OperationContext* opCtx,
                        const CollectionPtr& coll,
                        const BSONObj& oldDoc,
                        const BSONObj& newDoc,
                        const BSONObj* opDiff,
                        const RecordId& recordId,
                        int64_t* keysInsertedOut,
                        int64_t* keysDeletedOut) const override;

    void unindexRecord(OperationContext* opCtx,
                       const CollectionPtr& collection,
                       const BSONObj& obj,
                       const RecordId& loc,
                       bool noWarn,
                       int64_t* keysDeletedOut,
                       CheckRecordId checkRecordId = CheckRecordId::Off) const override;

    StatusWith<int64_t> compactIndexes(OperationContext* opCtx,
                                       const CompactOptions& options) const override;

    inline std::string getAccessMethodName(const BSONObj& keyPattern) override {
        return _getAccessMethodName(keyPattern);
    }

    std::string::size_type getLongestIndexNameLength(OperationContext* opCtx) const override;

    BSONObj fixIndexKey(const BSONObj& key) const override;

    void prepareInsertDeleteOptions(OperationContext* opCtx,
                                    const NamespaceString&,
                                    const IndexDescriptor* desc,
                                    InsertDeleteOptions* options) const override;

    void indexBuildSuccess(OperationContext* opCtx,
                           Collection* coll,
                           IndexCatalogEntry* index) override;

    /**
     * Returns a status indicating whether 'expression' is valid for use in a partial index
     * partialFilterExpression.
     */
    static Status checkValidFilterExpressions(const MatchExpression* expression);

private:
    static const BSONObj _idObj;  // { _id : 1 }

    /**
     * In addition to IndexNames::findPluginName, validates that it is a known index type.
     * If all you need is to check for a certain type, just use IndexNames::findPluginName.
     *
     * Uasserts if the index type is unknown.
     */
    std::string _getAccessMethodName(const BSONObj& keyPattern) const;

    Status _indexFilteredRecords(OperationContext* opCtx,
                                 const CollectionPtr& coll,
                                 const IndexCatalogEntry* index,
                                 const std::vector<BsonRecord>& bsonRecords,
                                 int64_t* keysInsertedOut) const;

    Status _indexRecords(OperationContext* opCtx,
                         const CollectionPtr& coll,
                         const IndexCatalogEntry* index,
                         const std::vector<BsonRecord>& bsonRecords,
                         int64_t* keysInsertedOut) const;

    Status _updateRecord(OperationContext* opCtx,
                         const CollectionPtr& coll,
                         const IndexCatalogEntry* index,
                         const BSONObj& oldDoc,
                         const BSONObj& newDoc,
                         const RecordId& recordId,
                         int64_t* keysInsertedOut,
                         int64_t* keysDeletedOut) const;

    void _unindexRecord(OperationContext* opCtx,
                        const CollectionPtr& collection,
                        const IndexCatalogEntry* entry,
                        const BSONObj& obj,
                        const RecordId& loc,
                        bool logIfError,
                        int64_t* keysDeletedOut,
                        CheckRecordId checkRecordId = CheckRecordId::Off) const;

    /**
     * Helper to remove the index from disk.
     * The index should be removed from the in-memory catalog beforehand.
     */
    void _deleteIndexFromDisk(OperationContext* opCtx,
                              Collection* collection,
                              const std::string& indexName,
                              std::shared_ptr<IndexCatalogEntry> entry);

    /**
     * Applies a set of transformations to the user-provided index object 'spec' to make it
     * conform to the standard for insertion.  Removes the '_id' field if it exists, applies
     * plugin-level transformations if appropriate, etc.
     */
    StatusWith<BSONObj> _fixIndexSpec(OperationContext* opCtx,
                                      const CollectionPtr& collection,
                                      const BSONObj& spec) const;

    Status _isSpecOk(OperationContext* opCtx,
                     const CollectionPtr& collection,
                     const BSONObj& spec) const;

    /**
     * Validates the 'original' index specification, alters any legacy fields and does plugin-level
     * transformations for text and geo indexes. Returns a clean spec ready to be built, or an
     * error.
     */
    StatusWith<BSONObj> _validateAndFixIndexSpec(OperationContext* opCtx,
                                                 const CollectionPtr& collection,
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
                                         const CollectionPtr& collection,
                                         const BSONObj& spec,
                                         InclusionPolicy inclusionPolicy) const;

    /**
     * Returns true if the replica set member's config has {buildIndexes:false} set, which means
     * we are not allowed to build non-_id indexes on this server, AND this index spec is for a
     * non-_id index.
     */
    Status _isNonIDIndexAndNotAllowedToBuild(OperationContext* opCtx, const BSONObj& spec) const;

    void _logInternalState(OperationContext* opCtx,
                           const CollectionPtr& collection,
                           long long numIndexesInCollectionCatalogEntry,
                           const std::vector<std::string>& indexNamesToDrop);

    /**
     * Returns a writable IndexCatalogEntry copy that will be returned by current and future calls
     * to this function. Any previous IndexCatalogEntry/IndexDescriptor pointers that were returned
     * may be invalidated.
     */
    IndexCatalogEntry* _getWritableEntry(const IndexDescriptor* descriptor);

    IndexCatalogEntryContainer _readyIndexes;
    IndexCatalogEntryContainer _buildingIndexes;
    IndexCatalogEntryContainer _frozenIndexes;
};
}  // namespace mongo
