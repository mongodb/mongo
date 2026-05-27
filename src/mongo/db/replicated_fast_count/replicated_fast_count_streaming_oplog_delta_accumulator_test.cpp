/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <list>
#include <utility>

namespace mongo::replicated_fast_count {
namespace {

/**
 * In-memory oplog-cursor stub for driving StreamingOplogDeltaAccumulator from a list of
 * pre-built OplogEntries. Mirrors the cursor's iteration order; does not model oplog visibility.
 */
class OplogCursorMock : public SeekableRecordCursor {
public:
    OplogCursorMock(std::list<repl::OplogEntry> entries) {
        for (const auto& entry : entries) {
            _records.emplace_back(RecordId(entry.getTimestamp().asULL()),
                                  entry.getEntry().toBSON().getOwned());
        }
    }

    ~OplogCursorMock() override {}

    boost::optional<Record> next() override {
        if (_records.empty()) {
            return boost::none;
        }

        if (!_initialized) {
            _initialized = true;
            _it = _records.cbegin();
        } else {
            ++_it;
        }

        if (_it == _records.cend()) {
            _initialized = false;
            return boost::none;
        }

        return Record{_it->first, RecordData(_it->second.objdata(), _it->second.objsize())};
    }

    boost::optional<Record> seekExact(const RecordId& id) override {
        for (auto it = _records.cbegin(); it != _records.cend(); ++it) {
            if (it->first == id) {
                _initialized = true;
                _it = it;
                return Record{it->first, RecordData(it->second.objdata(), it->second.objsize())};
            }
        }
        _initialized = false;
        return boost::none;
    }

    void save() override {}
    bool restore(RecoveryUnit&, bool) override {
        return true;
    }
    void detachFromOperationContext() override {}
    void reattachToOperationContext(OperationContext*) override {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}

    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override {
        invariant(boundInclusion == BoundInclusion::kExclude);
        for (auto it = _records.cbegin(); it != _records.cend(); ++it) {
            if (it->first > start) {
                _initialized = true;
                _it = it;
                return Record{it->first, RecordData(it->second.objdata(), it->second.objsize())};
            }
        }
        _initialized = false;
        return {};
    }

private:
    bool _initialized = false;
    std::list<std::pair<RecordId, BSONObj>> _records;
    std::list<std::pair<RecordId, BSONObj>>::const_iterator _it;
};

class StreamingOplogDeltaAccumulatorTest : public CatalogTestFixture {
protected:
    static constexpr int64_t kTestTerm = 1;

    repl::OpTime opTimeAt(Timestamp ts) const {
        return repl::OpTime(ts, kTestTerm);
    }

    /**
     * Builds a partial applyOps oplog entry (partialTxn: true) at 'ts' whose single inner
     * op is an insert of 'sizeDelta' bytes into 'coll'. 'prevOpTime' links this entry into
     * a chained-transaction sequence: pass repl::OpTime{} for the first entry in a chain,
     * or the opTime of the preceding partial entry for continuations.
     */
    repl::OplogEntry makePartialApplyOpsEntry(Timestamp ts,
                                              const test_helpers::NsAndUUID& coll,
                                              int32_t sizeDelta,
                                              repl::OpTime prevOpTime) {
        BSONObj innerOp = BSON("op" << "i"
                                    << "ns" << coll.nss.ns_forTest() << "ui" << coll.uuid << "o"
                                    << BSONObj() << "m" << BSON("sz" << sizeDelta));
        BSONObj oField = BSON("applyOps" << BSON_ARRAY(innerOp) << "partialTxn" << true);
        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = opTimeAt(ts),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = NamespaceString::kAdminCommandNamespace,
            .oField = oField,
            .wallClockTime = Date_t::now(),
            .prevWriteOpTimeInTransaction = prevOpTime,
        }};
    }

    OplogScanResult runAccumulator(StreamingOplogDeltaAccumulator::Options opts,
                                   std::list<repl::OplogEntry> entries) {
        OplogCursorMock cursor(std::move(entries));
        StreamingOplogDeltaAccumulator acc(std::move(opts));
        while (auto rec = cursor.next()) {
            acc.consumeRecord(*rec);
        }
        return acc.finish();
    }

    test_helpers::NsAndUUID collA = {.nss = NamespaceString::createNamespaceString_forTest(
                                         "streaming_accumulator_test", "collA"),
                                     .uuid = UUID::gen()};
    test_helpers::NsAndUUID collB = {.nss = NamespaceString::createNamespaceString_forTest(
                                         "streaming_accumulator_test", "collB"),
                                     .uuid = UUID::gen()};
    test_helpers::NsAndUUID fastCountColl = {.nss = NamespaceString::makeGlobalConfigCollection(
                                                 NamespaceString::kReplicatedFastCountStore),
                                             .uuid = UUID::gen()};
    const UUID oplogUuid = UUID::gen();
};

TEST_F(StreamingOplogDeltaAccumulatorTest, EmptyStream_NoLastTimestampNoDeltas) {
    const auto result = runAccumulator({}, {});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_FALSE(result.lastTimestamp);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, SingleEntry_AccumulatesAndAdvancesTimestamp) {
    const Timestamp ts{1, 5};
    const auto result = runAccumulator(
        {}, {test_helpers::makeOplogEntry(ts, collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 10);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 1);
    EXPECT_EQ(result.lastTimestamp, ts);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, MultipleEntries_LastTimestampIsLatestNonInternalEntry) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    const auto result =
        runAccumulator({},
                       {test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 10),
                        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 20),
                        test_helpers::makeOplogEntry(ts3, collB, repl::OpTypeEnum::kInsert, 30)});
    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 30);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 2);
    ASSERT_TRUE(result.deltas.contains(collB.uuid));
    EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount.size, 30);
    EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount.count, 1);
    EXPECT_EQ(result.lastTimestamp, ts3);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, UuidFilter_OnlyMatchingUuidAccumulated) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const auto result =
        runAccumulator({.uuidFilter = collA.uuid},
                       {test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 10),
                        test_helpers::makeOplogEntry(ts2, collB, repl::OpTypeEnum::kInsert, 20)});
    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_FALSE(result.deltas.contains(collB.uuid));
    // lastTimestamp tracks the latest non-internal entry regardless of the UUID filter.
    EXPECT_EQ(result.lastTimestamp, ts2);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, OplogUuid_TracksRawBytesAcrossAllRecords) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    auto entry1 = test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 10);
    auto entry2 = test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 20);
    const int64_t expectedBytes =
        entry1.getEntry().toBSON().objsize() + entry2.getEntry().toBSON().objsize();
    const auto result =
        runAccumulator({.oplogUuid = oplogUuid}, {std::move(entry1), std::move(entry2)});
    ASSERT_TRUE(result.deltas.contains(oplogUuid));
    EXPECT_EQ(result.deltas.at(oplogUuid).sizeCount.size, expectedBytes);
    EXPECT_EQ(result.deltas.at(oplogUuid).sizeCount.count, 2);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, OplogUuid_TrackedEvenForInternalEntries) {
    const Timestamp ts1{1, 1};
    auto entry = test_helpers::makeOplogEntry(ts1, fastCountColl, repl::OpTypeEnum::kInsert, 10);
    const int64_t expectedBytes = entry.getEntry().toBSON().objsize();
    const auto result = runAccumulator({.oplogUuid = oplogUuid}, {std::move(entry)});
    ASSERT_TRUE(result.deltas.contains(oplogUuid));
    EXPECT_EQ(result.deltas.at(oplogUuid).sizeCount.size, expectedBytes);
    EXPECT_EQ(result.deltas.at(oplogUuid).sizeCount.count, 1);
    EXPECT_FALSE(result.lastTimestamp);
    EXPECT_FALSE(result.deltas.contains(fastCountColl.uuid));
}

TEST_F(StreamingOplogDeltaAccumulatorTest, InternalOnly_NoLastTimestamp) {
    const Timestamp ts1{1, 1};
    const auto result = runAccumulator(
        {}, {test_helpers::makeOplogEntry(ts1, fastCountColl, repl::OpTypeEnum::kInsert, 10)});
    EXPECT_FALSE(result.lastTimestamp);
    EXPECT_TRUE(result.deltas.empty());
}

TEST_F(StreamingOplogDeltaAccumulatorTest, InternalThenUser_LastTimestampIsUserEntry) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const auto result = runAccumulator(
        {},
        {test_helpers::makeOplogEntry(ts1, fastCountColl, repl::OpTypeEnum::kInsert, 10),
         test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 50)});
    EXPECT_EQ(result.lastTimestamp, ts2);
    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 50);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 1);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, UserThenInternal_LastTimestampDoesNotRegress) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const auto result = runAccumulator(
        {},
        {test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 50),
         test_helpers::makeOplogEntry(ts2, fastCountColl, repl::OpTypeEnum::kInsert, 10)});
    // Internal entry must not advance lastTimestamp past the latest non-internal entry.
    EXPECT_EQ(result.lastTimestamp, ts1);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, Checkpoint_ErasesOplogUuidWhenOnlyInternalEntries) {
    const Timestamp ts1{1, 1};
    const auto result = runAccumulator(
        {.isCheckpoint = true, .oplogUuid = oplogUuid},
        {test_helpers::makeOplogEntry(ts1, fastCountColl, repl::OpTypeEnum::kInsert, 10)});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_FALSE(result.lastTimestamp);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, Checkpoint_KeepsOplogUuidWhenUserEntriesPresent) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    auto fastCountEntry =
        test_helpers::makeOplogEntry(ts1, fastCountColl, repl::OpTypeEnum::kInsert, 10);
    auto userEntry = test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 50);
    const int64_t expectedOplogBytes =
        fastCountEntry.getEntry().toBSON().objsize() + userEntry.getEntry().toBSON().objsize();
    const auto result = runAccumulator({.isCheckpoint = true, .oplogUuid = oplogUuid},
                                       {std::move(fastCountEntry), std::move(userEntry)});
    ASSERT_TRUE(result.deltas.contains(oplogUuid));
    EXPECT_EQ(result.deltas.at(oplogUuid).sizeCount.size, expectedOplogBytes);
    EXPECT_EQ(result.deltas.at(oplogUuid).sizeCount.count, 2);
    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.lastTimestamp, ts2);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, NonCheckpoint_KeepsOplogUuidEvenWithNoUserEntries) {
    const Timestamp ts1{1, 1};
    auto entry = test_helpers::makeOplogEntry(ts1, fastCountColl, repl::OpTypeEnum::kInsert, 10);
    const int64_t expectedBytes = entry.getEntry().toBSON().objsize();
    const auto result =
        runAccumulator({.isCheckpoint = false, .oplogUuid = oplogUuid}, {std::move(entry)});
    ASSERT_TRUE(result.deltas.contains(oplogUuid));
    EXPECT_EQ(result.deltas.at(oplogUuid).sizeCount.size, expectedBytes);
    EXPECT_EQ(result.deltas.at(oplogUuid).sizeCount.count, 1);
    EXPECT_FALSE(result.lastTimestamp);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, PartialTxn_FollowedByNonApplyOps_ChainDiscarded) {
    // Verifies that a partial applyOps entry (partialTxn=true) that is interrupted by a
    // non-applyOps entry is implicitly aborted: the partial chain's accumulated deltas are
    // discarded and only the non-applyOps entry's delta appears in the result.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const auto result = runAccumulator(
        {},
        {makePartialApplyOpsEntry(ts1, collA, /*sizeDelta=*/100, repl::OpTime{}),
         test_helpers::makeOplogEntry(ts2, collB, repl::OpTypeEnum::kInsert, /*sizeDelta=*/70)});
    // collA's partial chain is discarded when the non-applyOps entry interrupts it.
    EXPECT_FALSE(result.deltas.contains(collA.uuid));
    // The regular insert for collB is counted normally.
    ASSERT_TRUE(result.deltas.contains(collB.uuid));
    EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount.size, 70);
    EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount.count, 1);
    EXPECT_EQ(result.lastTimestamp, ts2);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, PartialTxn_AtEndOfStream_Discarded) {
    // Verifies that a partial chain that reaches end-of-stream without a terminal applyOps
    // contributes no deltas (the transaction never committed within the scanned range).
    // lastTimestamp is still advanced because the partial entries were physically scanned.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const auto result =
        runAccumulator({},
                       {makePartialApplyOpsEntry(ts1, collA, /*sizeDelta=*/100, repl::OpTime{}),
                        makePartialApplyOpsEntry(ts2, collA, /*sizeDelta=*/200, opTimeAt(ts1))});
    EXPECT_FALSE(result.deltas.contains(collA.uuid));
    // lastTimestamp advances to the last scanned entry even though its delta is not yet visible.
    EXPECT_EQ(result.lastTimestamp, ts2);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
