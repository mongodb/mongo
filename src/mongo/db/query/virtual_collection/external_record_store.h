/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/container_base.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

// TODO(SERVER-110243): Use TestIntegerKeyedContainer
class ExternalIntegerKeyedContainer : public IntegerKeyedContainerBase {
public:
    ExternalIntegerKeyedContainer() : IntegerKeyedContainerBase(nullptr) {}

    Status insert(RecoveryUnit& ru, int64_t key, std::span<const char> value) final {
        return Status::OK();
    }

    Status remove(RecoveryUnit& ru, int64_t key) final {
        return Status::OK();
    }
};

class ExternalStringKeyedContainer : public StringKeyedContainerBase {
public:
    ExternalStringKeyedContainer() : StringKeyedContainerBase(nullptr) {}

    Status insert(RecoveryUnit& ru, std::span<const char> key, std::span<const char> value) final {
        return Status::OK();
    }

    Status remove(RecoveryUnit& ru, std::span<const char> key) final {
        return Status::OK();
    }
};

class ExternalRecordStore : public RecordStore {
public:
    ExternalRecordStore(boost::optional<UUID> uuid, const VirtualCollectionOptions& vopts);

    const VirtualCollectionOptions& getOptions() const {
        return _vopts;
    }

    const char* name() const override {
        return "external";
    }

    boost::optional<UUID> uuid() const final {
        return boost::none;
    }

    bool isTemp() const final {
        return true;
    }

    std::shared_ptr<Ident> getSharedIdent() const final {
        unimplementedTasserted();
        return nullptr;
    }

    StringData getIdent() const final {
        unimplementedTasserted();
        static std::string ident;
        return ident;
    }

    void setIdent(std::shared_ptr<Ident>) final {
        unimplementedTasserted();
    }

    KeyFormat keyFormat() const final {
        return KeyFormat::Long;
    }

    long long dataSize() const final {
        return 0LL;
    }

    long long numRecords() const final {
        return 0LL;
    }

    int64_t storageSize(RecoveryUnit&, BSONObjBuilder*, int) const final {
        return 0LL;
    }

    int64_t freeStorageSize(RecoveryUnit&) const final {
        return 0ULL;
    }

    RecordData dataFor(OperationContext*, RecoveryUnit&, const RecordId&) const final {
        unimplementedTasserted();
        return {};
    }

    bool findRecord(OperationContext*, RecoveryUnit&, const RecordId&, RecordData*) const final {
        unimplementedTasserted();
        return false;
    }

    void deleteRecord(OperationContext* opCtx, RecoveryUnit&, const RecordId& dl) final {
        unimplementedTasserted();
    }

    Status insertRecords(OperationContext*,
                         RecoveryUnit&,
                         std::vector<Record>*,
                         const std::vector<Timestamp>&) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    StatusWith<RecordId> insertRecord(
        OperationContext*, RecoveryUnit&, const char* data, int len, Timestamp) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    StatusWith<RecordId> insertRecord(OperationContext*,
                                      RecoveryUnit&,
                                      const RecordId&,
                                      const char* data,
                                      int len,
                                      Timestamp) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    Status updateRecord(
        OperationContext*, RecoveryUnit&, const RecordId&, const char* data, int len) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    bool updateWithDamagesSupported() const final {
        return false;
    }

    StatusWith<RecordData> updateWithDamages(OperationContext* opCtx,
                                             RecoveryUnit&,
                                             const RecordId& loc,
                                             const RecordData& oldRec,
                                             const char* damageSource,
                                             const DamageVector& damages) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    void printRecordMetadata(const RecordId&, std::set<Timestamp>* recordTimestamps) const final {
        unimplementedTasserted();
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    RecoveryUnit& ru,
                                                    bool forward = true) const final;

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext* opCtx,
                                                  RecoveryUnit& ru) const final {
        unimplementedTasserted();
        return nullptr;
    }

    Status truncate(OperationContext*, RecoveryUnit&) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    Status rangeTruncate(OperationContext*,
                         RecoveryUnit&,
                         const RecordId& minRecordId = RecordId(),
                         const RecordId& maxRecordId = RecordId(),
                         int64_t hintDataSizeIncrement = 0,
                         int64_t hintNumRecordsIncrement = 0) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    bool compactSupported() const final {
        return false;
    }

    StatusWith<int64_t> compact(OperationContext*, RecoveryUnit&, const CompactOptions&) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    void validate(RecoveryUnit&,
                  const CollectionValidation::ValidationOptions&,
                  ValidateResults*) final {
        unimplementedTasserted();
    }

    void appendNumericCustomStats(RecoveryUnit&, BSONObjBuilder*, double) const final {}

    void appendAllCustomStats(RecoveryUnit&, BSONObjBuilder*, double scale) const final {}

    RecordId getLargestKey(OperationContext*, RecoveryUnit&) const final {
        unimplementedTasserted();
        return {};
    }

    void reserveRecordIds(OperationContext*,
                          RecoveryUnit&,
                          std::vector<RecordId>*,
                          size_t numRecords) final {
        unimplementedTasserted();
    }

    void updateStatsAfterRepair(long long numRecords, long long dataSize) final {
        unimplementedTasserted();
    }

    RecordStore::Capped* capped() final {
        return nullptr;
    }

    RecordStore::Oplog* oplog() final {
        return nullptr;
    }

    RecordStore::RecordStoreContainer getContainer() override;

private:
    void unimplementedTasserted() const {
        MONGO_UNIMPLEMENTED_TASSERT(6968600);
    }

    std::variant<ExternalIntegerKeyedContainer, ExternalStringKeyedContainer> _makeContainer();

    VirtualCollectionOptions _vopts;
    std::variant<ExternalIntegerKeyedContainer, ExternalStringKeyedContainer> _container;
};
}  // namespace mongo
