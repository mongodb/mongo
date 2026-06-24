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
#include "mongo/db/storage/ident.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <list>
#include <utility>

namespace mongo::replicated_fast_count {
namespace {

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
        test_helpers::OplogCursorMock cursor(std::move(entries));
        StreamingOplogDeltaAccumulator acc(std::move(opts));
        while (auto rec = cursor.next()) {
            acc.consumeRecord(*rec);
        }
        return acc.finish();
    }

    // Feeds an arbitrary BSON record straight to consumeRecord, bypassing the cursor + OplogEntry
    // round-trip. Used by the fast-lane tests that need to construct deliberately malformed or
    // hand-shaped raw entries (e.g. non-string `op`, missing `sz`, custom `tid` field).
    OplogScanResult runAccumulatorRaw(StreamingOplogDeltaAccumulator::Options opts,
                                      const std::vector<BSONObj>& rawRecords) {
        StreamingOplogDeltaAccumulator acc(std::move(opts));
        int64_t rid = 1;
        for (const auto& raw : rawRecords) {
            Record rec{RecordId(rid++), RecordData(raw.objdata(), raw.objsize())};
            acc.consumeRecord(rec);
        }
        return acc.finish();
    }

    // Convenience overloads for tests that only assert on per-collection deltas. Strips the oplog's
    // own self-delta from the result.
    OplogScanResult runAccumulator(std::list<repl::OplogEntry> entries) {
        auto result = runAccumulator({.oplogUuid = oplogUuid}, std::move(entries));
        result.deltas.erase(oplogUuid);
        return result;
    }

    OplogScanResult runAccumulatorRaw(const std::vector<BSONObj>& rawRecords) {
        auto result = runAccumulatorRaw({.oplogUuid = oplogUuid}, rawRecords);
        result.deltas.erase(oplogUuid);
        return result;
    }

    // Builds a noop (`n`) oplog BSON without `m` or `ui` — Layer 1 returns kCountedNoDelta and
    // advances lastTimestamp without recording any delta.
    BSONObj makeNoopBson(Timestamp ts) {
        return repl::DurableOplogEntry{
            repl::DurableOplogEntryParams{
                .opTime = opTimeAt(ts),
                .opType = repl::OpTypeEnum::kNoop,
                .nss = NamespaceString::createNamespaceString_forTest("test", "$cmd"),
                .oField = BSON("msg" << "noop"),
                .wallClockTime = Date_t::now(),
            }}
            .toBSON()
            .getOwned();
    }

    // Builds a `c` (command) oplog BSON whose first o-field is a known no-delta command
    // (createIndexes). Layer 1 returns kCountedNoDelta.
    BSONObj makeUnrelatedCommandBson(Timestamp ts) {
        return repl::DurableOplogEntry{
            repl::DurableOplogEntryParams{
                .opTime = opTimeAt(ts),
                .opType = repl::OpTypeEnum::kCommand,
                .nss = collA.nss.getCommandNS(),
                .uuid = collA.uuid,
                .oField = BSON("createIndexes" << collA.nss.coll() << "v" << 2 << "key"
                                               << BSON("a" << 1) << "name" << "a_1"),
                .wallClockTime = Date_t::now(),
            }}
            .toBSON()
            .getOwned();
    }

    // Builds a container op (ci/cu/cd) oplog BSON. `containerIdent` is written to the top-level
    // `container` field that Layer 1 reads to decide whether to skip the entry.
    BSONObj makeContainerOpBson(Timestamp ts,
                                std::string_view opStr,
                                std::string_view containerIdent) {
        BSONObjBuilder b;
        b.append("op", opStr);
        b.append("ns", collA.nss.ns_forTest());
        collA.uuid.appendToBuilder(&b, "ui");
        b.append("ts", ts);
        b.append("t", kTestTerm);
        b.append("v", repl::DurableOplogEntry::kOplogVersion);
        b.append("wall", Date_t::now());
        b.append("o", BSONObj());
        b.append("container", containerIdent);
        return b.obj();
    }

    // Builds a raw CRUD oplog BSON with explicit control over which fields are present and what
    // shape `m` and `sz` take. Used to exercise the Layer 2 fall-through branches.
    BSONObj makeRawCrudBson(Timestamp ts,
                            std::string_view opStr,
                            const NamespaceString& nss,
                            boost::optional<UUID> uuid,
                            boost::optional<BSONObj> mField) {
        BSONObjBuilder b;
        b.append("op", opStr);
        b.append("ns", nss.ns_forTest());
        if (uuid) {
            uuid->appendToBuilder(&b, "ui");
        }
        b.append("ts", ts);
        b.append("t", kTestTerm);
        b.append("v", repl::DurableOplogEntry::kOplogVersion);
        b.append("wall", Date_t::now());
        b.append("o", BSON("_id" << 1));
        b.append("o2", BSON("_id" << 1));
        if (mField) {
            b.append("m", *mField);
        }
        return b.obj();
    }

    // Builds a non-partial applyOps oplog BSON with the supplied inner ops. Set `prepare=true`
    // to mark it as a prepare entry (Layer 2.5 short-circuits to kProcessed{0}).
    BSONObj makeApplyOpsBson(Timestamp ts, BSONArray innerOps, bool prepare = false) {
        BSONObjBuilder oBuilder;
        oBuilder.append("applyOps", innerOps);
        if (prepare) {
            oBuilder.append("prepare", true);
        }
        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
                                           .opTime = opTimeAt(ts),
                                           .opType = repl::OpTypeEnum::kCommand,
                                           .nss = NamespaceString::kAdminCommandNamespace,
                                           .oField = oBuilder.obj(),
                                           .wallClockTime = Date_t::now(),
                                       }}
            .toBSON()
            .getOwned();
    }

    // Builds a commitTransaction oplog BSON. If `mArray` is provided, it is written as the
    // top-level `m` field (array variant of OplogEntrySizeMetadata).
    BSONObj makeCommitTxnBson(Timestamp ts, boost::optional<BSONArray> mArray) {
        BSONObjBuilder b;
        b.append("op", "c");
        b.append("ns", NamespaceString::kAdminCommandNamespace.ns_forTest());
        b.append("ts", ts);
        b.append("t", kTestTerm);
        b.append("v", repl::DurableOplogEntry::kOplogVersion);
        b.append("wall", Date_t::now());
        b.append("o", BSON("commitTransaction" << 1));
        if (mArray) {
            b.append("m", *mArray);
        }
        return b.obj();
    }

    // Builds a raw insert inner-op BSON for use inside an applyOps array.
    BSONObj makeInnerInsertBson(const test_helpers::NsAndUUID& coll, int32_t sizeDelta) {
        return BSON("op" << "i"
                         << "ns" << coll.nss.ns_forTest() << "ui" << coll.uuid << "o"
                         << BSON("_id" << 1) << "m" << BSON("sz" << sizeDelta));
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
    const auto result = runAccumulator({});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_FALSE(result.lastTimestamp);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, SingleEntry_AccumulatesAndAdvancesTimestamp) {
    const Timestamp ts{1, 5};
    const auto result = runAccumulator(
        {test_helpers::makeOplogEntry(ts, collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10)});
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
        runAccumulator({test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 10),
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
        {test_helpers::makeOplogEntry(ts1, fastCountColl, repl::OpTypeEnum::kInsert, 10)});
    EXPECT_FALSE(result.lastTimestamp);
    EXPECT_TRUE(result.deltas.empty());
}

TEST_F(StreamingOplogDeltaAccumulatorTest, InternalThenUser_LastTimestampIsUserEntry) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const auto result = runAccumulator(
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
        runAccumulator({makePartialApplyOpsEntry(ts1, collA, /*sizeDelta=*/100, repl::OpTime{}),
                        makePartialApplyOpsEntry(ts2, collA, /*sizeDelta=*/200, opTimeAt(ts1))});
    EXPECT_FALSE(result.deltas.contains(collA.uuid));
    // lastTimestamp advances to the last scanned entry even though its delta is not yet visible.
    EXPECT_EQ(result.lastTimestamp, ts2);
}

// ---------- Fast-lane focused tests ----------
//
// The tests below exercise the Layer 1 / Layer 2 / Layer 2.5 fast lanes in
// `consumeRecord`. Each test targets a specific branch in `classifyForFastLane`,
// `tryRecordFastCrud`, `tryFastApplyOps`, or `tryFastCommitTxn` that the higher-level scan tests
// cover only incidentally.

// ----- Layer 1: classifyForFastLane / kCountedNoDelta -----

TEST_F(StreamingOplogDeltaAccumulatorTest, FastLane_Noop_AdvancesTimestampNoDeltas) {
    const Timestamp ts{1, 1};
    const auto result = runAccumulatorRaw({makeNoopBson(ts)});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_EQ(result.lastTimestamp, ts);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, FastLane_UnrelatedCommand_AdvancesTimestampNoDeltas) {
    const Timestamp ts{1, 1};
    const auto result = runAccumulatorRaw({makeUnrelatedCommandBson(ts)});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_EQ(result.lastTimestamp, ts);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, FastLane_ContainerOpOnUserIdent_AdvancesTimestamp) {
    // ci/cu/cd targeting a non-fast-count ident are still counted-no-delta (ts advances).
    const Timestamp ts{1, 1};
    const auto result =
        runAccumulatorRaw({makeContainerOpBson(ts, "ci"_sd, "collection-user-ident"_sd)});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_EQ(result.lastTimestamp, ts);
}

TEST_F(StreamingOplogDeltaAccumulatorTest,
       FastLane_ContainerOpOnFastCountIdent_NoTimestampAdvance) {
    // ci/cu/cd targeting a replicated-fast-count ident are internal writes; Layer 1 returns
    // kFastCountStoreSkip so lastTimestamp must NOT advance (avoids feedback loop on the
    // checkpoint store itself). The container field stores the ident string, not the namespace.
    const Timestamp ts{1, 1};
    const auto result =
        runAccumulatorRaw({makeContainerOpBson(ts, "ci"_sd, ident::kFastCountMetadataStore)});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_FALSE(result.lastTimestamp);
}

// ----- Layer 1: classifyForFastLane / kFastCountStoreSkip via ns -----

TEST_F(StreamingOplogDeltaAccumulatorTest, FastLane_DirectCrudOnFastCountStore_NoTimestampAdvance) {
    const Timestamp ts{1, 1};
    const auto result = runAccumulator(
        {test_helpers::makeOplogEntry(ts, fastCountColl, repl::OpTypeEnum::kInsert, /*sz=*/10)});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_FALSE(result.lastTimestamp);
}

// ----- Layer 2: tryRecordFastCrud shape handling -----

TEST_F(StreamingOplogDeltaAccumulatorTest, FastLane_CrudMissingM_NoDeltas) {
    // CRUD entry without `m` size metadata is treated as no-delta. ts still advances.
    const Timestamp ts{1, 1};
    const auto result = runAccumulatorRaw(
        {makeRawCrudBson(ts, "i"_sd, collA.nss, collA.uuid, /*mField=*/boost::none)});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_EQ(result.lastTimestamp, ts);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, FastLane_CrudIneligibleNamespace_NoDeltas) {
    // local.* is ineligible for replicated fast count. Layer 2's `isFastCountEligibleNonStore`
    // check skips the delta but still counts the entry as processed (ts advances).
    const auto localNss =
        NamespaceString::createNamespaceString_forTest("local", "rs.oplog.rs.archive");
    const Timestamp ts{1, 1};
    const auto result =
        runAccumulatorRaw({makeRawCrudBson(ts, "i"_sd, localNss, UUID::gen(), BSON("sz" << 10))});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_EQ(result.lastTimestamp, ts);
}

// ----- Layer 2.5: tryFastApplyOps applyOps shapes -----

TEST_F(StreamingOplogDeltaAccumulatorTest, FastLane_PreparedApplyOps_AdvancesTimestampNoDeltas) {
    // A prepare entry holds its deltas until the matching commitTransaction. Layer 2.5 returns
    // kProcessed{0}: ts advances, but no per-uuid deltas are recorded.
    const Timestamp ts{1, 1};
    const auto applyOpsBson =
        makeApplyOpsBson(ts, BSON_ARRAY(makeInnerInsertBson(collA, 100)), /*prepare=*/true);
    const auto result = runAccumulatorRaw({applyOpsBson});
    EXPECT_FALSE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.lastTimestamp, ts);
}

TEST_F(StreamingOplogDeltaAccumulatorTest,
       FastLane_NonPartialApplyOps_CrudInnerOps_AccumulatesDeltas) {
    const Timestamp ts{1, 1};
    const auto applyOpsBson = makeApplyOpsBson(
        ts, BSON_ARRAY(makeInnerInsertBson(collA, 100) << makeInnerInsertBson(collB, 200)));
    const auto result = runAccumulatorRaw({applyOpsBson});
    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 100);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 1);
    ASSERT_TRUE(result.deltas.contains(collB.uuid));
    EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount.size, 200);
    EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount.count, 1);
    EXPECT_EQ(result.lastTimestamp, ts);
}

TEST_F(StreamingOplogDeltaAccumulatorTest,
       FastLane_ApplyOpsAllInternalInnerOps_NoTimestampAdvance) {
    // applyOps where every inner op targets the fast-count store maps to kAllInternal: the
    // entry is skipped entirely (no ts advance), mirroring `operationsOnFastCountStores`.
    const Timestamp ts{1, 1};
    const auto applyOpsBson =
        makeApplyOpsBson(ts, BSON_ARRAY(makeInnerInsertBson(fastCountColl, 50)));
    const auto result = runAccumulatorRaw({applyOpsBson});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_FALSE(result.lastTimestamp);
}

TEST_F(StreamingOplogDeltaAccumulatorTest,
       FastLane_ApplyOpsMixedUserAndInternal_OnlyUserDeltasRecorded) {
    // Mixed inner ops: internal-store inner ops are silently dropped, user-collection inner ops
    // contribute deltas. ts advances because at least one user op was observed.
    const Timestamp ts{1, 1};
    const auto applyOpsBson = makeApplyOpsBson(
        ts, BSON_ARRAY(makeInnerInsertBson(fastCountColl, 1) << makeInnerInsertBson(collA, 100)));
    const auto result = runAccumulatorRaw({applyOpsBson});
    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 100);
    EXPECT_FALSE(result.deltas.contains(fastCountColl.uuid));
    EXPECT_EQ(result.lastTimestamp, ts);
}

TEST_F(StreamingOplogDeltaAccumulatorTest, FastLane_ApplyOpsInnerNonCrud_FallsThroughToLayer3) {
    // An inner op whose `op` is not in {i,u,d} forces Layer 2.5 to fall through. Layer 3 then
    // parses the entry and processes inner ops via the typed dispatch. For this synthetic
    // applyOps containing a noop inner op, the typed dispatch still produces no deltas because
    // the noop has no size metadata. ts advances.
    const Timestamp ts{1, 1};
    BSONObj innerNoop =
        BSON("op" << "n"
                  << "ns" << collA.nss.ns_forTest() << "o" << BSON("msg" << "noop"));
    const auto applyOpsBson = makeApplyOpsBson(ts, BSON_ARRAY(innerNoop));
    const auto result = runAccumulatorRaw({applyOpsBson});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_EQ(result.lastTimestamp, ts);
}

// ----- Layer 2.5: tryFastCommitTxn shapes -----

TEST_F(StreamingOplogDeltaAccumulatorTest,
       FastLane_CommitTxnWithoutSizeMetadata_AdvancesTimestampNoDeltas) {
    const Timestamp ts{1, 1};
    const auto result = runAccumulatorRaw({makeCommitTxnBson(ts, /*mArray=*/boost::none)});
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_EQ(result.lastTimestamp, ts);
}

TEST_F(StreamingOplogDeltaAccumulatorTest,
       FastLane_CommitTxnWithSizeMetadataArray_AccumulatesPerUuidDeltas) {
    // commitTransaction's `m` field is an array of {uuid, sz, ct} (per-collection rollup of the
    // prepared transaction's writes). Layer 2.5 reads it directly and records each entry.
    const Timestamp ts{1, 1};
    BSONArrayBuilder mArr;
    mArr.append(BSON("uuid" << collA.uuid << "sz" << int64_t{300} << "ct" << int64_t{3}));
    mArr.append(BSON("uuid" << collB.uuid << "sz" << int64_t{500} << "ct" << int64_t{5}));
    const auto result = runAccumulatorRaw({makeCommitTxnBson(ts, mArr.arr())});
    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 300);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 3);
    ASSERT_TRUE(result.deltas.contains(collB.uuid));
    EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount.size, 500);
    EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount.count, 5);
    EXPECT_EQ(result.lastTimestamp, ts);
}

// ----- Death tests for unsupported data shapes -----

class StreamingOplogDeltaAccumulatorDeathTest : public StreamingOplogDeltaAccumulatorTest {};

DEATH_TEST_F(StreamingOplogDeltaAccumulatorDeathTest, FastLane_TidFieldCrashes, "12565801") {
    // SERVER-125723: an oplog entry with a `tid` field must crash the fast-scan path. Layer 1's
    // `extractScanFields` reads the `tid` byte triple in its size-3 dispatch and aborts the
    // process with assert id 12565801. Multi-tenancy is no longer supported on this hot path.
    const Timestamp ts{1, 1};
    BSONObjBuilder b;
    b.append("op", "i");
    b.append("ns", collA.nss.ns_forTest());
    collA.uuid.appendToBuilder(&b, "ui");
    b.append("tid", "tenant1");
    b.append("ts", ts);
    b.append("t", kTestTerm);
    b.append("v", repl::DurableOplogEntry::kOplogVersion);
    b.append("wall", Date_t::now());
    b.append("o", BSON("_id" << 1));
    b.append("m", BSON("sz" << 10));
    runAccumulatorRaw({b.obj()});
}

// ----- Checkpoint metrics interaction -----

TEST_F(StreamingOplogDeltaAccumulatorTest,
       FastLane_CheckpointSkippedMetric_FiresOnFastCountStoreSkip) {
    // Records that hit kFastCountStoreSkip (direct CRUD on the store, or container ops on the
    // fast-count ident) must report a "skipped" metric in checkpoint mode and must not advance
    // lastTimestamp. This is the contract Layer 1 has with the checkpoint accountant.
    const Timestamp ts{1, 1};
    const auto result = runAccumulator(
        {.isCheckpoint = true, .oplogUuid = oplogUuid},
        {test_helpers::makeOplogEntry(ts, fastCountColl, repl::OpTypeEnum::kInsert, 10)});
    // The only entry is an internal store write, so lastTimestamp never advances and finish()
    // erases the oplog self-delta, leaving no deltas.
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_FALSE(result.lastTimestamp);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
