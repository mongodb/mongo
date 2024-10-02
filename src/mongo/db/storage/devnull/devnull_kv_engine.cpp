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

#include <boost/move/utility_core.hpp>
#include <cstddef>
#include <memory>
#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/devnull/ephemeral_catalog_record_store.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/uuid.h"

namespace mongo {

class EmptyRecordCursor final : public SeekableRecordCursor {
public:
    boost::optional<Record> next() final {
        return {};
    }
    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override {
        return {};
    }
    boost::optional<Record> seekExact(const RecordId& id) final {
        return {};
    }
    void save() final {}
    bool restore(bool tolerateCappedRepositioning = true) final {
        return true;
    }
    void detachFromOperationContext() final {}
    void reattachToOperationContext(OperationContext* opCtx) final {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}
};

class DevNullRecordStore : public RecordStore {
public:
    DevNullRecordStore(const NamespaceString& nss,
                       boost::optional<UUID> uuid,
                       StringData identName,
                       const CollectionOptions& options,
                       KeyFormat keyFormat)
        : RecordStore(uuid, identName, options.capped),
          _options(options),
          _keyFormat(keyFormat),
          _ns(nss) {
        _numInserts = 0;
        _dummy = BSON("_id" << 1);
    }

    const char* name() const override {
        return "devnull";
    }

    NamespaceString ns(OperationContext* opCtx) const override {
        return _ns;
    }

    long long dataSize(OperationContext* opCtx) const override {
        return 0;
    }

    long long numRecords(OperationContext* opCtx) const override {
        return 0;
    }

    virtual bool isCapped() const {
        return _options.capped;
    }

    KeyFormat keyFormat() const override {
        return _keyFormat;
    }

    int64_t storageSize(OperationContext* opCtx,
                        BSONObjBuilder* extraInfo = nullptr,
                        int infoLevel = 0) const override {
        return 0;
    }

    bool findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* rd) const override {
        return false;
    }

    void doDeleteRecord(OperationContext* opCtx, const RecordId& dl) override {}

    Status doInsertRecords(OperationContext* opCtx,
                           std::vector<Record>* inOutRecords,
                           const std::vector<Timestamp>& timestamps) override {
        _numInserts += inOutRecords->size();
        for (auto& record : *inOutRecords) {
            record.id = RecordId(6, 4);
        }
        return Status::OK();
    }

    Status doUpdateRecord(OperationContext* opCtx,
                          const RecordId& oldLocation,
                          const char* data,
                          int len) override {
        return Status::OK();
    }

    bool updateWithDamagesSupported() const override {
        return false;
    }

    StatusWith<RecordData> doUpdateWithDamages(OperationContext* opCtx,
                                               const RecordId& loc,
                                               const RecordData& oldRec,
                                               const char* damageSource,
                                               const DamageVector& damages) override {
        MONGO_UNREACHABLE;
    }

    void printRecordMetadata(OperationContext* opCtx,
                             const RecordId& recordId,
                             std::set<Timestamp>* recordTimestamps) const override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final {
        return std::make_unique<EmptyRecordCursor>();
    }

    Status doTruncate(OperationContext* opCtx) override {
        return Status::OK();
    }

    Status doRangeTruncate(OperationContext* opCtx,
                           const RecordId& minRecordId,
                           const RecordId& maxRecordId,
                           int64_t hintDataSizeDiff,
                           int64_t hintNumRecordsDiff) override {
        return Status::OK();
    }

    void doCappedTruncateAfter(OperationContext* opCtx,
                               const RecordId& end,
                               bool inclusive,
                               const AboutToDeleteRecordCallback& aboutToDelete) override {}

    void appendNumericCustomStats(OperationContext* opCtx,
                                  BSONObjBuilder* result,
                                  double scale) const override {
        result->appendNumber("numInserts", _numInserts);
    }

    void updateStatsAfterRepair(OperationContext* opCtx,
                                long long numRecords,
                                long long dataSize) override {}

    RecordId getLargestKey(OperationContext* opCtx) const final {
        return RecordId();
    }

    void reserveRecordIds(OperationContext* opCtx,
                          std::vector<RecordId>* out,
                          size_t nRecords) final {
        for (size_t i = 0; i < nRecords; i++) {
            out->push_back(RecordId(i));
        }
    };

private:
    CollectionOptions _options;
    KeyFormat _keyFormat;
    long long _numInserts;
    BSONObj _dummy;
    NamespaceString _ns;
};

class DevNullSortedDataBuilderInterface : public SortedDataBuilderInterface {
    DevNullSortedDataBuilderInterface(const DevNullSortedDataBuilderInterface&) = delete;
    DevNullSortedDataBuilderInterface& operator=(const DevNullSortedDataBuilderInterface&) = delete;

public:
    DevNullSortedDataBuilderInterface() {}

    Status addKey(const key_string::Value& keyString) override {
        return Status::OK();
    }
};

class DevNullSortedDataInterface : public SortedDataInterface {
public:
    DevNullSortedDataInterface(StringData identName)
        : SortedDataInterface(identName,
                              key_string::Version::kLatestVersion,
                              Ordering::make(BSONObj()),
                              KeyFormat::Long) {}

    ~DevNullSortedDataInterface() override {}

    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) override {
        return {};
    }

    Status insert(OperationContext* opCtx,
                  const key_string::Value& keyString,
                  bool dupsAllowed,
                  IncludeDuplicateRecordId includeDuplicateRecordId) override {
        return Status::OK();
    }

    void unindex(OperationContext* opCtx,
                 const key_string::Value& keyString,
                 bool dupsAllowed) override {}

    Status dupKeyCheck(OperationContext* opCtx, const key_string::Value& keyString) override {
        return Status::OK();
    }

    boost::optional<RecordId> findLoc(OperationContext* opCtx,
                                      StringData keyString) const override {
        return boost::none;
    }

    IndexValidateResults validate(OperationContext* opCtx, bool full) const override {
        return IndexValidateResults{};
    }

    bool appendCustomStats(OperationContext* opCtx,
                           BSONObjBuilder* output,
                           double scale) const override {
        return false;
    }

    long long getSpaceUsedBytes(OperationContext* opCtx) const override {
        return 0;
    }

    long long getFreeStorageBytes(OperationContext* opCtx) const override {
        return 0;
    }

    bool isEmpty(OperationContext* opCtx) override {
        return true;
    }

    int64_t numEntries(OperationContext* opCtx) const override {
        return 0;
    }

    void printIndexEntryMetadata(OperationContext* opCtx,
                                 const key_string::Value& keyString) const override {}

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           bool isForward) const override {
        return {};
    }

    Status initAsEmpty(OperationContext* opCtx) override {
        return Status::OK();
    }
};

DevNullKVEngine::DevNullKVEngine() : _engineDbPath(storageGlobalParams.dbpath) {
    auto testFilePath = _engineDbPath / "testFile.txt";
    _mockBackupBlocks.push_back(BackupBlock(/*opCtx=*/nullptr,
                                            /*nss=*/boost::none,
                                            /*uuid=*/boost::none,
                                            /*filePath=*/testFilePath.string()));
}

DevNullKVEngine::~DevNullKVEngine() = default;

RecoveryUnit* DevNullKVEngine::newRecoveryUnit() {
    return new RecoveryUnitNoop();
}

std::unique_ptr<RecordStore> DevNullKVEngine::getRecordStore(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             StringData ident,
                                                             const CollectionOptions& options) {
    if (ident == "_mdb_catalog") {
        return std::make_unique<EphemeralForTestRecordStore>(
            nss, options.uuid, ident, &_catalogInfo);
    }
    return std::make_unique<DevNullRecordStore>(nss, options.uuid, ident, options, KeyFormat::Long);
}

std::unique_ptr<RecordStore> DevNullKVEngine::getTemporaryRecordStore(OperationContext* opCtx,
                                                                      StringData ident,
                                                                      KeyFormat keyFormat) {
    return makeTemporaryRecordStore(opCtx, ident, keyFormat);
}

std::unique_ptr<RecordStore> DevNullKVEngine::makeTemporaryRecordStore(OperationContext* opCtx,
                                                                       StringData ident,
                                                                       KeyFormat keyFormat) {
    return std::make_unique<DevNullRecordStore>(NamespaceString::kEmpty /* ns */,
                                                boost::none /* uuid */,
                                                ident,
                                                CollectionOptions(),
                                                keyFormat);
}

std::unique_ptr<SortedDataInterface> DevNullKVEngine::getSortedDataInterface(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& collOptions,
    StringData ident,
    const IndexDescriptor* desc) {
    return std::make_unique<DevNullSortedDataInterface>(ident);
}

namespace {

class StreamingCursorImpl : public StorageEngine::StreamingCursor {
public:
    StreamingCursorImpl() = delete;
    StreamingCursorImpl(StorageEngine::BackupOptions options, std::deque<BackupBlock> backupBlocks)
        : StorageEngine::StreamingCursor(options), _backupBlocks(std::move(backupBlocks)) {
        _exhaustCursor = false;
    };

    ~StreamingCursorImpl() override = default;

    BSONObj getMetadataObject(UUID backupId) {
        return BSONObj();
    }

    void setCatalogEntries(stdx::unordered_map<std::string, std::pair<NamespaceString, UUID>>
                               identsToNsAndUUID) override {}

    StatusWith<std::deque<BackupBlock>> getNextBatch(OperationContext* opCtx,
                                                     const std::size_t batchSize) override {
        if (_exhaustCursor) {
            std::deque<BackupBlock> emptyVector;
            return emptyVector;
        }
        _exhaustCursor = true;
        return _backupBlocks;
    }

private:
    std::deque<BackupBlock> _backupBlocks;
    bool _exhaustCursor;
};

}  // namespace

StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> DevNullKVEngine::beginNonBlockingBackup(
    OperationContext* opCtx, const StorageEngine::BackupOptions& options) {
    return std::make_unique<StreamingCursorImpl>(options, _mockBackupBlocks);
}

StatusWith<std::deque<std::string>> DevNullKVEngine::extendBackupCursor(OperationContext* opCtx) {
    std::deque<std::string> filesToCopy = {
        (_engineDbPath / "journal" / "WiredTigerLog.999").string()};
    return filesToCopy;
}

}  // namespace mongo
