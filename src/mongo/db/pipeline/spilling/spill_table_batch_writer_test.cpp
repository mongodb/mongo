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

#include "mongo/db/pipeline/spilling/spill_table_batch_writer.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/spilling/spilling_test_fixture.h"

namespace mongo {
namespace {

class RecordStoreBatchWriterTest : public SpillingTestFixture {
public:
    std::unique_ptr<SpillTable> createRecordStore() const {
        return _expCtx->getMongoProcessInterface()->createSpillTable(_expCtx, KeyFormat::Long);
    }

    void assertRecordStoreContent(SpillTable& spillTable, std::vector<BSONObj> records) const {
        for (size_t i = 0; i < records.size(); ++i) {
            const auto docFromStore = _expCtx->getMongoProcessInterface()->readRecordFromSpillTable(
                _expCtx, spillTable, RecordId{static_cast<int64_t>(i + 1)});
            ASSERT_DOCUMENT_EQ(Document{records[i]}, docFromStore);
        }
    }

    std::string generateString(size_t size) const {
        std::string result;
        result.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            result.push_back('a' + (i % 26));
        }
        return result;
    }
};

TEST_F(RecordStoreBatchWriterTest, CanWriteUnownedDocuments) {
    std::vector<BSONObj> records = {
        BSON("_id" << 0 << "data" << 3),
        BSON("_id" << 1 << "data" << 1),
        BSON("_id" << 2 << "data" << 4),
        BSON("_id" << 3 << "data" << 1),
        BSON("_id" << 4 << "data" << 5),
        BSON("_id" << 4 << "data" << 9),
        BSON("_id" << 4 << "data" << 2),
        BSON("_id" << 4 << "data" << 6),
        BSON("_id" << 4 << "data" << 5),
    };

    // Create independent copy for later assertions
    std::vector<BSONObj> recordsCopy;
    for (const auto& record : records) {
        recordsCopy.push_back(record.copy());
    }

    auto rs = createRecordStore();
    SpillTableBatchWriter writer{_expCtx.get(), *rs};

    for (size_t i = 0; i < records.size(); ++i) {
        RecordId recordId{static_cast<int64_t>(i + 1)};
        BSONObj unowned{records[i].objdata()};
        writer.write(recordId, unowned);
        // Destory the only owner. RecordStoreBatchWriter should preserve a copy if original was not
        // owned.
        records[i] = BSONObj{};

        if (i % 3 == 2) {
            writer.flush();
        }
    }

    writer.flush();
    assertRecordStoreContent(*rs, recordsCopy);
}

}  // namespace
}  // namespace mongo
