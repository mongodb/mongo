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

#include "mongo/db/catalog/collection_catalog_entry.h"

namespace mongo {
class CollectionCatalogEntryMock : public CollectionCatalogEntry {
public:
    CollectionCatalogEntryMock(StringData ns) : CollectionCatalogEntry(ns) {}

    CollectionOptions getCollectionOptions(OperationContext* opCtx) const {
        return CollectionOptions();
    }

    int getTotalIndexCount(OperationContext* opCtx) const {
        return 0;
    }

    int getCompletedIndexCount(OperationContext* opCtx) const {
        return 0;
    }

    int getMaxAllowedIndexes() const {
        return 0;
    }

    void getAllIndexes(OperationContext* opCtx, std::vector<std::string>* names) const {}

    void getReadyIndexes(OperationContext* opCtx, std::vector<std::string>* names) const {}

    void getAllUniqueIndexes(OperationContext* opCtx, std::vector<std::string>* names) const {}

    BSONObj getIndexSpec(OperationContext* opCtx, StringData idxName) const {
        return BSONObj();
    }

    bool isIndexMultikey(OperationContext* opCtx,
                         StringData indexName,
                         MultikeyPaths* multikeyPaths) const {
        return false;
    }

    bool setIndexIsMultikey(OperationContext* opCtx,
                            StringData indexName,
                            const MultikeyPaths& multikeyPaths) {
        return false;
    }

    RecordId getIndexHead(OperationContext* opCtx, StringData indexName) const {
        return RecordId(0);
    }

    void setIndexHead(OperationContext* opCtx, StringData indexName, const RecordId& newHead) {}

    bool isIndexReady(OperationContext* opCtx, StringData indexName) const {
        return false;
    }

    bool isIndexPresent(OperationContext* opCtx, StringData indexName) const {
        return false;
    }

    KVPrefix getIndexPrefix(OperationContext* opCtx, StringData indexName) const {
        MONGO_UNREACHABLE;
    }

    Status removeIndex(OperationContext* opCtx, StringData indexName) {
        return Status::OK();
    }

    Status prepareForIndexBuild(OperationContext* opCtx,
                                const IndexDescriptor* spec,
                                IndexBuildProtocol indexBuildProtocol,
                                bool isBackgroundSecondaryBuild) {
        return Status::OK();
    }

    bool isTwoPhaseIndexBuild(OperationContext* opCtx, StringData indexName) const {
        return false;
    }

    long getIndexBuildVersion(OperationContext* opCtx, StringData indexName) const {
        return 0;
    }

    void setIndexBuildScanning(OperationContext* opCtx,
                               StringData indexName,
                               std::string sideWritesIdent,
                               boost::optional<std::string> constraintViolationsIdent) {}

    bool isIndexBuildScanning(OperationContext* opCtx, StringData indexName) const {
        return false;
    }

    void setIndexBuildDraining(OperationContext* opCtx, StringData indexName) {}

    bool isIndexBuildDraining(OperationContext* opCtx, StringData indexName) const {
        return false;
    }

    void indexBuildSuccess(OperationContext* opCtx, StringData indexName) {}

    boost::optional<std::string> getSideWritesIdent(OperationContext* opCtx,
                                                    StringData indexName) const {
        return boost::none;
    }

    boost::optional<std::string> getConstraintViolationsIdent(OperationContext* opCtx,
                                                              StringData indexName) const {
        return boost::none;
    }

    void updateTTLSetting(OperationContext* opCtx, StringData idxName, long long newExpireSeconds) {
    }

    void updateIndexMetadata(OperationContext* opCtx, const IndexDescriptor* desc) {}

    void updateFlags(OperationContext* opCtx, int newValue) {}

    void updateValidator(OperationContext* opCtx,
                         const BSONObj& validator,
                         StringData validationLevel,
                         StringData validationAction) {}

    void setIsTemp(OperationContext* opCtx, bool isTemp) {}

    bool isEqualToMetadataUUID(OperationContext* opCtx, OptionalCollectionUUID uuid) {
        return false;
    }

    void updateCappedSize(OperationContext* opCtx, long long size) {}

    void setIndexKeyStringWithLongTypeBitsExistsOnDisk(OperationContext* opCtx) {}

    RecordStore* getRecordStore() {
        return nullptr;
    }
    const RecordStore* getRecordStore() const {
        return nullptr;
    }
};
}
