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
 * IndexCatalog implementation used for testing only.
 * Unit tests may override functions selectively to use as mock objects.
 */
class IndexCatalogNoop : public IndexCatalog {
public:
    Status init(OperationContext* const opCtx) override {
        return Status::OK();
    }

    bool haveAnyIndexes() const override {
        return false;
    }

    bool haveAnyIndexesInProgress() const override {
        return false;
    }

    int numIndexesTotal(OperationContext* const opCtx) const override {
        return 0;
    }

    int numIndexesReady(OperationContext* const opCtx) const override {
        return 0;
    }

    int numIndexesInProgress(OperationContext* const opCtx) const override {
        return 0;
    }

    bool haveIdIndex(OperationContext* const opCtx) const override {
        return false;
    }

    BSONObj getDefaultIdIndexSpec() const override {
        return {};
    }

    IndexDescriptor* findIdIndex(OperationContext* const opCtx) const override {
        return nullptr;
    }

    IndexDescriptor* findIndexByName(OperationContext* const opCtx,
                                     const StringData name,
                                     const bool includeUnfinishedIndexes = false) const override {
        return nullptr;
    }

    IndexDescriptor* findIndexByKeyPatternAndCollationSpec(
        OperationContext* const opCtx,
        const BSONObj& key,
        const BSONObj& collationSpec,
        const bool includeUnfinishedIndexes = false) const override {
        return nullptr;
    }

    void findIndexesByKeyPattern(
        OperationContext* const opCtx,
        const BSONObj& key,
        const bool includeUnfinishedIndexes,
        std::vector<const IndexDescriptor*>* const matches) const override {}

    IndexDescriptor* findShardKeyPrefixedIndex(OperationContext* const opCtx,
                                               const BSONObj& shardKey,
                                               const bool requireSingleKey) const override {
        return nullptr;
    }

    void findIndexByType(OperationContext* const opCtx,
                         const std::string& type,
                         std::vector<const IndexDescriptor*>& matches,
                         const bool includeUnfinishedIndexes = false) const override {}

    const IndexDescriptor* refreshEntry(OperationContext* const opCtx,
                                        const IndexDescriptor* const oldDesc) override {
        return nullptr;
    }

    const IndexCatalogEntry* getEntry(const IndexDescriptor* const desc) const override {
        return nullptr;
    }

    std::shared_ptr<const IndexCatalogEntry> getEntryShared(const IndexDescriptor*) const override {
        return nullptr;
    }

    std::vector<std::shared_ptr<const IndexCatalogEntry>> getAllReadyEntriesShared()
        const override {
        return {};
    }

    std::unique_ptr<IndexIterator> getIndexIterator(
        OperationContext* const opCtx, const bool includeUnfinishedIndexes) const override {
        return {};
    }

    IndexCatalogEntry* createIndexEntry(OperationContext* opCtx,
                                        std::unique_ptr<IndexDescriptor> descriptor,
                                        bool initFromDisk,
                                        bool isReadyIndex) override {
        return nullptr;
    }

    StatusWith<BSONObj> createIndexOnEmptyCollection(OperationContext* const opCtx,
                                                     const BSONObj spec) override {
        return spec;
    }

    StatusWith<BSONObj> prepareSpecForCreate(OperationContext* const opCtx,
                                             const BSONObj& original) const override {
        return original;
    }

    std::vector<BSONObj> removeExistingIndexes(OperationContext* const opCtx,
                                               const std::vector<BSONObj>& indexSpecsToBuild,
                                               const bool removeIndexBuildsToo) const override {
        return indexSpecsToBuild;
    }

    std::vector<BSONObj> removeExistingIndexesNoChecks(
        OperationContext* const opCtx,
        const std::vector<BSONObj>& indexSpecsToBuild) const override {
        return {};
    }

    void dropAllIndexes(OperationContext* opCtx,
                        bool includingIdIndex,
                        std::function<void(const IndexDescriptor*)> onDropFn) override {}

    void dropAllIndexes(OperationContext* opCtx, bool includingIdIndex) override {}

    Status dropIndex(OperationContext* const opCtx, const IndexDescriptor* const desc) override {
        return Status::OK();
    }

    Status dropIndexEntry(OperationContext* opCtx, IndexCatalogEntry* entry) override {
        return Status::OK();
    }

    void deleteIndexFromDisk(OperationContext* opCtx, const std::string& indexName) override {}

    std::vector<BSONObj> getAndClearUnfinishedIndexes(OperationContext* const opCtx) {
        return {};
    }

    bool isMultikey(OperationContext* const opCtx, const IndexDescriptor* const idx) {
        return false;
    }

    MultikeyPaths getMultikeyPaths(OperationContext* const opCtx,
                                   const IndexDescriptor* const idx) {
        return {};
    }

    void setMultikeyPaths(OperationContext* const opCtx,
                          const IndexDescriptor* const desc,
                          const MultikeyPaths& multikeyPaths) override {}

    Status indexRecords(OperationContext* const opCtx,
                        const std::vector<BsonRecord>& bsonRecords,
                        int64_t* const keysInsertedOut) override {
        return Status::OK();
    }

    Status updateRecord(OperationContext* const opCtx,
                        const BSONObj& oldDoc,
                        const BSONObj& newDoc,
                        const RecordId& recordId,
                        int64_t* const keysInsertedOut,
                        int64_t* const keysDeletedOut) override {
        return Status::OK();
    };

    void unindexRecord(OperationContext* const opCtx,
                       const BSONObj& obj,
                       const RecordId& loc,
                       const bool noWarn,
                       int64_t* const keysDeletedOut) override {}

    virtual Status compactIndexes(OperationContext* opCtx) override {
        return Status::OK();
    }

    std::string getAccessMethodName(const BSONObj& keyPattern) override {
        return "";
    }

    std::string::size_type getLongestIndexNameLength(OperationContext* opCtx) const override {
        return 0U;
    }

    BSONObj fixIndexKey(const BSONObj& key) const override {
        return {};
    }

    void prepareInsertDeleteOptions(OperationContext* opCtx,
                                    const IndexDescriptor* desc,
                                    InsertDeleteOptions* options) const override {}

    void indexBuildSuccess(OperationContext* opCtx, IndexCatalogEntry* index) override {}
};

}  // namespace mongo
