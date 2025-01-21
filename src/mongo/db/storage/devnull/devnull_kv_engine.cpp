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
#include "mongo/db/catalog/validate/validate_results.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/devnull/ephemeral_catalog_record_store.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_base.h"
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

class DevNullRecordStore : public RecordStoreBase {
public:
    class Capped;
    class Oplog;

    DevNullRecordStore(boost::optional<UUID> uuid,
                       StringData identName,
                       const CollectionOptions& options,
                       KeyFormat keyFormat)
        : RecordStoreBase(uuid, identName), _options(options), _keyFormat(keyFormat) {
        _numInserts = 0;
        _dummy = BSON("_id" << 1);
    }

    const char* name() const override {
        return "devnull";
    }

    long long dataSize() const override {
        return 0;
    }

    long long numRecords() const override {
        return 0;
    }

    virtual bool isCapped() const {
        return _options.capped;
    }

    KeyFormat keyFormat() const override {
        return _keyFormat;
    }

    int64_t storageSize(RecoveryUnit& ru,
                        BSONObjBuilder* extraInfo = nullptr,
                        int infoLevel = 0) const override {
        return 0;
    }

    int64_t freeStorageSize(RecoveryUnit&) const override {
        return 0;
    }

    bool updateWithDamagesSupported() const override {
        return false;
    }

    void printRecordMetadata(const RecordId& recordId,
                             std::set<Timestamp>* recordTimestamps) const override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward) const final {
        return std::make_unique<EmptyRecordCursor>();
    }

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext*) const override {
        return {};
    }

    bool compactSupported() const override {
        return false;
    }

    void validate(RecoveryUnit&,
                  const CollectionValidation::ValidationOptions&,
                  ValidateResults*) override {}

    void appendNumericCustomStats(RecoveryUnit& ru,
                                  BSONObjBuilder* result,
                                  double scale) const override {
        result->appendNumber("numInserts", _numInserts);
    }

    void appendAllCustomStats(RecoveryUnit& ru,
                              BSONObjBuilder* result,
                              double scale) const override {
        appendNumericCustomStats(ru, result, scale);
    }

    void updateStatsAfterRepair(long long numRecords, long long dataSize) override {}

    RecordStore::Capped* capped() override {
        return nullptr;
    }

    RecordStore::Oplog* oplog() override {
        return nullptr;
    }

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
    void _deleteRecord(OperationContext* opCtx, const RecordId& dl) override {}

    Status _insertRecords(OperationContext* opCtx,
                          std::vector<Record>* inOutRecords,
                          const std::vector<Timestamp>& timestamps) override {
        _numInserts += inOutRecords->size();
        for (auto& record : *inOutRecords) {
            record.id = RecordId(6, 4);
        }
        return Status::OK();
    }

    Status _updateRecord(OperationContext* opCtx,
                         const RecordId& oldLocation,
                         const char* data,
                         int len) override {
        return Status::OK();
    }

    StatusWith<RecordData> _updateWithDamages(OperationContext* opCtx,
                                              const RecordId& loc,
                                              const RecordData& oldRec,
                                              const char* damageSource,
                                              const DamageVector& damages) override {
        MONGO_UNREACHABLE;
    }

    Status _truncate(OperationContext* opCtx) override {
        return Status::OK();
    }

    Status _rangeTruncate(OperationContext* opCtx,
                          const RecordId& minRecordId,
                          const RecordId& maxRecordId,
                          int64_t hintDataSizeDiff,
                          int64_t hintNumRecordsDiff) override {
        return Status::OK();
    }

    StatusWith<int64_t> _compact(OperationContext*, const CompactOptions&) override {
        return Status::OK();
    }

    CollectionOptions _options;
    KeyFormat _keyFormat;
    long long _numInserts;
    BSONObj _dummy;
};

class DevNullRecordStore::Capped : public DevNullRecordStore, public RecordStoreBase::Capped {
public:
    Capped(boost::optional<UUID> uuid,
           StringData identName,
           const CollectionOptions& options,
           KeyFormat keyFormat)
        : DevNullRecordStore(uuid, identName, options, keyFormat) {}

    RecordStore::Capped* capped() override {
        return this;
    }

private:
    void _truncateAfter(OperationContext*,
                        const RecordId&,
                        bool inclusive,
                        const AboutToDeleteRecordCallback&) override {}
};

class DevNullRecordStore::Oplog final : public DevNullRecordStore::Capped,
                                        public RecordStore::Oplog {
public:
    Oplog(UUID uuid, StringData identName, const CollectionOptions& options)
        : DevNullRecordStore::Capped(uuid, identName, options, KeyFormat::Long) {}

    RecordStore::Capped* capped() override {
        return this;
    }

    RecordStore::Oplog* oplog() override {
        return this;
    }

    bool selfManagedTruncation() const override {
        return false;
    }

    Status updateSize(long long size) override {
        return Status::OK();
    }

    std::unique_ptr<SeekableRecordCursor> getRawCursor(OperationContext* opCtx,
                                                       bool forward) const override {
        return std::make_unique<EmptyRecordCursor>();
    }

    StatusWith<Timestamp> getLatestTimestamp(RecoveryUnit&) const override {
        return Status::OK();
    }

    StatusWith<Timestamp> getEarliestTimestamp(RecoveryUnit&) override {
        return Status::OK();
    }

    std::shared_ptr<CollectionTruncateMarkers> getCollectionTruncateMarkers() override {
        return nullptr;
    }
};

class DevNullSortedDataBuilderInterface : public SortedDataBuilderInterface {
    DevNullSortedDataBuilderInterface(const DevNullSortedDataBuilderInterface&) = delete;
    DevNullSortedDataBuilderInterface& operator=(const DevNullSortedDataBuilderInterface&) = delete;

public:
    DevNullSortedDataBuilderInterface() {}

    void addKey(const key_string::View& keyString) override {}
};

class DevNullSortedDataInterface : public SortedDataInterface {
public:
    DevNullSortedDataInterface(StringData identName)
        : SortedDataInterface(identName,
                              key_string::Version::kLatestVersion,
                              Ordering::make(BSONObj()),
                              KeyFormat::Long) {}

    ~DevNullSortedDataInterface() override {}

    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx) override {
        return {};
    }

    std::variant<Status, DuplicateKey> insert(
        OperationContext* opCtx,
        const key_string::Value& keyString,
        bool dupsAllowed,
        IncludeDuplicateRecordId includeDuplicateRecordId) override {
        return Status::OK();
    }

    void unindex(OperationContext* opCtx,
                 const key_string::View& keyString,
                 bool dupsAllowed) override {}

    boost::optional<DuplicateKey> dupKeyCheck(OperationContext* opCtx,
                                              const SortedDataKeyValueView& keyString) override {
        return boost::none;
    }

    boost::optional<RecordId> findLoc(OperationContext* opCtx,
                                      std::span<const char> keyString) const override {
        return boost::none;
    }

    IndexValidateResults validate(
        OperationContext* opCtx,
        const CollectionValidation::ValidationOptions& options) const override {
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

    Status initAsEmpty() override {
        return Status::OK();
    }
};

DevNullKVEngine::DevNullKVEngine() : _engineDbPath(storageGlobalParams.dbpath) {
    auto testFilePath = _engineDbPath / "testFile.txt";
    _mockBackupBlocks.push_back(BackupBlock(/*nss=*/boost::none,
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
        return std::make_unique<EphemeralForTestRecordStore>(options.uuid, ident, &_catalogInfo);
    } else if (nss == NamespaceString::kRsOplogNamespace) {
        return std::make_unique<DevNullRecordStore::Oplog>(*options.uuid, ident, options);
    } else if (options.capped) {
        return std::make_unique<DevNullRecordStore::Capped>(
            options.uuid, ident, options, KeyFormat::Long);
    }
    return std::make_unique<DevNullRecordStore>(options.uuid, ident, options, KeyFormat::Long);
}

std::unique_ptr<RecordStore> DevNullKVEngine::getTemporaryRecordStore(OperationContext* opCtx,
                                                                      StringData ident,
                                                                      KeyFormat keyFormat) {
    return makeTemporaryRecordStore(opCtx, ident, keyFormat);
}

std::unique_ptr<RecordStore> DevNullKVEngine::makeTemporaryRecordStore(OperationContext* opCtx,
                                                                       StringData ident,
                                                                       KeyFormat keyFormat) {
    return std::make_unique<DevNullRecordStore>(
        boost::none /* uuid */, ident, CollectionOptions(), keyFormat);
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

    StatusWith<std::deque<BackupBlock>> getNextBatch(const std::size_t batchSize) override {
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
    const StorageEngine::BackupOptions& options) {
    return std::make_unique<StreamingCursorImpl>(options, _mockBackupBlocks);
}

StatusWith<std::deque<std::string>> DevNullKVEngine::extendBackupCursor() {
    std::deque<std::string> filesToCopy = {
        (_engineDbPath / "journal" / "WiredTigerLog.999").string()};
    return filesToCopy;
}

}  // namespace mongo
