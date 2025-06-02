/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/spill_wiredtiger_record_store.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/spill_wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class SpillWiredTigerRecordStoreTest : public ServiceContextTest {
protected:
    SpillWiredTigerRecordStoreTest() : _dbpath("wt_test"), _opCtx(makeOperationContext()) {
        WiredTigerKVEngineBase::WiredTigerConfig wtConfig =
            getWiredTigerConfigFromStartupOptions(true /* usingSpillWiredTigerKVEngine */);
        wtConfig.cacheSizeMB = 1;
        wtConfig.inMemory = true;
        wtConfig.logEnabled = false;
        _kvEngine = std::make_unique<SpillWiredTigerKVEngine>(
            std::string{kWiredTigerEngineName}, _dbpath.path(), &_clockSource, std::move(wtConfig));

        _recordStore = makeTemporaryRecordStore("a.b", KeyFormat::Long);
        ASSERT_TRUE(_kvEngine->hasIdent(
            _recordStore->getRecoveryUnit(*shard_role_details::getRecoveryUnit(_opCtx.get())),
            "a.b"));
    }

    std::unique_ptr<SpillWiredTigerRecordStore> makeTemporaryRecordStore(const std::string& ns,
                                                                         KeyFormat keyFormat) {
        StringData ident = ns;
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
        auto rs = _kvEngine->makeTemporaryRecordStore(_opCtx.get(), ident, keyFormat);
        return std::unique_ptr<SpillWiredTigerRecordStore>(
            dynamic_cast<SpillWiredTigerRecordStore*>(rs.release()));
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() {
        return std::unique_ptr<RecoveryUnit>(_kvEngine->newRecoveryUnit());
    }

    unittest::TempDir _dbpath;
    ClockSourceMock _clockSource;
    std::unique_ptr<SpillWiredTigerKVEngine> _kvEngine;
    std::unique_ptr<SpillWiredTigerRecordStore> _recordStore;
    ServiceContext::UniqueOperationContext _opCtx;
};

// Test that insertRecord() works as expected.
TEST_F(SpillWiredTigerRecordStoreTest, InsertRecord) {
    std::vector<std::string> recordDataVec(11);
    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        std::ostringstream oss;
        oss << std::setw(5) << std::setfill('0') << i;
        recordDataVec[i] = oss.str();
        auto statusWith = _recordStore->insertRecord(_opCtx.get(),
                                                     recordId,
                                                     recordDataVec[i].c_str(),
                                                     recordDataVec[i].size(),
                                                     Timestamp{1});
        ASSERT_TRUE(statusWith.isOK());
        ASSERT_EQ(recordId, statusWith.getValue());
        ASSERT_EQ(i, _recordStore->numRecords());
        ASSERT_EQ(i * 5, _recordStore->dataSize());
    }

    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        RecordData recordData;
        bool found = _recordStore->findRecord(_opCtx.get(), recordId, &recordData);
        ASSERT_TRUE(found);
        ASSERT_EQ(StringData(recordData.data(), recordData.size()), StringData(recordDataVec[i]));
    }
}

// Test that updateRecord() works as expected.
TEST_F(SpillWiredTigerRecordStoreTest, UpdateRecord) {
    std::vector<std::string> recordDataVec(11);
    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        std::ostringstream oss;
        oss << std::setw(5) << std::setfill('0') << i;
        recordDataVec[i] = oss.str();
        auto statusWith = _recordStore->insertRecord(_opCtx.get(),
                                                     recordId,
                                                     recordDataVec[i].c_str(),
                                                     recordDataVec[i].size(),
                                                     Timestamp{1});
        ASSERT_TRUE(statusWith.isOK());
        ASSERT_EQ(recordId, statusWith.getValue());
        ASSERT_EQ(i, _recordStore->numRecords());
        ASSERT_EQ(i * 5, _recordStore->dataSize());
    }

    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        recordDataVec[i] += "00";
        auto status = _recordStore->updateRecord(
            _opCtx.get(), recordId, recordDataVec[i].c_str(), recordDataVec[i].size());
        ASSERT_TRUE(status.isOK());
        ASSERT_EQ(10, _recordStore->numRecords());
        ASSERT_EQ(50 + i * 2, _recordStore->dataSize());
    }

    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        RecordData recordData;
        bool found = _recordStore->findRecord(_opCtx.get(), recordId, &recordData);
        ASSERT_TRUE(found);
        ASSERT_EQ(StringData(recordData.data(), recordData.size()), StringData(recordDataVec[i]));
    }
}

// Test that deleteRecord() works as expected.
TEST_F(SpillWiredTigerRecordStoreTest, DeleteRecord) {
    std::vector<std::string> recordDataVec(11);
    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        std::ostringstream oss;
        oss << std::setw(5) << std::setfill('0') << i;
        recordDataVec[i] = oss.str();
        auto statusWith = _recordStore->insertRecord(_opCtx.get(),
                                                     recordId,
                                                     recordDataVec[i].c_str(),
                                                     recordDataVec[i].size(),
                                                     Timestamp{1});
        ASSERT_TRUE(statusWith.isOK());
        ASSERT_EQ(recordId, statusWith.getValue());
        ASSERT_EQ(i, _recordStore->numRecords());
        ASSERT_EQ(i * 5, _recordStore->dataSize());
    }

    for (int32_t i = 2; i <= 7; ++i) {
        _recordStore->deleteRecord(_opCtx.get(), RecordId(i));
    }

    ASSERT_EQ(4, _recordStore->numRecords());
    ASSERT_EQ(20, _recordStore->dataSize());

    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        RecordData recordData;
        bool found = _recordStore->findRecord(_opCtx.get(), recordId, &recordData);
        ASSERT_EQ(found, i < 2 || i > 7);
        if (found) {
            ASSERT_EQ(StringData(recordData.data(), recordData.size()),
                      StringData(recordDataVec[i]));
        }
    }
}

// Test that truncate() works as expected.
TEST_F(SpillWiredTigerRecordStoreTest, Truncate) {
    std::vector<std::string> recordDataVec(11);
    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        std::ostringstream oss;
        oss << std::setw(5) << std::setfill('0') << i;
        recordDataVec[i] = oss.str();
        auto statusWith = _recordStore->insertRecord(_opCtx.get(),
                                                     recordId,
                                                     recordDataVec[i].c_str(),
                                                     recordDataVec[i].size(),
                                                     Timestamp{1});
        ASSERT_TRUE(statusWith.isOK());
        ASSERT_EQ(recordId, statusWith.getValue());
        ASSERT_EQ(i, _recordStore->numRecords());
        ASSERT_EQ(i * 5, _recordStore->dataSize());
    }

    auto status = _recordStore->truncate(_opCtx.get());
    ASSERT_TRUE(status.isOK());

    ASSERT_EQ(0, _recordStore->numRecords());
    ASSERT_EQ(0, _recordStore->dataSize());

    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        RecordData recordData;
        bool found = _recordStore->findRecord(_opCtx.get(), recordId, &recordData);
        ASSERT_FALSE(found);
    }
}

// Test that rangeTruncate() works as expected.
TEST_F(SpillWiredTigerRecordStoreTest, RangeTruncate) {
    std::vector<std::string> recordDataVec(11);
    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        std::ostringstream oss;
        oss << std::setw(5) << std::setfill('0') << i;
        recordDataVec[i] = oss.str();
        auto statusWith = _recordStore->insertRecord(_opCtx.get(),
                                                     recordId,
                                                     recordDataVec[i].c_str(),
                                                     recordDataVec[i].size(),
                                                     Timestamp{1});
        ASSERT_TRUE(statusWith.isOK());
        ASSERT_EQ(recordId, statusWith.getValue());
        ASSERT_EQ(i, _recordStore->numRecords());
        ASSERT_EQ(i * 5, _recordStore->dataSize());
    }

    auto status = _recordStore->rangeTruncate(_opCtx.get(),
                                              RecordId(2),
                                              RecordId(7),
                                              -(6 * 5) /* hintDataSizeIncrement */,
                                              -6 /* hintNumRecordsIncrement */);

    ASSERT_TRUE(status.isOK());

    ASSERT_EQ(4, _recordStore->numRecords());
    ASSERT_EQ(20, _recordStore->dataSize());

    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        RecordData recordData;
        bool found = _recordStore->findRecord(_opCtx.get(), recordId, &recordData);
        ASSERT_EQ(found, i < 2 || i > 7);
        if (found) {
            ASSERT_EQ(StringData(recordData.data(), recordData.size()),
                      StringData(recordDataVec[i]));
        }
    }
}

// Test that RecordCursor works as expected.
TEST_F(SpillWiredTigerRecordStoreTest, RecordCursor) {
    auto& ru = *shard_role_details::getRecoveryUnit(_opCtx.get());

    std::vector<std::string> recordDataVec(11);
    for (int32_t i = 1; i <= 10; ++i) {
        RecordId recordId(i);
        std::ostringstream oss;
        oss << std::setw(5) << std::setfill('0') << i;
        recordDataVec[i] = oss.str();
        auto statusWith = _recordStore->insertRecord(_opCtx.get(),
                                                     recordId,
                                                     recordDataVec[i].c_str(),
                                                     recordDataVec[i].size(),
                                                     Timestamp{1});
        ASSERT_TRUE(statusWith.isOK());
        ASSERT_EQ(recordId, statusWith.getValue());
        ASSERT_EQ(i, _recordStore->numRecords());
        ASSERT_EQ(i * 5, _recordStore->dataSize());
    }

    auto cursor = _recordStore->getCursor(_opCtx.get(), ru, true /* forward */);
    for (int32_t i = 1; i <= 10; ++i) {
        auto record = cursor->next();
        ASSERT_TRUE(record);
        ASSERT_EQ(StringData(record->data.data(), record->data.size()),
                  StringData(recordDataVec[i]));
    }
    ASSERT_FALSE(cursor->next());

    // Test seek() in {forward, kInclude} mode.
    cursor = _recordStore->getCursor(_opCtx.get(), ru, true /* forward */);
    auto record = cursor->seek(RecordId(5), SeekableRecordCursor::BoundInclusion::kInclude);
    for (int32_t i = 5; i <= 10; ++i) {
        ASSERT_TRUE(record);
        ASSERT_EQ(StringData(record->data.data(), record->data.size()),
                  StringData(recordDataVec[i]));
        record = cursor->next();
    }
    ASSERT_FALSE(cursor->next());

    // Test seekExact() in forward mode.
    cursor = _recordStore->getCursor(_opCtx.get(), ru, true /* forward */);
    record = cursor->seekExact(RecordId(5));
    for (int32_t i = 5; i <= 10; ++i) {
        ASSERT_TRUE(record);
        ASSERT_EQ(StringData(record->data.data(), record->data.size()),
                  StringData(recordDataVec[i]));
        record = cursor->next();
    }
    ASSERT_FALSE(cursor->next());

    // Test seek() in {forward, kExclude} mode.
    cursor = _recordStore->getCursor(_opCtx.get(), ru, true /* forward */);
    record = cursor->seek(RecordId(5), SeekableRecordCursor::BoundInclusion::kExclude);
    for (int32_t i = 6; i <= 10; ++i) {
        ASSERT_TRUE(record);
        ASSERT_EQ(StringData(record->data.data(), record->data.size()),
                  StringData(recordDataVec[i]));
        record = cursor->next();
    }
    ASSERT_FALSE(cursor->next());


    // Test seek() in {backward, kInclude} mode.
    cursor = _recordStore->getCursor(_opCtx.get(), ru, false /* forward */);
    record = cursor->seek(RecordId(5), SeekableRecordCursor::BoundInclusion::kInclude);
    for (int32_t i = 5; i >= 1; --i) {
        ASSERT_TRUE(record);
        ASSERT_EQ(StringData(record->data.data(), record->data.size()),
                  StringData(recordDataVec[i]));
        record = cursor->next();
    }
    ASSERT_FALSE(cursor->next());

    // Test seekExact() in backward mode.
    cursor = _recordStore->getCursor(_opCtx.get(), ru, false /* forward */);
    record = cursor->seekExact(RecordId(5));
    for (int32_t i = 5; i >= 1; --i) {
        ASSERT_TRUE(record);
        ASSERT_EQ(StringData(record->data.data(), record->data.size()),
                  StringData(recordDataVec[i]));
        record = cursor->next();
    }
    ASSERT_FALSE(cursor->next());

    // Test seek() in {backward, kExclude} mode.
    cursor = _recordStore->getCursor(_opCtx.get(), ru, false /* forward */);
    record = cursor->seek(RecordId(5), SeekableRecordCursor::BoundInclusion::kExclude);
    for (int32_t i = 4; i >= 1; --i) {
        ASSERT_TRUE(record);
        ASSERT_EQ(StringData(record->data.data(), record->data.size()),
                  StringData(recordDataVec[i]));
        record = cursor->next();
    }
    ASSERT_FALSE(cursor->next());
}

// Test that tables get created and deleted as expected.
TEST_F(SpillWiredTigerRecordStoreTest, TableCreation) {
    SpillRecoveryUnit wtRu(&_kvEngine->getConnection());

    std::vector<std::unique_ptr<SpillWiredTigerRecordStore>> recordStores;
    for (int i = 0; i < 3; ++i) {
        auto ident = "a." + std::to_string(i + 1);
        auto recordStore = makeTemporaryRecordStore(ident, KeyFormat::Long);
        ASSERT_TRUE(_kvEngine->hasIdent(wtRu, ident));
        recordStores.push_back(std::move(recordStore));
    }

    auto allIdents = _kvEngine->getAllIdents(wtRu);
    stdx::unordered_set<std::string> allIdentsSet(allIdents.begin(), allIdents.end());
    ASSERT_EQ(allIdentsSet.size(), 4);
    ASSERT_TRUE(allIdentsSet.contains("a.b"));
    for (int i = 0; i < 3; ++i) {
        auto ident = "a." + std::to_string(i + 1);
        ASSERT_TRUE(allIdentsSet.contains(ident));
    }

    for (int i = 0; i < 3; ++i) {
        ASSERT_OK(recordStores[i]->truncate(_opCtx.get()));
        recordStores[i].reset();

        auto ident = "a." + std::to_string(i + 1);
        ASSERT_OK(_kvEngine->dropIdent(wtRu, ident, false));
        ASSERT_FALSE(_kvEngine->hasIdent(wtRu, ident));
    }

    allIdents = _kvEngine->getAllIdents(wtRu);
    allIdentsSet.clear();
    allIdentsSet.insert(allIdents.begin(), allIdents.end());
    ASSERT_EQ(allIdentsSet.size(), 1);
    ASSERT_TRUE(allIdentsSet.contains("a.b"));
}

}  // namespace
}  // namespace mongo
