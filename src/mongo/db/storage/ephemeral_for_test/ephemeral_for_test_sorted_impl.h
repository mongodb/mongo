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

#include "mongo/db/index/index_descriptor_fwd.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_radix_store.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {
namespace ephemeral_for_test {

class SortedDataBuilderBase : public SortedDataBuilderInterface {
public:
    SortedDataBuilderBase(OperationContext* opCtx,
                          bool dupsAllowed,
                          Ordering order,
                          KeyFormat rsKeyFormat,
                          const std::string& prefix,
                          const std::string& identEnd,
                          const IndexDescriptor* desc,
                          const std::string& indexName,
                          const BSONObj& keyPattern,
                          const BSONObj& collation);

protected:
    OperationContext* _opCtx;
    bool _dupsAllowed;
    // Order of the keys.
    Ordering _order;
    // RecordId format of the related record store
    KeyFormat _rsKeyFormat;
    // Prefix and identEnd for the ident.
    std::string _prefix;
    std::string _identEnd;
    // Index metadata.
    const IndexDescriptor* _desc;
    const std::string _indexName;
    const BSONObj _keyPattern;
    const BSONObj _collation;
};

class SortedDataBuilderUnique : public SortedDataBuilderBase {
public:
    using SortedDataBuilderBase::SortedDataBuilderBase;
    Status addKey(const KeyString::Value& keyString) override;
};

class SortedDataInterfaceBase : public SortedDataInterface {
public:
    // Truncate is not required at the time of writing but will be when the truncate command is
    // created
    Status truncate(RecoveryUnit* ru);
    SortedDataInterfaceBase(OperationContext* opCtx,
                            StringData ident,
                            KeyFormat rsKeyFormat,
                            const IndexDescriptor* desc);
    SortedDataInterfaceBase(const Ordering& ordering, StringData ident);
    bool appendCustomStats(OperationContext* opCtx,
                           BSONObjBuilder* output,
                           double scale) const override;
    long long getSpaceUsedBytes(OperationContext* opCtx) const override;
    long long getFreeStorageBytes(OperationContext* opCtx) const override {
        return 0;
    }
    bool isEmpty(OperationContext* opCtx) override;
    Status initAsEmpty(OperationContext* opCtx) override;

protected:
    // These two are the same as before.
    std::string _prefix;
    std::string _identEnd;
    // Index metadata.
    const IndexDescriptor* _desc;
    const std::string _indexName;
    const BSONObj _keyPattern;
    const BSONObj _collation;
    // These are the keystring representations of the _prefix and the _identEnd.
    std::string _KSForIdentStart;
    std::string _KSForIdentEnd;
    // Whether or not the index is partial
    bool _isPartial;
};

class SortedDataInterfaceUnique : public SortedDataInterfaceBase {
public:
    SortedDataInterfaceUnique(OperationContext* opCtx,
                              StringData ident,
                              KeyFormat rsKeyFormat,
                              const IndexDescriptor* desc);
    SortedDataInterfaceUnique(const Ordering& ordering, StringData ident);
    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) override;
    StatusWith<bool> insert(OperationContext* opCtx,
                            const KeyString::Value& keyString,
                            bool dupsAllowed) override;
    void unindex(OperationContext* opCtx,
                 const KeyString::Value& keyString,
                 bool dupsAllowed) override;
    Status dupKeyCheck(OperationContext* opCtx, const KeyString::Value& keyString) override;
    void fullValidate(OperationContext* opCtx,
                      long long* numKeysOut,
                      IndexValidateResults* fullResults) const override;
    std::unique_ptr<mongo::SortedDataInterface::Cursor> newCursor(
        OperationContext* opCtx, bool isForward = true) const override;
};

class SortedDataBuilderStandard : public SortedDataBuilderBase {
public:
    using SortedDataBuilderBase::SortedDataBuilderBase;
    Status addKey(const KeyString::Value& keyString) override;
};

class SortedDataInterfaceStandard : public SortedDataInterfaceBase {
public:
    SortedDataInterfaceStandard(OperationContext* opCtx,
                                StringData ident,
                                KeyFormat rsKeyFormat,
                                const IndexDescriptor* desc);
    SortedDataInterfaceStandard(const Ordering& ordering, StringData ident);
    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) override;
    StatusWith<bool> insert(OperationContext* opCtx,
                            const KeyString::Value& keyString,
                            bool dupsAllowed) override;
    void unindex(OperationContext* opCtx,
                 const KeyString::Value& keyString,
                 bool dupsAllowed) override;
    Status dupKeyCheck(OperationContext* opCtx, const KeyString::Value& keyString) override;
    void fullValidate(OperationContext* opCtx,
                      long long* numKeysOut,
                      IndexValidateResults* fullResults) const override;
    std::unique_ptr<mongo::SortedDataInterface::Cursor> newCursor(
        OperationContext* opCtx, bool isForward = true) const override;
};
}  // namespace ephemeral_for_test
}  // namespace mongo
