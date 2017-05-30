// kv_collection_catalog_entry.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class KVCatalog;
class KVEngine;

class KVCollectionCatalogEntry final : public BSONCollectionCatalogEntry {
public:
    KVCollectionCatalogEntry(KVEngine* engine,
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

    void setIndexHead(OperationContext* opCtx, StringData indexName, const RecordId& newHead) final;

    Status removeIndex(OperationContext* opCtx, StringData indexName) final;

    Status prepareForIndexBuild(OperationContext* opCtx, const IndexDescriptor* spec) final;

    void indexBuildSuccess(OperationContext* opCtx, StringData indexName) final;

    void updateTTLSetting(OperationContext* opCtx,
                          StringData idxName,
                          long long newExpireSeconds) final;

    void updateFlags(OperationContext* opCtx, int newValue) final;

    void updateValidator(OperationContext* opCtx,
                         const BSONObj& validator,
                         StringData validationLevel,
                         StringData validationAction) final;

    void updateCappedSize(OperationContext*, long long int) final;

    void addUUID(OperationContext* opCtx, CollectionUUID uuid, Collection* coll) final;

    void removeUUID(OperationContext* opCtx) final;

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

    KVEngine* _engine;    // not owned
    KVCatalog* _catalog;  // not owned
    std::string _ident;
    std::unique_ptr<RecordStore> _recordStore;  // owned
};
}
