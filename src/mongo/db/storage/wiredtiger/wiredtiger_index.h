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
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

namespace mongo {

/**
 * Table of WiredTiger index types<->KeyString format<->Data format version:
 *
 * | Index Type                   | Key                             | Data Format Version        |
 * | ---------------------------- | ------------------------------- | -------------------------- |
 * | _id index                    | KeyString without RecordId      | index V1: 6, index V2: 8   |
 * | non-unique index             | KeyString with RecordId         | index V1: 6, index V2: 8   |
 * | unique secondary index (new) | KeyString with RecordId         | index V1: 13, index V2: 14 |
 * | unique secondary index (old) | KeyString with/without RecordId | index V1: 11, index V2: 12 |
 *
 * Starting in 4.2, unique indexes can be in format version 11 or 12. On upgrade to 4.2, an existing
 * format 6 unique index will upgrade to format 11 and an existing format 8 unique index will
 * upgrade to format 12.
 *
 * Starting in 6.0, any new unique index will be in format 13 or 14, which guarantees that all keys
 * are in the new format.
 */
const int kDataFormatV1KeyStringV0IndexVersionV1 = 6;
const int kDataFormatV2KeyStringV1IndexVersionV2 = 8;
const int kDataFormatV3KeyStringV0UniqueIndexVersionV1 = 11;
const int kDataFormatV4KeyStringV1UniqueIndexVersionV2 = 12;
const int kDataFormatV5KeyStringV0UniqueIndexVersionV1 = 13;
const int kDataFormatV6KeyStringV1UniqueIndexVersionV2 = 14;
const int kMinimumIndexVersion = kDataFormatV1KeyStringV0IndexVersionV1;
const int kMaximumIndexVersion = kDataFormatV6KeyStringV1UniqueIndexVersionV2;

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
                                                        const NamespaceString& collectionNamespace,
                                                        const IndexDescriptor& desc,
                                                        bool isLogged);

    /**
     * Creates a WiredTiger table suitable for implementing a MongoDB index.
     * 'config' should be created with generateCreateString().
     */
    static Status create(OperationContext* opCtx,
                         const std::string& uri,
                         const std::string& config);

    /**
     * Drops the specified WiredTiger table. This should only be used for resuming index builds.
     */
    static Status Drop(OperationContext* opCtx, const std::string& uri);

    /**
     * Constructs an index. The rsKeyFormat is the RecordId key format of the related RecordStore.
     */
    WiredTigerIndex(OperationContext* ctx,
                    const std::string& uri,
                    const UUID& collectionUUID,
                    StringData ident,
                    KeyFormat rsKeyFormat,
                    const IndexDescriptor* desc,
                    bool isLogged);

    virtual Status insert(
        OperationContext* opCtx,
        const KeyString::Value& keyString,
        bool dupsAllowed,
        IncludeDuplicateRecordId includeDuplicateRecordId = IncludeDuplicateRecordId::kOff);

    virtual void unindex(OperationContext* opCtx,
                         const KeyString::Value& keyString,
                         bool dupsAllowed);

    virtual boost::optional<RecordId> findLoc(OperationContext* opCtx,
                                              const KeyString::Value& keyString) const override;

    virtual IndexValidateResults validate(OperationContext* opCtx, bool full) const;

    virtual bool appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* output,
                                   double scale) const;
    virtual Status dupKeyCheck(OperationContext* opCtx, const KeyString::Value& keyString);

    virtual bool isEmpty(OperationContext* opCtx);

    virtual int64_t numEntries(OperationContext* opCtx) const;

    virtual long long getSpaceUsedBytes(OperationContext* opCtx) const;

    virtual long long getFreeStorageBytes(OperationContext* opCtx) const;

    virtual Status initAsEmpty(OperationContext* opCtx);

    virtual void printIndexEntryMetadata(OperationContext* opCtx,
                                         const KeyString::Value& keyString) const;

    Status compact(OperationContext* opCtx) override;

    const std::string& uri() const {
        return _uri;
    }

    // WiredTigerIndex additions

    uint64_t tableId() const {
        return _tableId;
    }

    std::string indexName() const {
        return _indexName;
    }

    NamespaceString getCollectionNamespace(OperationContext* opCtx) const;

    const BSONObj& keyPattern() const {
        return _keyPattern;
    }

    virtual bool isIdIndex() const {
        return false;
    }

    virtual UUID getCollectionUUID() const {
        return _collectionUUID;
    }

    virtual bool isDup(OperationContext* opCtx,
                       WT_CURSOR* c,
                       const KeyString::Value& keyString) = 0;
    virtual bool unique() const = 0;
    virtual bool isTimestampSafeUniqueIdx() const = 0;
    void insertWithRecordIdInValue_forTest(OperationContext* opCtx,
                                           const KeyString::Value& keyString,
                                           RecordId rid) override {
        MONGO_UNREACHABLE;
    }

protected:
    virtual Status _insert(
        OperationContext* opCtx,
        WT_CURSOR* c,
        const KeyString::Value& keyString,
        bool dupsAllowed,
        IncludeDuplicateRecordId includeDuplicateRecordId = IncludeDuplicateRecordId::kOff) = 0;

    virtual void _unindex(OperationContext* opCtx,
                          WT_CURSOR* c,
                          const KeyString::Value& keyString,
                          bool dupsAllowed) = 0;

    void setKey(WT_CURSOR* cursor, const WT_ITEM* item);
    void getKey(OperationContext* opCtx, WT_CURSOR* cursor, WT_ITEM* key);

    /**
     * Checks whether the prefix key defined by 'buffer' and 'size' is in the index. If it is,
     * returns the RecordId of the first matching key and positions the cursor 'c' on that key.
     */
    boost::optional<RecordId> _keyExists(OperationContext* opCtx,
                                         WT_CURSOR* c,
                                         const char* buffer,
                                         size_t size);

    /**
     * Checks whether the prefix key defined by 'keyString' and 'sizeWithoutRecordId' is in the
     * index. If it is, returns the RecordId of the first matching key and positions the cursor 'c'
     * on that key.
     */
    boost::optional<RecordId> _keyExistsBounded(OperationContext* opCtx,
                                                WT_CURSOR* c,
                                                const KeyString::Value& keyString,
                                                size_t sizeWithoutRecordId);

    /**
     * Sets the upper bound on the passed in cursor to be the maximum value of the KeyString prefix.
     */
    void _setUpperBound(WT_CURSOR* c,
                        const KeyString::Value& keyString,
                        size_t sizeWithoutRecordId);

    /**
     * Returns a DuplicateKey error if the prefix key exists in the index with a different RecordId.
     * Returns true if the prefix key exists in the index with the same RecordId. Returns false if
     * the prefix key does not exist in the index. Should only be used for non-_id indexes.
     */
    StatusWith<bool> _checkDups(
        OperationContext* opCtx,
        WT_CURSOR* c,
        const KeyString::Value& keyString,
        IncludeDuplicateRecordId includeDuplicateRecordId = IncludeDuplicateRecordId::kOff);

    /*
     * Determines the data format version from application metadata and verifies compatibility.
     * Returns the corresponding KeyString version.
     */
    KeyString::Version _handleVersionInfo(OperationContext* ctx,
                                          const std::string& uri,
                                          StringData ident,
                                          const IndexDescriptor* desc,
                                          bool isLogged);

    /*
     * Attempts to repair the data format version in the index table metadata if there is a mismatch
     * to the index type during startup.
     */
    void _repairDataFormatVersion(OperationContext* opCtx,
                                  const std::string& uri,
                                  StringData ident,
                                  const IndexDescriptor* desc);

    RecordId _decodeRecordIdAtEnd(const void* buffer, size_t size);

    class BulkBuilder;
    class IdBulkBuilder;
    class StandardBulkBuilder;
    class UniqueBulkBuilder;

    /*
     * The data format version is effectively const after the WiredTigerIndex instance is
     * constructed.
     */
    int _dataFormatVersion;
    std::string _uri;
    uint64_t _tableId;
    const UUID _collectionUUID;
    const std::string _indexName;
    const BSONObj _keyPattern;
    const BSONObj _collation;
    const bool _isLogged;
};

class WiredTigerIndexUnique : public WiredTigerIndex {
public:
    WiredTigerIndexUnique(OperationContext* ctx,
                          const std::string& uri,
                          const UUID& collectionUUID,
                          StringData ident,
                          KeyFormat rsKeyFormat,
                          const IndexDescriptor* desc,
                          bool isLogged);

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           bool forward) const override;

    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) override;

    bool unique() const override {
        return true;
    }

    bool isTimestampSafeUniqueIdx() const override;

    bool isDup(OperationContext* opCtx, WT_CURSOR* c, const KeyString::Value& keyString) override;

    void insertWithRecordIdInValue_forTest(OperationContext* opCtx,
                                           const KeyString::Value& keyString,
                                           RecordId rid) override;

protected:
    Status _insert(OperationContext* opCtx,
                   WT_CURSOR* c,
                   const KeyString::Value& keyString,
                   bool dupsAllowed,
                   IncludeDuplicateRecordId includeDuplicateRecordId =
                       IncludeDuplicateRecordId::kOff) override;

    void _unindex(OperationContext* opCtx,
                  WT_CURSOR* c,
                  const KeyString::Value& keyString,
                  bool dupsAllowed) override;

    /**
     * This function continues to exist in order to support v4.0 unique partial index format: the
     * format changed in v4.2 and onward. _unindex will call this if an index entry in the new
     * format cannot be found, and this function will check for the old format.
     */
    void _unindexTimestampUnsafe(OperationContext* opCtx,
                                 WT_CURSOR* c,
                                 const KeyString::Value& keyString,
                                 bool dupsAllowed);

private:
    bool _partial;
};

class WiredTigerIdIndex : public WiredTigerIndex {
public:
    WiredTigerIdIndex(OperationContext* ctx,
                      const std::string& uri,
                      const UUID& collectionUUID,
                      StringData ident,
                      const IndexDescriptor* desc,
                      bool isLogged);

    std::unique_ptr<Cursor> newCursor(OperationContext* opCtx,
                                      bool isForward = true) const override;

    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) override;

    bool unique() const override {
        return true;
    }

    bool isIdIndex() const override {
        return true;
    }

    bool isTimestampSafeUniqueIdx() const override {
        return false;
    }

    bool isDup(OperationContext* opCtx, WT_CURSOR* c, const KeyString::Value& keyString) override {
        // Unimplemented by _id indexes for lack of need
        MONGO_UNREACHABLE;
    }

protected:
    Status _insert(OperationContext* opCtx,
                   WT_CURSOR* c,
                   const KeyString::Value& keyString,
                   bool dupsAllowed,
                   IncludeDuplicateRecordId includeDuplicateRecordId =
                       IncludeDuplicateRecordId::kOff) override;

    void _unindex(OperationContext* opCtx,
                  WT_CURSOR* c,
                  const KeyString::Value& keyString,
                  bool dupsAllowed) override;

    /**
     * This is not applicable to id indexes. See base class comments.
     */
    Status _checkDups(OperationContext* opCtx,
                      WT_CURSOR* c,
                      const KeyString::Value& keyString,
                      IncludeDuplicateRecordId includeDuplicateRecordId) = delete;
};

class WiredTigerIndexStandard : public WiredTigerIndex {
public:
    WiredTigerIndexStandard(OperationContext* ctx,
                            const std::string& uri,
                            const UUID& collectionUUID,
                            StringData ident,
                            KeyFormat rsKeyFormat,
                            const IndexDescriptor* desc,
                            bool isLogged);

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           bool forward) const override;

    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) override;

    bool unique() const override {
        return false;
    }

    bool isTimestampSafeUniqueIdx() const override {
        return false;
    }

    bool isDup(OperationContext* opCtx, WT_CURSOR* c, const KeyString::Value& keyString) override {
        // Unimplemented by non-unique indexes
        MONGO_UNREACHABLE;
    }

protected:
    Status _insert(OperationContext* opCtx,
                   WT_CURSOR* c,
                   const KeyString::Value& keyString,
                   bool dupsAllowed,
                   IncludeDuplicateRecordId includeDuplicateRecordId =
                       IncludeDuplicateRecordId::kOff) override;

    void _unindex(OperationContext* opCtx,
                  WT_CURSOR* c,
                  const KeyString::Value& keyString,
                  bool dupsAllowed) override;
};

}  // namespace mongo
