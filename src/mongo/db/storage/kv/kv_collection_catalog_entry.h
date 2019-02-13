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

#include <memory>

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class KVCatalog;
class KVStorageEngineInterface;

class KVCollectionCatalogEntry final : public BSONCollectionCatalogEntry {
public:
    KVCollectionCatalogEntry(KVStorageEngineInterface* engine,
                             KVCatalog* catalog,
                             StringData ns,
                             StringData ident,
                             std::unique_ptr<RecordStore> rs);

    ~KVCollectionCatalogEntry() final;

    int getMaxAllowedIndexes() const final {
        return 64;
    };

    bool setIndexIsMultikey(OperationContext* opCtx,
                            StringData indexName,
                            const MultikeyPaths& multikeyPaths) final;

    // TODO SERVER-36385 Remove this function: we don't set the feature tracker bit in 4.4 because
    // 4.4 can only downgrade to 4.2 which can read long TypeBits.
    void setIndexKeyStringWithLongTypeBitsExistsOnDisk(OperationContext* opCtx) final;

    void setIndexHead(OperationContext* opCtx, StringData indexName, const RecordId& newHead) final;

    Status removeIndex(OperationContext* opCtx, StringData indexName) final;

    Status prepareForIndexBuild(OperationContext* opCtx,
                                const IndexDescriptor* spec,
                                IndexBuildProtocol indexBuildProtocol,
                                bool isBackgroundSecondaryBuild) final;

    bool isTwoPhaseIndexBuild(OperationContext* opCtx, StringData indexName) const final;

    long getIndexBuildVersion(OperationContext* opCtx, StringData indexName) const final;

    void setIndexBuildScanning(OperationContext* opCtx,
                               StringData indexName,
                               std::string sideWritesIdent,
                               boost::optional<std::string> constraintViolationsIdent) final;

    bool isIndexBuildScanning(OperationContext* opCtx, StringData indexName) const final;

    void setIndexBuildDraining(OperationContext* opCtx, StringData indexName) final;

    bool isIndexBuildDraining(OperationContext* opCtx, StringData indexName) const final;

    void indexBuildSuccess(OperationContext* opCtx, StringData indexName) final;

    boost::optional<std::string> getSideWritesIdent(OperationContext* opCtx,
                                                    StringData indexName) const final;

    boost::optional<std::string> getConstraintViolationsIdent(OperationContext* opCtx,
                                                              StringData indexName) const final;
    void updateTTLSetting(OperationContext* opCtx,
                          StringData idxName,
                          long long newExpireSeconds) final;

    void updateFlags(OperationContext* opCtx, int newValue) final;

    void updateIndexMetadata(OperationContext* opCtx, const IndexDescriptor* desc) final;

    void updateValidator(OperationContext* opCtx,
                         const BSONObj& validator,
                         StringData validationLevel,
                         StringData validationAction) final;

    void setIsTemp(OperationContext* opCtx, bool isTemp);

    void updateCappedSize(OperationContext*, long long int) final;

    bool isEqualToMetadataUUID(OperationContext* opCtx, OptionalCollectionUUID uuid) final;

    RecordStore* getRecordStore() {
        return _recordStore.get();
    }
    const RecordStore* getRecordStore() const {
        return _recordStore.get();
    }

protected:
    MetaData _getMetaData(OperationContext* opCtx) const final;

private:
    class AddIndexChange;
    class RemoveIndexChange;

    KVStorageEngineInterface* const _engine;  // not owned
    KVCatalog* _catalog;                      // not owned
    std::string _ident;
    std::unique_ptr<RecordStore> _recordStore;  // owned
};
}
