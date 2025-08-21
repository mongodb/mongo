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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_container.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <wiredtiger.h>

#include <boost/optional/optional.hpp>

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

namespace CollectionValidation {
class ValidationOptions;
}

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
    static std::string generateAppMetadataString(const IndexConfig& config);

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
                                                        StringData tableName,
                                                        const IndexConfig& config,
                                                        bool isLogged);

    /**
     * Creates a WiredTiger table suitable for implementing a MongoDB index.
     * 'config' should be created with generateCreateString().
     */
    static Status create(WiredTigerRecoveryUnit&,
                         const std::string& uri,
                         const std::string& config);

    /**
     * Drops the specified WiredTiger table. This should only be used for resuming index builds.
     */
    static Status Drop(WiredTigerRecoveryUnit&, const std::string& uri);

    /**
     * Constructs an index. The rsKeyFormat is the RecordId key format of the related RecordStore.
     */
    WiredTigerIndex(OperationContext* ctx,
                    RecoveryUnit& ru,
                    const std::string& uri,
                    const UUID& collectionUUID,
                    StringData ident,
                    KeyFormat rsKeyFormat,
                    const IndexConfig& config,
                    bool isLogged);

    std::variant<Status, DuplicateKey> insert(OperationContext* opCtx,
                                              RecoveryUnit& ru,
                                              const key_string::View& keyString,
                                              bool dupsAllowed,
                                              IncludeDuplicateRecordId includeDuplicateRecordId =
                                                  IncludeDuplicateRecordId::kOff) override;

    void unindex(OperationContext* opCtx,
                 RecoveryUnit& ru,
                 const key_string::View& keyString,
                 bool dupsAllowed) override;

    boost::optional<RecordId> findLoc(OperationContext* opCtx,
                                      RecoveryUnit& ru,
                                      std::span<const char> keyString) const override;

    IndexValidateResults validate(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        const CollectionValidation::ValidationOptions& options) const override;

    bool appendCustomStats(OperationContext* opCtx,
                           RecoveryUnit& ru,
                           BSONObjBuilder* output,
                           double scale) const override;
    boost::optional<DuplicateKey> dupKeyCheck(OperationContext* opCtx,
                                              RecoveryUnit& ru,
                                              const key_string::View& keyString) override;

    bool isEmpty(OperationContext* opCtx, RecoveryUnit& ru) override;

    int64_t numEntries(OperationContext* opCtx, RecoveryUnit& ru) const override;

    long long getSpaceUsedBytes(OperationContext* opCtx, RecoveryUnit& ru) const override;

    long long getFreeStorageBytes(OperationContext* opCtx, RecoveryUnit& ru) const override;

    Status initAsEmpty() override;

    void printIndexEntryMetadata(OperationContext* opCtx,
                                 RecoveryUnit& ru,
                                 const key_string::View& keyString) const override;

    StatusWith<int64_t> compact(OperationContext* opCtx,
                                RecoveryUnit& ru,
                                const CompactOptions& options) override;

    Status truncate(OperationContext* opCtx, RecoveryUnit& ru) override;

    StringKeyedContainer& getContainer() override;

    const StringKeyedContainer& getContainer() const override;

    WiredTigerStringKeyedContainer& getWiredTigerContainer();

    StringData uri() const {
        return _container.uri();
    }

    // WiredTigerIndex additions

    uint64_t tableId() const {
        return _container.tableId();
    }

    std::string indexName() const {
        return _indexName;
    }

    virtual bool isIdIndex() const {
        return false;
    }

    UUID getCollectionUUID() const {
        return _collectionUUID;
    }

    virtual bool isDup(OperationContext* opCtx,
                       RecoveryUnit& ru,
                       WT_CURSOR* c,
                       WiredTigerSession* session,
                       const key_string::View& keyString) = 0;
    virtual bool unique() const = 0;
    virtual bool isTimestampSafeUniqueIdx() const = 0;

    bool hasOldFormatVersion() const {
        return _dataFormatVersion == kDataFormatV3KeyStringV0UniqueIndexVersionV1 ||
            _dataFormatVersion == kDataFormatV4KeyStringV1UniqueIndexVersionV2;
    }

protected:
    virtual std::variant<Status, SortedDataInterface::DuplicateKey> _insert(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        WT_CURSOR* c,
        WiredTigerSession* session,
        const key_string::View& keyString,
        bool dupsAllowed,
        IncludeDuplicateRecordId includeDuplicateRecordId = IncludeDuplicateRecordId::kOff) = 0;

    virtual void _unindex(OperationContext* opCtx,
                          RecoveryUnit& ru,
                          WT_CURSOR* c,
                          const key_string::View& keyString,
                          bool dupsAllowed) = 0;

    /**
     * Loads the key positioned by this cursor into 'key'.
     */
    void getKey(WT_CURSOR* cursor, WT_ITEM* key);

    /**
     * Checks whether the prefix key defined by 'keyString' and 'sizeWithoutRecordId' is in the
     * index. If it is, returns the RecordId of the first matching key and positions the cursor 'c'
     * on that key.
     */
    boost::optional<RecordId> _keyExists(OperationContext* opCtx,
                                         RecoveryUnit& ru,
                                         WT_CURSOR* c,
                                         WiredTigerSession* session,
                                         const key_string::View& keyString);

    /**
     * Sets the upper bound on the passed in cursor to be the maximum value of the KeyString prefix.
     * Used when checking if a specific key prefix exists.
     */
    void _setUpperBoundForKeyExists(WT_CURSOR* c,
                                    WiredTigerSession* session,
                                    const key_string::View& keyString);

    /**
     * Returns a DuplicateKey error if the prefix key exists in the index with a different RecordId.
     * Returns true if the prefix key exists in the index with the same RecordId. Returns false if
     * the prefix key does not exist in the index. Should only be used for non-_id indexes.
     */
    std::variant<bool, DuplicateKey> _checkDups(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        WT_CURSOR* c,
        WiredTigerSession* session,
        const key_string::View& keyString,
        IncludeDuplicateRecordId includeDuplicateRecordId = IncludeDuplicateRecordId::kOff);

    /*
     * Determines the data format version from application metadata and verifies compatibility.
     * Returns the corresponding KeyString version.
     */
    key_string::Version _handleVersionInfo(OperationContext* ctx,
                                           RecoveryUnit& ru,
                                           const std::string& uri,
                                           StringData ident,
                                           const IndexConfig& config,
                                           bool isLogged);

    /*
     * Attempts to repair the data format version in the index table metadata if there is a mismatch
     * to the index type during startup.
     */
    void _repairDataFormatVersion(OperationContext* opCtx,
                                  RecoveryUnit& ru,
                                  const std::string& uri,
                                  StringData ident,
                                  const IndexConfig& config);

    WiredTigerStringKeyedContainer _container;

    /*
     * The data format version is effectively const after the WiredTigerIndex instance is
     * constructed.
     */
    int _dataFormatVersion;
    const UUID _collectionUUID;
    const std::string _indexName;
    const bool _isLogged;
};

class WiredTigerIndexUnique : public WiredTigerIndex {
public:
    WiredTigerIndexUnique(OperationContext* ctx,
                          RecoveryUnit& ru,
                          const std::string& uri,
                          const UUID& collectionUUID,
                          StringData ident,
                          KeyFormat rsKeyFormat,
                          const IndexConfig& config,
                          bool isLogged);

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           RecoveryUnit& ru,
                                                           bool forward) const override;

    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                RecoveryUnit& ru) override;

    bool unique() const override {
        return true;
    }

    bool isTimestampSafeUniqueIdx() const override;

    bool isDup(OperationContext* opCtx,
               RecoveryUnit& ru,
               WT_CURSOR* c,
               WiredTigerSession* session,
               const key_string::View& keyString) override;


protected:
    std::variant<Status, SortedDataInterface::DuplicateKey> _insert(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        WT_CURSOR* c,
        WiredTigerSession* session,
        const key_string::View& keyString,
        bool dupsAllowed,
        IncludeDuplicateRecordId includeDuplicateRecordId =
            IncludeDuplicateRecordId::kOff) override;

    void _unindex(OperationContext* opCtx,
                  RecoveryUnit& ru,
                  WT_CURSOR* c,
                  const key_string::View& keyString,
                  bool dupsAllowed) override;

    /**
     * This function continues to exist in order to support v4.0 unique index format: the format
     * changed in v4.2 and onward. _unindex will call this if an index entry in the new format
     * cannot be found, and this function will check for the old format.
     */
    void _unindexTimestampUnsafe(OperationContext* opCtx,
                                 RecoveryUnit& ru,
                                 WT_CURSOR* c,
                                 const key_string::View& keyString,
                                 bool dupsAllowed);
};

class WiredTigerIdIndex : public WiredTigerIndex {
public:
    WiredTigerIdIndex(OperationContext* ctx,
                      RecoveryUnit& ru,
                      const std::string& uri,
                      const UUID& collectionUUID,
                      StringData ident,
                      const IndexConfig& config,
                      bool isLogged);

    std::unique_ptr<Cursor> newCursor(OperationContext* opCtx,
                                      RecoveryUnit& ru,
                                      bool isForward = true) const override;

    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                RecoveryUnit& ru) override;

    bool unique() const override {
        return true;
    }

    bool isIdIndex() const override {
        return true;
    }

    bool isTimestampSafeUniqueIdx() const override {
        return false;
    }

    bool isDup(OperationContext* opCtx,
               RecoveryUnit& ru,
               WT_CURSOR* c,
               WiredTigerSession* session,
               const key_string::View& keyString) override {
        // Unimplemented by _id indexes for lack of need
        MONGO_UNREACHABLE;
    }

protected:
    std::variant<Status, SortedDataInterface::DuplicateKey> _insert(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        WT_CURSOR* c,
        WiredTigerSession* session,
        const key_string::View& keyString,
        bool dupsAllowed,
        IncludeDuplicateRecordId includeDuplicateRecordId =
            IncludeDuplicateRecordId::kOff) override;

    void _unindex(OperationContext* opCtx,
                  RecoveryUnit& ru,
                  WT_CURSOR* c,
                  const key_string::View& keyString,
                  bool dupsAllowed) override;

    /**
     * This is not applicable to id indexes. See base class comments.
     */
    Status _checkDups(OperationContext* opCtx,
                      RecoveryUnit& ru,
                      WT_CURSOR* c,
                      const key_string::View& keyString,
                      IncludeDuplicateRecordId includeDuplicateRecordId) = delete;
};

class WiredTigerIndexStandard : public WiredTigerIndex {
public:
    WiredTigerIndexStandard(OperationContext* ctx,
                            RecoveryUnit& ru,
                            const std::string& uri,
                            const UUID& collectionUUID,
                            StringData ident,
                            KeyFormat rsKeyFormat,
                            const IndexConfig& config,
                            bool isLogged);

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           RecoveryUnit& ru,
                                                           bool forward) const override;

    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                RecoveryUnit& ru) override;

    bool unique() const override {
        return false;
    }

    bool isTimestampSafeUniqueIdx() const override {
        return false;
    }

    bool isDup(OperationContext* opCtx,
               RecoveryUnit& ru,
               WT_CURSOR* c,
               WiredTigerSession* session,
               const key_string::View& keyString) override {
        // Unimplemented by non-unique indexes
        MONGO_UNREACHABLE;
    }

protected:
    std::variant<Status, SortedDataInterface::DuplicateKey> _insert(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        WT_CURSOR* c,
        WiredTigerSession* session,
        const key_string::View& keyString,
        bool dupsAllowed,
        IncludeDuplicateRecordId includeDuplicateRecordId =
            IncludeDuplicateRecordId::kOff) override;

    void _unindex(OperationContext* opCtx,
                  RecoveryUnit& ru,
                  WT_CURSOR* c,
                  const key_string::View& keyString,
                  bool dupsAllowed) override;
};

}  // namespace mongo
