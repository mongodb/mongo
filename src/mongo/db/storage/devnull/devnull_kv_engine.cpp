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

#include "mongo/db/storage/devnull/devnull_kv_engine.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/container_base.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/devnull/ephemeral_catalog_record_store.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_base.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <set>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

// TODO(SERVER-110243): Use TestIntegerKeyedContainer
class DevNullIntegerKeyedContainer : public IntegerKeyedContainerBase {
public:
    DevNullIntegerKeyedContainer() : IntegerKeyedContainerBase(nullptr) {}

    Status insert(RecoveryUnit& ru, int64_t key, std::span<const char> value) final {
        return Status::OK();
    }

    Status remove(RecoveryUnit& ru, int64_t key) final {
        return Status::OK();
    }
};

class DevNullStringKeyedContainer : public StringKeyedContainerBase {
public:
    DevNullStringKeyedContainer() : StringKeyedContainerBase(nullptr) {}

    Status insert(RecoveryUnit& ru, std::span<const char> key, std::span<const char> value) final {
        return Status::OK();
    }

    Status remove(RecoveryUnit& ru, std::span<const char> key) final {
        return Status::OK();
    }
};

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
    bool restore(RecoveryUnit& ru, bool tolerateCappedRepositioning = true) final {
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

    DevNullRecordStore(boost::optional<UUID> uuid, StringData ident, KeyFormat keyFormat)
        : RecordStoreBase(uuid, ident), _container(_makeContainer(keyFormat)) {
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
        // Record stores for capped collections should inherit from 'DevNullRecordStore::Capped',
        // which overrides this to true.
        return false;
    }

    KeyFormat keyFormat() const override {
        return std::visit(
            OverloadedVisitor(
                [](const DevNullIntegerKeyedContainer& v) -> KeyFormat { return KeyFormat::Long; },
                [](const DevNullStringKeyedContainer& v) -> KeyFormat {
                    return KeyFormat::String;
                }),
            _container);
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

    using RecordStoreBase::getCursor;
    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    RecoveryUnit& ru,
                                                    bool forward) const final {
        return std::make_unique<EmptyRecordCursor>();
    }

    using RecordStoreBase::getRandomCursor;
    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext*, RecoveryUnit&) const override {
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

    RecordId getLargestKey(OperationContext* opCtx, RecoveryUnit& ru) const final {
        return RecordId();
    }

    using RecordStoreBase::reserveRecordIds;
    void reserveRecordIds(OperationContext* opCtx,
                          RecoveryUnit& ru,
                          std::vector<RecordId>* out,
                          size_t nRecords) final {
        for (size_t i = 0; i < nRecords; i++) {
            out->push_back(RecordId(i));
        }
    };

    RecordStore::RecordStoreContainer getContainer() override {
        return std::visit([](auto& v) -> RecordStore::RecordStoreContainer { return v; },
                          _container);
    };

private:
    void _deleteRecord(OperationContext* opCtx, RecoveryUnit& ru, const RecordId& dl) override {}

    Status _insertRecords(OperationContext* opCtx,
                          RecoveryUnit& ru,
                          std::vector<Record>* inOutRecords,
                          const std::vector<Timestamp>& timestamps) override {
        _numInserts += inOutRecords->size();
        for (auto& record : *inOutRecords) {
            record.id = RecordId(6, 4);
        }
        return Status::OK();
    }

    Status _updateRecord(OperationContext* opCtx,
                         RecoveryUnit& ru,
                         const RecordId& oldLocation,
                         const char* data,
                         int len) override {
        return Status::OK();
    }

    StatusWith<RecordData> _updateWithDamages(OperationContext* opCtx,
                                              RecoveryUnit& ru,
                                              const RecordId& loc,
                                              const RecordData& oldRec,
                                              const char* damageSource,
                                              const DamageVector& damages) override {
        MONGO_UNREACHABLE;
    }

    Status _truncate(OperationContext* opCtx, RecoveryUnit& ru) override {
        return Status::OK();
    }

    Status _rangeTruncate(OperationContext* opCtx,
                          RecoveryUnit& ru,
                          const RecordId& minRecordId,
                          const RecordId& maxRecordId,
                          int64_t hintDataSizeDiff,
                          int64_t hintNumRecordsDiff) override {
        return Status::OK();
    }

    StatusWith<int64_t> _compact(OperationContext*, RecoveryUnit&, const CompactOptions&) override {
        return Status::OK();
    }

    std::variant<DevNullIntegerKeyedContainer, DevNullStringKeyedContainer> _makeContainer(
        KeyFormat& keyFormat) {
        switch (keyFormat) {
            case KeyFormat::Long: {
                auto container = DevNullIntegerKeyedContainer();
                return container;
            }
            case KeyFormat::String: {
                auto container = DevNullStringKeyedContainer();
                return container;
            }
        }
        MONGO_UNREACHABLE;
    }

    std::variant<DevNullIntegerKeyedContainer, DevNullStringKeyedContainer> _container;
    long long _numInserts;
    BSONObj _dummy;
};

class DevNullRecordStore::Capped : public DevNullRecordStore, public RecordStoreBase::Capped {
public:
    Capped(boost::optional<UUID> uuid, StringData ident, KeyFormat keyFormat)
        : DevNullRecordStore(uuid, ident, keyFormat) {}

    bool isCapped() const final {
        return true;
    }

    RecordStore::Capped* capped() override {
        return this;
    }

private:
    TruncateAfterResult _truncateAfter(OperationContext*,
                                       RecoveryUnit& ru,
                                       const RecordId&,
                                       bool inclusive) override {
        return {};
    }
};

class DevNullRecordStore::Oplog final : public DevNullRecordStore::Capped,
                                        public RecordStoreBase::Oplog {
public:
    Oplog(UUID uuid, StringData ident, int64_t maxSize)
        : DevNullRecordStore::Capped(uuid, ident, KeyFormat::Long), _maxSize(maxSize) {}

    RecordStore::Capped* capped() override {
        return this;
    }

    RecordStore::Oplog* oplog() override {
        return this;
    }

    Status updateSize(long long size) override {
        _maxSize = size;
        return Status::OK();
    }

    int64_t getMaxSize() const override {
        return _maxSize;
    }

    std::unique_ptr<SeekableRecordCursor> getRawCursor(OperationContext* opCtx,
                                                       RecoveryUnit& ru,
                                                       bool forward) const override {
        return std::make_unique<EmptyRecordCursor>();
    }

    StatusWith<Timestamp> getLatestTimestamp(RecoveryUnit&) const override {
        return Status::OK();
    }

    StatusWith<Timestamp> getEarliestTimestamp(RecoveryUnit&) override {
        return Status::OK();
    }

private:
    int64_t _maxSize;
};

class DevNullSortedDataBuilderInterface : public SortedDataBuilderInterface {
    DevNullSortedDataBuilderInterface(const DevNullSortedDataBuilderInterface&) = delete;
    DevNullSortedDataBuilderInterface& operator=(const DevNullSortedDataBuilderInterface&) = delete;

public:
    DevNullSortedDataBuilderInterface() {}

    void addKey(RecoveryUnit& ru, const key_string::View& keyString) override {}
};

class DevNullSortedDataInterface : public SortedDataInterface {
public:
    DevNullSortedDataInterface(StringData identName)
        : SortedDataInterface(
              key_string::Version::kLatestVersion, Ordering::make(BSONObj()), KeyFormat::Long) {}

    ~DevNullSortedDataInterface() override {}

    std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                RecoveryUnit& ru) override {
        return {};
    }

    std::variant<Status, DuplicateKey> insert(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        const key_string::View& keyString,
        bool dupsAllowed,
        IncludeDuplicateRecordId includeDuplicateRecordId) override {
        return Status::OK();
    }

    void unindex(OperationContext* opCtx,
                 RecoveryUnit& ru,
                 const key_string::View& keyString,
                 bool dupsAllowed) override {}

    boost::optional<DuplicateKey> dupKeyCheck(OperationContext* opCtx,
                                              RecoveryUnit& ru,
                                              const key_string::View& keyString) override {
        return boost::none;
    }

    boost::optional<RecordId> findLoc(OperationContext* opCtx,
                                      RecoveryUnit& ru,
                                      std::span<const char> keyString) const override {
        return boost::none;
    }

    IndexValidateResults validate(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        const CollectionValidation::ValidationOptions& options) const override {
        return IndexValidateResults{};
    }

    bool appendCustomStats(OperationContext* opCtx,
                           RecoveryUnit& ru,
                           BSONObjBuilder* output,
                           double scale) const override {
        return false;
    }

    long long getSpaceUsedBytes(OperationContext* opCtx, RecoveryUnit& ru) const override {
        return 0;
    }

    long long getFreeStorageBytes(OperationContext* opCtx, RecoveryUnit& ru) const override {
        return 0;
    }

    bool isEmpty(OperationContext* opCtx, RecoveryUnit& ru) override {
        return true;
    }

    int64_t numEntries(OperationContext* opCtx, RecoveryUnit& ru) const override {
        return 0;
    }

    void printIndexEntryMetadata(OperationContext* opCtx,
                                 RecoveryUnit& ru,
                                 const key_string::View& keyString) const override {}

    std::unique_ptr<SortedDataInterface::Cursor> newCursor(OperationContext* opCtx,
                                                           RecoveryUnit& ru,
                                                           bool isForward) const override {
        return {};
    }

    Status initAsEmpty() override {
        return Status::OK();
    }

    Status truncate(OperationContext* opCtx, RecoveryUnit& ru) override {
        return Status::OK();
    }

    StringKeyedContainer& getContainer() override {
        return _container;
    }

    const StringKeyedContainer& getContainer() const override {
        return _container;
    }

private:
    DevNullStringKeyedContainer _container;
};

DevNullKVEngine::DevNullKVEngine() : _engineDbPath(storageGlobalParams.dbpath) {
    auto testFilePath = _engineDbPath / "testFile.txt";
    _mockBackupBlocks.push_back(KVBackupBlock(/*ident=*/"", /*filePath=*/testFilePath.string()));
}

DevNullKVEngine::~DevNullKVEngine() = default;

std::unique_ptr<RecoveryUnit> DevNullKVEngine::newRecoveryUnit() {
    return std::make_unique<RecoveryUnitNoop>();
}

std::unique_ptr<RecordStore> DevNullKVEngine::getRecordStore(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             StringData ident,
                                                             const RecordStore::Options& options,
                                                             boost::optional<UUID> uuid) {
    if (ident == ident::kMbdCatalog) {
        return std::make_unique<EphemeralForTestRecordStore>(uuid, ident, &_catalogInfo);
    } else if (options.isOplog) {
        return std::make_unique<DevNullRecordStore::Oplog>(*uuid, ident, options.oplogMaxSize);
    } else if (options.isCapped) {
        return std::make_unique<DevNullRecordStore::Capped>(uuid, ident, options.keyFormat);
    }
    return std::make_unique<DevNullRecordStore>(uuid, ident, options.keyFormat);
}

std::unique_ptr<RecordStore> DevNullKVEngine::getTemporaryRecordStore(RecoveryUnit& ru,
                                                                      StringData ident,
                                                                      KeyFormat keyFormat) {
    return makeTemporaryRecordStore(ru, ident, keyFormat);
}

std::unique_ptr<RecordStore> DevNullKVEngine::makeTemporaryRecordStore(RecoveryUnit& ru,
                                                                       StringData ident,
                                                                       KeyFormat keyFormat) {
    return std::make_unique<DevNullRecordStore>(boost::none /* uuid */, ident, keyFormat);
}

std::unique_ptr<SortedDataInterface> DevNullKVEngine::getSortedDataInterface(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    const NamespaceString& nss,
    const UUID& uuid,
    StringData ident,
    const IndexConfig& config,
    KeyFormat keyFormat) {
    return std::make_unique<DevNullSortedDataInterface>(ident);
}

namespace {

class StreamingCursorImpl : public StorageEngine::StreamingCursor {
public:
    StreamingCursorImpl() = delete;
    StreamingCursorImpl(StorageEngine::BackupOptions options,
                        std::deque<KVBackupBlock> kvBackupBlocks)
        : StorageEngine::StreamingCursor(options), _kvBackupBlocks(std::move(kvBackupBlocks)) {
        _exhaustCursor = false;
    };

    ~StreamingCursorImpl() override = default;

    BSONObj getMetadataObject(UUID backupId) {
        return BSONObj();
    }

    StatusWith<std::deque<KVBackupBlock>> getNextBatch(const std::size_t batchSize) override {
        if (_exhaustCursor) {
            std::deque<KVBackupBlock> emptyVector;
            return emptyVector;
        }
        _exhaustCursor = true;
        return _kvBackupBlocks;
    }

private:
    std::deque<KVBackupBlock> _kvBackupBlocks;
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
