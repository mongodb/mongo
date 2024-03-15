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

#include "mongo/db/catalog/index_catalog.h"

namespace mongo {

/**
 * This class comprises a mock IndexCatalog for use in unit tests.
 */
class IndexCatalogMock : public IndexCatalog {
public:
    /**
     * Creates a cloned IndexCatalogMock.
     */
    std::unique_ptr<IndexCatalog> clone() const override {
        return std::make_unique<IndexCatalogMock>(*this);
    }

    void init(OperationContext*, Collection*, bool = false) {
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

    bool haveIdIndex(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

    BSONObj getDefaultIdIndexSpec(const CollectionPtr&) const override {
        MONGO_UNREACHABLE;
    }

    const IndexDescriptor* findIdIndex(OperationContext*) const override {
        MONGO_UNREACHABLE;
    }

    const IndexDescriptor* findIndexByName(
        OperationContext*, StringData, InclusionPolicy = InclusionPolicy::kReady) const override {
        MONGO_UNREACHABLE;
    }

    const IndexDescriptor* findIndexByKeyPatternAndOptions(
        OperationContext*,
        const BSONObj&,
        const BSONObj&,
        InclusionPolicy = InclusionPolicy::kReady) const override {
        MONGO_UNREACHABLE;
    }

    void findIndexesByKeyPattern(OperationContext*,
                                 const BSONObj&,
                                 InclusionPolicy,
                                 std::vector<const IndexDescriptor*>*) const override {
        MONGO_UNREACHABLE;
    }

    void findIndexByType(OperationContext*,
                         const std::string&,
                         std::vector<const IndexDescriptor*>&,
                         InclusionPolicy = InclusionPolicy::kReady) const override {
        MONGO_UNREACHABLE;
    }

    const IndexDescriptor* findIndexByIdent(
        OperationContext*, StringData, InclusionPolicy = InclusionPolicy::kReady) const override {
        MONGO_UNREACHABLE;
    }

    const IndexDescriptor* refreshEntry(OperationContext*,
                                        Collection*,
                                        const IndexDescriptor*,
                                        CreateIndexEntryFlags) override {
        MONGO_UNREACHABLE;
    }

    const IndexCatalogEntry* getEntry(const IndexDescriptor*) const override {
        MONGO_UNREACHABLE;
    }

    IndexCatalogEntry* getWritableEntryByName(OperationContext*,
                                              StringData,
                                              InclusionPolicy = InclusionPolicy::kReady) override {
        MONGO_UNREACHABLE;
    }

    IndexCatalogEntry* getWritableEntryByKeyPatternAndOptions(
        OperationContext*,
        const BSONObj&,
        const BSONObj&,
        InclusionPolicy = InclusionPolicy::kReady) override {
        MONGO_UNREACHABLE;
    }

    std::shared_ptr<const IndexCatalogEntry> getEntryShared(const IndexDescriptor*) const override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::shared_ptr<const IndexCatalogEntry>> getAllReadyEntriesShared()
        const override {
        MONGO_UNREACHABLE;
    }

    using IndexIterator = IndexCatalog::IndexIterator;
    std::unique_ptr<IndexIterator> getIndexIterator(OperationContext* opCtx,
                                                    InclusionPolicy) const override {
        return std::make_unique<AllIndexesIterator>(
            opCtx, std::make_unique<std::vector<const IndexCatalogEntry*>>());
    }

    IndexCatalogEntry* createIndexEntry(OperationContext*,
                                        Collection*,
                                        IndexDescriptor&&,
                                        CreateIndexEntryFlags) override {
        MONGO_UNREACHABLE;
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

    std::vector<BSONObj> removeExistingIndexesNoChecks(OperationContext*,
                                                       const CollectionPtr&,
                                                       const std::vector<BSONObj>&,
                                                       bool) const override {
        MONGO_UNREACHABLE;
    }

    void dropIndexes(OperationContext*,
                     Collection*,
                     std::function<bool(const IndexDescriptor*)>,
                     std::function<void(const IndexDescriptor*)>) override {
        MONGO_UNREACHABLE;
    }

    void dropAllIndexes(OperationContext*,
                        Collection*,
                        bool,
                        std::function<void(const IndexDescriptor*)>) override {
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
                          const IndexDescriptor*,
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
};
}  // namespace mongo
