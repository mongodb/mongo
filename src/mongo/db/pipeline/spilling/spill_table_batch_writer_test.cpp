// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
