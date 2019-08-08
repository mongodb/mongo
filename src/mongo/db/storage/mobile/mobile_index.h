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

#include <set>

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/platform/basic.h"

namespace mongo {

class MobileIndex : public SortedDataInterface {
public:
    MobileIndex(OperationContext* opCtx, const IndexDescriptor* desc, const std::string& ident);

    MobileIndex(bool isUnique,
                const Ordering& ordering,
                const std::string& ident,
                const std::string& collectionNamespace,
                const std::string& indexName);

    virtual ~MobileIndex() {}

    Status insert(OperationContext* opCtx,
                  const BSONObj& key,
                  const RecordId& recId,
                  bool dupsAllowed) override;

    Status insert(OperationContext* opCtx,
                  const KeyString::Value& keyString,
                  const RecordId& recId,
                  bool dupsAllowed) override;

    void unindex(OperationContext* opCtx,
                 const BSONObj& key,
                 const RecordId& recId,
                 bool dupsAllowed) override;

    void unindex(OperationContext* opCtx,
                 const KeyString::Value& keyString,
                 const RecordId& recId,
                 bool dupsAllowed) override;

    void fullValidate(OperationContext* opCtx,
                      long long* numKeysOut,
                      ValidateResults* fullResults) const override;

    bool appendCustomStats(OperationContext* opCtx,
                           BSONObjBuilder* output,
                           double scale) const override;

    long long getSpaceUsedBytes(OperationContext* opCtx) const override;

    long long numEntries(OperationContext* opCtx) const override;

    bool isEmpty(OperationContext* opCtx) override;

    Status initAsEmpty(OperationContext* opCtx) override;

    Status dupKeyCheck(OperationContext* opCtx, const BSONObj& key) override;

    // Beginning of MobileIndex-specific methods

    /**
     * Creates a SQLite table suitable for a new Mobile index.
     */
    static Status create(OperationContext* opCtx, const std::string& ident);

    /**
     * Performs the insert into the table with the given key and value.
     */
    template <typename ValueType>
    Status doInsert(OperationContext* opCtx,
                    const char* keyBuffer,
                    size_t keySize,
                    const KeyString::TypeBits& typeBits,
                    const ValueType& value,
                    bool isTransactional = true);

    bool isUnique() {
        return _isUnique;
    }

    std::string getIdent() const {
        return _ident;
    }

protected:
    bool _isDup(OperationContext* opCtx, const BSONObj& key);

    /**
     * Performs the deletion from the table matching the given key.
     */
    void _doDelete(OperationContext* opCtx,
                   const char* keyBuffer,
                   size_t keySize,
                   KeyString::Builder* value = nullptr);

    virtual Status _insert(OperationContext* opCtx,
                           const KeyString::Value& keyString,
                           const RecordId& recId,
                           bool dupsAllowed) = 0;

    virtual void _unindex(OperationContext* opCtx,
                          const KeyString::Value& keyString,
                          const RecordId& recId,
                          bool dupsAllowed) = 0;

    class BulkBuilderBase;
    class BulkBuilderStandard;
    class BulkBuilderUnique;

    const bool _isUnique;
    const Ordering _ordering;
    const std::string _ident;
    const NamespaceString _collectionNamespace;
    const std::string _indexName;
    const BSONObj _keyPattern;
};

class MobileIndexStandard final : public MobileIndex {
public:
    MobileIndexStandard(OperationContext* opCtx,
                        const IndexDescriptor* desc,
                        const std::string& ident);

    SortedDataBuilderInterface* getBulkBuilder(OperationContext* opCtx, bool dupsAllowed) override;

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           bool isForward) const override;

protected:
    Status _insert(OperationContext* opCtx,
                   const KeyString::Value& keyString,
                   const RecordId& recId,
                   bool dupsAllowed) override;

    void _unindex(OperationContext* opCtx,
                  const KeyString::Value& keyString,
                  const RecordId& recId,
                  bool dupsAllowed) override;
};

class MobileIndexUnique final : public MobileIndex {
public:
    MobileIndexUnique(OperationContext* opCtx,
                      const IndexDescriptor* desc,
                      const std::string& ident);

    SortedDataBuilderInterface* getBulkBuilder(OperationContext* opCtx, bool dupsAllowed) override;

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           bool isForward) const override;

protected:
    Status _insert(OperationContext* opCtx,
                   const KeyString::Value& keyString,
                   const RecordId& recId,
                   bool dupsAllowed) override;

    void _unindex(OperationContext* opCtx,
                  const KeyString::Value& keyString,
                  const RecordId& recId,
                  bool dupsAllowed) override;

    const bool _isPartial = false;
};
}  // namespace mongo
