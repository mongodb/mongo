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
#include "mongo/db/catalog/virtual_collection_options.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"

namespace mongo {
class ExternalRecordStore : public RecordStore {
public:
    ExternalRecordStore(StringData ns, const VirtualCollectionOptions& vopts);

    const VirtualCollectionOptions& getOptions() const {
        return _vopts;
    }

    const char* name() const {
        return "external";
    }

    bool isTemp() const {
        return true;
    }

    KeyFormat keyFormat() const final {
        return KeyFormat::Long;
    }

    long long dataSize(OperationContext*) const final {
        return 0LL;
    }

    long long numRecords(OperationContext*) const final {
        return 0LL;
    }

    int64_t storageSize(OperationContext*, BSONObjBuilder*, int) const final {
        return 0LL;
    }

    bool findRecord(OperationContext*, const RecordId&, RecordData*) const final {
        unimplementedTasserted();
        return false;
    }

    bool updateWithDamagesSupported() const final {
        return false;
    }

    void printRecordMetadata(OperationContext*, const RecordId&) const final {
        unimplementedTasserted();
    }

    std::unique_ptr<SeekableRecordCursor> getCursor(OperationContext* opCtx,
                                                    bool forward = true) const final;

    std::unique_ptr<RecordCursor> getRandomCursor(OperationContext* opCtx) const final {
        unimplementedTasserted();
        return nullptr;
    }

    void appendNumericCustomStats(OperationContext*, BSONObjBuilder*, double) const final {}

    void updateStatsAfterRepair(OperationContext* opCtx,
                                long long numRecords,
                                long long dataSize) final {
        unimplementedTasserted();
    }

protected:
    void doDeleteRecord(OperationContext*, const RecordId&) final {
        unimplementedTasserted();
    }

    Status doInsertRecords(OperationContext*,
                           std::vector<Record>*,
                           const std::vector<Timestamp>&) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    Status doUpdateRecord(OperationContext*, const RecordId&, const char*, int) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    StatusWith<RecordData> doUpdateWithDamages(OperationContext*,
                                               const RecordId&,
                                               const RecordData&,
                                               const char*,
                                               const mutablebson::DamageVector&) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    Status doTruncate(OperationContext* opCtx) final {
        unimplementedTasserted();
        return {ErrorCodes::Error::UnknownError, "Unknown error"};
    }

    void doCappedTruncateAfter(OperationContext*,
                               const RecordId&,
                               bool,
                               const AboutToDeleteRecordCallback&) final {
        unimplementedTasserted();
    }

    void waitForAllEarlierOplogWritesToBeVisibleImpl(OperationContext*) const final {
        unimplementedTasserted();
    }

private:
    void unimplementedTasserted() const {
        MONGO_UNIMPLEMENTED_TASSERT(6968600);
    }

    VirtualCollectionOptions _vopts;
};
}  // namespace mongo
