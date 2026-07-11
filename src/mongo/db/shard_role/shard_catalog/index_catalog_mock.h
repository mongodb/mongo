// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/index/index_access_method.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry_mock.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"

#include <string_view>

namespace mongo {

/**
 * This class comprises a mock IndexCatalog for use in unit tests.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] IndexCatalogMock : public IndexCatalog {
public:
    /**
     * Creates a cloned IndexCatalogMock.
     */
    std::unique_ptr<IndexCatalog> clone() const override {
        return std::make_unique<IndexCatalogMock>(*this);
    }

    void init(OperationContext*, Collection*, bool = false) override {
        MONGO_UNREACHABLE;
    }

    void onDeregisterFromCatalog(ServiceContext* svcCtx, Collection* collection) override {
        MONGO_UNREACHABLE;
    }

    bool haveAnyIndexes() const override {
        MONGO_UNREACHABLE;
    }

    bool haveAnyIndexesInProgress() const override {
        MONGO_UNREACHABLE;
    }

    int numIndexesTotal() const override {
        MONGO_UNREACHABLE;
    }

    int numIndexesReady() const override {
        MONGO_UNREACHABLE;
    }

    int numIndexesInProgress() const override {
        MONGO_UNREACHABLE;
    }

    const doc_diff::IndexUpdateIdentifier* getIndexUpdateIdentifier() const override {
        MONGO_UNREACHABLE;
    }

    bool haveIdIndex(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

    BSONObj getDefaultIdIndexSpec(const CollectionPtr&) const override {
        MONGO_UNREACHABLE;
    }

    const IndexCatalogEntry* findIdIndex(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

    // Note that the inclusion policy is currently ignored for this mock implementation (all added
    // indexes are considered).
    const IndexCatalogEntry* findIndexByName(OperationContext*,
                                             std::string_view name,
                                             InclusionPolicy) const override {
        for (const auto& entry : _indexEntries) {
            if (entry->descriptor()->indexName() == name) {
                return entry.get();
            }
        }
        return nullptr;
    }

    const IndexCatalogEntry* findIndexByKeyPatternAndOptions(OperationContext*,
                                                             const BSONObj&,
                                                             const BSONObj&,
                                                             InclusionPolicy) const override {
        MONGO_UNREACHABLE;
    }

    void findIndexesByKeyPattern(OperationContext*,
                                 const BSONObj&,
                                 InclusionPolicy,
                                 std::vector<const IndexCatalogEntry*>*) const override {
        MONGO_UNREACHABLE;
    }

    void findIndexByType(OperationContext*,
                         const std::string&,
                         std::vector<const IndexCatalogEntry*>&,
                         InclusionPolicy) const override {
        MONGO_UNREACHABLE;
    }

    const IndexCatalogEntry* findIndexByIdent(OperationContext*,
                                              std::string_view ident,
                                              InclusionPolicy) const override {
        for (const auto& entry : _indexEntries) {
            if (entry->getIdent() == ident) {
                return entry.get();
            }
        }
        return nullptr;
    }

    const IndexCatalogEntry* refreshEntry(OperationContext*,
                                          Collection*,
                                          const IndexCatalogEntry*,
                                          CreateIndexEntryFlags) override {
        MONGO_UNREACHABLE;
    }

    IndexCatalogEntry* getWritableEntryByName(OperationContext*,
                                              std::string_view,
                                              InclusionPolicy) override {
        MONGO_UNREACHABLE;
    }

    IndexCatalogEntry* getWritableEntryByKeyPatternAndOptions(OperationContext*,
                                                              const BSONObj&,
                                                              const BSONObj&,
                                                              InclusionPolicy) override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::shared_ptr<const IndexCatalogEntry>> getEntriesShared(
        InclusionPolicy inclusionPolicy) const override {
        return {_indexEntries.begin(), _indexEntries.end()};
    }

    class IteratorMock : public IndexIterator {
    public:
        IteratorMock(IndexCatalogEntryContainer::const_iterator beginIt,
                     IndexCatalogEntryContainer::const_iterator endIt)
            : _it(beginIt), _endIt(endIt) {}

    private:
        const IndexCatalogEntry* _advance() override {
            return _it == _endIt ? nullptr : _it++->get();
        }

        IndexCatalogEntryContainer::const_iterator _it;
        IndexCatalogEntryContainer::const_iterator _endIt;
    };

    std::unique_ptr<IndexIterator> getIndexIterator(InclusionPolicy) const override {
        return std::make_unique<IteratorMock>(_indexEntries.begin(), _indexEntries.end());
    }

    IndexCatalogEntry* createIndexEntry(OperationContext* opCtx,
                                        Collection* collection,
                                        IndexDescriptor&& descriptor,
                                        CreateIndexEntryFlags) override {
        const auto ident = descriptor.indexName();
        auto entry = std::make_shared<IndexCatalogEntryMock>(
            opCtx, CollectionPtr(collection), ident, std::move(descriptor), false /* isFrozen */);

        auto save = entry.get();
        _indexEntries.add(std::move(entry));

        return save;
    }

    StatusWith<BSONObj> createIndexOnEmptyCollection(OperationContext*,
                                                     Collection*,
                                                     BSONObj) override {
        MONGO_UNREACHABLE;
    }

    StatusWith<BSONObj> prepareSpecForCreate(
        OperationContext*,
        const CollectionPtr&,
        const BSONObj&,
        const boost::optional<ResumeIndexInfo>& = boost::none) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<BSONObj> removeExistingIndexes(OperationContext*,
                                               const CollectionPtr&,
                                               const std::vector<BSONObj>&,
                                               bool) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<BSONObj> removeExistingIndexesNoChecks(
        OperationContext*,
        const CollectionPtr&,
        const std::vector<BSONObj>&,
        IndexCatalog::RemoveExistingIndexesFlags) const override {
        MONGO_UNREACHABLE;
    }

    void dropIndexes(OperationContext*,
                     Collection*,
                     std::function<bool(const IndexCatalogEntry*)>,
                     std::function<void(const IndexCatalogEntry*)>) override {
        MONGO_UNREACHABLE;
    }

    void dropAllIndexes(OperationContext*,
                        Collection*,
                        bool,
                        std::function<void(const IndexCatalogEntry*)>) override {
        MONGO_UNREACHABLE;
    }

    Status truncateAllIndexes(OperationContext*, Collection*) override {
        MONGO_UNREACHABLE;
    }

    Status resetUnfinishedIndexForRecovery(OperationContext*,
                                           Collection*,
                                           IndexCatalogEntry*) override {
        MONGO_UNREACHABLE;
    }

    Status dropUnfinishedIndex(OperationContext*, Collection*, IndexCatalogEntry*) override {
        MONGO_UNREACHABLE;
    }

    Status dropIndexEntry(OperationContext*, Collection*, IndexCatalogEntry*) override {
        MONGO_UNREACHABLE;
    }

    void deleteIndexFromDisk(OperationContext*, Collection*, const std::string&) override {
        MONGO_UNREACHABLE;
    }

    void setMultikeyPaths(OperationContext*,
                          const CollectionPtr&,
                          const IndexCatalogEntry*,
                          const KeyStringSet&,
                          const MultikeyPaths&) const override {
        MONGO_UNREACHABLE;
    }

    Status indexRecords(OperationContext*,
                        const CollectionPtr&,
                        const std::vector<BsonRecord>&,
                        int64_t*) const override {
        MONGO_UNREACHABLE;
    }

    Status updateRecord(OperationContext*,
                        const CollectionPtr&,
                        const BSONObj&,
                        const BSONObj&,
                        const BSONObj*,
                        const RecordId&,
                        int64_t*,
                        int64_t*) const override {
        MONGO_UNREACHABLE;
    }

    void unindexRecord(OperationContext*,
                       const CollectionPtr&,
                       const BSONObj&,
                       const RecordId&,
                       bool,
                       int64_t*,
                       CheckRecordId = CheckRecordId::Off) const override {
        MONGO_UNREACHABLE;
    }

    StatusWith<int64_t> compactIndexes(OperationContext*, const CompactOptions&) const override {
        MONGO_UNREACHABLE;
    }

    inline std::string getAccessMethodName(const BSONObj&) override {
        MONGO_UNREACHABLE;
    }

    std::string::size_type getLongestIndexNameLength(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

    BSONObj fixIndexKey(const BSONObj&) const override {
        MONGO_UNREACHABLE;
    }

    void prepareInsertDeleteOptions(OperationContext*,
                                    const NamespaceString&,
                                    const IndexDescriptor*,
                                    InsertDeleteOptions*) const override {
        MONGO_UNREACHABLE;
    }

    void indexBuildSuccess(OperationContext*, Collection*, IndexCatalogEntry*) override {
        MONGO_UNREACHABLE;
    }

    /**
     * Returns a status indicating whether 'expression' is valid for use in a partial index
     * partialFilterExpression.
     */
    static Status checkValidFilterExpressions(const MatchExpression*) {
        MONGO_UNREACHABLE;
    }

    IndexCatalogEntryContainer _indexEntries;
};
}  // namespace mongo
