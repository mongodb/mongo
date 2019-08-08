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

#include <wiredtiger.h>

#include "mongo/base/status_with.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

namespace mongo {

class IndexCatalogEntry;
class IndexDescriptor;
struct WiredTigerItem;

class WiredTigerIndex : public SortedDataInterface {
public:
    /**
     * Parses index options for wired tiger configuration string suitable for table creation.
     * The document 'options' is typically obtained from the 'storageEngine.wiredTiger' field
     * of an IndexDescriptor's info object.
     */
    static StatusWith<std::string> parseIndexOptions(const BSONObj& options);

    /**
     * Creates the "app_metadata" string for the index from the index descriptor, to be stored
     * in WiredTiger's metadata. The output string is of the form:
     * ",app_metadata=(...)," and can be appended to the config strings for WiredTiger's API calls.
     */
    static std::string generateAppMetadataString(const IndexDescriptor& desc);

    /**
     * Creates a configuration string suitable for 'config' parameter in WT_SESSION::create().
     * Configuration string is constructed from:
     *     built-in defaults
     *     'sysIndexConfig'
     *     'collIndexConfig'
     *     storageEngine.wiredTiger.configString in index descriptor's info object.
     * Performs simple validation on the supplied parameters.
     * Returns error status if validation fails.
     * Note that even if this function returns an OK status, WT_SESSION:create() may still
     * fail with the constructed configuration string.
     */
    static StatusWith<std::string> generateCreateString(const std::string& engineName,
                                                        const std::string& sysIndexConfig,
                                                        const std::string& collIndexConfig,
                                                        const IndexDescriptor& desc,
                                                        bool isPrefixed);

    /**
     * Creates a WiredTiger table suitable for implementing a MongoDB index.
     * 'config' should be created with generateCreateString().
     */
    static int Create(OperationContext* opCtx, const std::string& uri, const std::string& config);

    WiredTigerIndex(OperationContext* ctx,
                    const std::string& uri,
                    const IndexDescriptor* desc,
                    KVPrefix prefix,
                    bool readOnly);

    virtual Status insert(OperationContext* opCtx,
                          const BSONObj& key,
                          const RecordId& id,
                          bool dupsAllowed);

    virtual Status insert(OperationContext* opCtx,
                          const KeyString::Value& keyString,
                          const RecordId& id,
                          bool dupsAllowed);

    virtual void unindex(OperationContext* opCtx,
                         const BSONObj& key,
                         const RecordId& id,
                         bool dupsAllowed);

    virtual void unindex(OperationContext* opCtx,
                         const KeyString::Value& keyString,
                         const RecordId& id,
                         bool dupsAllowed);

    virtual void fullValidate(OperationContext* opCtx,
                              long long* numKeysOut,
                              ValidateResults* fullResults) const;
    virtual bool appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* output,
                                   double scale) const;
    virtual Status dupKeyCheck(OperationContext* opCtx, const BSONObj& key);

    virtual bool isEmpty(OperationContext* opCtx);

    virtual Status touch(OperationContext* opCtx) const;

    virtual long long getSpaceUsedBytes(OperationContext* opCtx) const;

    virtual Status initAsEmpty(OperationContext* opCtx);

    virtual Status compact(OperationContext* opCtx);

    const std::string& uri() const {
        return _uri;
    }

    // WiredTigerIndex additions

    virtual bool isDup(OperationContext* opCtx, WT_CURSOR* c, const BSONObj& key);

    uint64_t tableId() const {
        return _tableId;
    }

    const NamespaceString& collectionNamespace() const {
        return _collectionNamespace;
    }

    std::string indexName() const {
        return _indexName;
    }

    const BSONObj& keyPattern() const {
        return _keyPattern;
    }

    bool isIdIndex() const {
        return _isIdIndex;
    }

    virtual bool unique() const = 0;
    virtual bool isTimestampSafeUniqueIdx() const = 0;

protected:
    virtual Status _insert(OperationContext* opCtx,
                           WT_CURSOR* c,
                           const KeyString::Value& keyString,
                           const RecordId& id,
                           bool dupsAllowed) = 0;

    virtual void _unindex(OperationContext* opCtx,
                          WT_CURSOR* c,
                          const KeyString::Value& keyString,
                          const RecordId& id,
                          bool dupsAllowed) = 0;

    void setKey(WT_CURSOR* cursor, const WT_ITEM* item);
    void getKey(WT_CURSOR* cursor, WT_ITEM* key);

    /*
     * Determines the data format version from application metadata and verifies compatibility.
     * Returns the corresponding KeyString version.
     */
    KeyString::Version _handleVersionInfo(OperationContext* ctx,
                                          const std::string& uri,
                                          const IndexDescriptor* desc,
                                          bool isReadOnly);

    class BulkBuilder;
    class StandardBulkBuilder;
    class UniqueBulkBuilder;

    /*
     * The data format version is effectively const after the WiredTigerIndex instance is
     * constructed.
     */
    int _dataFormatVersion;
    std::string _uri;
    uint64_t _tableId;
    const NamespaceString _collectionNamespace;
    const std::string _indexName;
    const BSONObj _keyPattern;
    KVPrefix _prefix;
    bool _isIdIndex;
};

class WiredTigerIndexUnique : public WiredTigerIndex {
public:
    WiredTigerIndexUnique(OperationContext* ctx,
                          const std::string& uri,
                          const IndexDescriptor* desc,
                          KVPrefix prefix,
                          bool readOnly = false);

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           bool forward) const override;

    SortedDataBuilderInterface* getBulkBuilder(OperationContext* opCtx, bool dupsAllowed) override;

    bool unique() const override {
        return true;
    }

    bool isTimestampSafeUniqueIdx() const override;

    bool isDup(OperationContext* opCtx, WT_CURSOR* c, const BSONObj& key) override;

    Status _insert(OperationContext* opCtx,
                   WT_CURSOR* c,
                   const KeyString::Value& keyString,
                   const RecordId& id,
                   bool dupsAllowed) override;

    Status _insertTimestampUnsafe(OperationContext* opCtx,
                                  WT_CURSOR* c,
                                  const KeyString::Value& keyString,
                                  const RecordId& id,
                                  bool dupsAllowed);

    Status _insertTimestampSafe(OperationContext* opCtx,
                                WT_CURSOR* c,
                                const KeyString::Value& keyString,
                                bool dupsAllowed);

    void _unindex(OperationContext* opCtx,
                  WT_CURSOR* c,
                  const KeyString::Value& keyString,
                  const RecordId& id,
                  bool dupsAllowed) override;

    void _unindexTimestampUnsafe(OperationContext* opCtx,
                                 WT_CURSOR* c,
                                 const KeyString::Value& keyString,
                                 const RecordId& id,
                                 bool dupsAllowed);

    void _unindexTimestampSafe(OperationContext* opCtx,
                               WT_CURSOR* c,
                               const KeyString::Value& keyString,
                               bool dupsAllowed);

private:
    /**
     * If this returns true, the cursor will be positioned on the first matching the input 'key'.
     */
    bool _keyExists(OperationContext* opCtx, WT_CURSOR* c, const char* buffer, size_t size);

    bool _partial;
};

class WiredTigerIndexStandard : public WiredTigerIndex {
public:
    WiredTigerIndexStandard(OperationContext* ctx,
                            const std::string& uri,
                            const IndexDescriptor* desc,
                            KVPrefix prefix,
                            bool readOnly = false);

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           bool forward) const override;

    SortedDataBuilderInterface* getBulkBuilder(OperationContext* opCtx, bool dupsAllowed) override;

    bool unique() const override {
        return false;
    }

    bool isTimestampSafeUniqueIdx() const override {
        return false;
    }

    Status _insert(OperationContext* opCtx,
                   WT_CURSOR* c,
                   const KeyString::Value& keyString,
                   const RecordId& id,
                   bool dupsAllowed) override;

    void _unindex(OperationContext* opCtx,
                  WT_CURSOR* c,
                  const KeyString::Value& keyString,
                  const RecordId& id,
                  bool dupsAllowed) override;
};

}  // namespace mongo
