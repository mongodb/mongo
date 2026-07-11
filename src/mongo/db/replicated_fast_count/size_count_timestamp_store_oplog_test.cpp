// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_timestamp_store_oplog.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/storage/ident.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo::replicated_fast_count {
namespace {

const std::string kTimestampsIdent{ident::kFastCountMetadataStoreTimestamps};
const std::string kMainStoreIdent{ident::kFastCountMetadataStore};

// Encodes the on-disk timestamp-store value '{valid-as-of: <ts>}' the way it is carried in a
// container op's 'v' field: a BinData whose bytes are that BSON object.
BSONObj makeValidAsOfValue(Timestamp validAsOf) {
    return BSON(std::string{kValidAsOfKey} << validAsOf);
}

// Builds a container op oplog entry (e.g. op = "ci" / "cu" / "cd") targeting 'container', carrying
// 'value' (already a BSONObj) in the BinData 'v' field of 'o'. Container update ('cu') ops also
// carry the '$v' update version field that real updates emit.
BSONObj makeContainerOp(std::string_view opType, std::string_view container, const BSONObj& value) {
    BSONObjBuilder o;
    o.append("k", int64_t{0});
    o.appendBinData("v", value.objsize(), BinDataType::BinDataGeneral, value.objdata());
    if (opType == "cu") {
        o.append("$v", int64_t{1});
    }
    BSONObjBuilder entry;
    entry.append("op", opType);
    entry.append("ns", "config.$container");
    entry.append("container", container);
    entry.append("o", o.obj());
    return entry.obj();
}

// Builds a container op whose 'v' BinData carries a timestamp-store '{valid-as-of: <ts>}' value.
BSONObj makeTimestampStoreOp(std::string_view opType,
                             std::string_view container,
                             Timestamp validAsOf) {
    return makeContainerOp(opType, container, makeValidAsOfValue(validAsOf));
}

// Wraps a bare operation document into a full top-level oplog entry by adding the 'ts' and 'wall'
// fields the oplog entry IDL parser requires.
BSONObj makeOplogEntry(const BSONObj& op) {
    BSONObjBuilder entry;
    entry.appendElements(op);
    entry.append("ts", Timestamp(100, 1));
    entry.append("wall", Date_t::fromMillisSinceEpoch(1000));
    return entry.obj();
}

// Builds an applyOps command oplog entry wrapping the provided inner operations.
BSONObj makeApplyOps(std::vector<BSONObj> innerOps) {
    BSONArrayBuilder arr;
    for (const auto& op : innerOps) {
        arr.append(op);
    }
    return makeOplogEntry(BSON("op" << "c"
                                    << "ns"
                                    << "admin.$cmd"
                                    << "o" << BSON("applyOps" << arr.arr())));
}

TEST(SizeCountTimestampStoreOplog, TopLevelContainerInsertReturnsValidAsOf) {
    auto entry = makeOplogEntry(makeTimestampStoreOp("ci", kTimestampsIdent, Timestamp(5, 2)));
    ASSERT_EQ(getTimestampStoreValidAsOfFromOplogEntry(entry), Timestamp(5, 2));
}

TEST(SizeCountTimestampStoreOplog, TopLevelContainerUpdateReturnsValidAsOf) {
    auto entry = makeOplogEntry(makeTimestampStoreOp("cu", kTimestampsIdent, Timestamp(7, 1)));
    ASSERT_EQ(getTimestampStoreValidAsOfFromOplogEntry(entry), Timestamp(7, 1));
}

TEST(SizeCountTimestampStoreOplog, NestedContainerUpdateInApplyOpsReturnsValidAsOf) {
    auto entry = makeApplyOps({makeTimestampStoreOp("cu", kTimestampsIdent, Timestamp(11, 4))});
    ASSERT_EQ(getTimestampStoreValidAsOfFromOplogEntry(entry), Timestamp(11, 4));
}

TEST(SizeCountTimestampStoreOplog, NestedContainerInsertAmongOtherOpsReturnsValidAsOf) {
    auto crud = BSON("op" << "i"
                          << "ns"
                          << "test.coll"
                          << "o" << BSON("_id" << 1));
    auto otherContainer = makeTimestampStoreOp("ci", kMainStoreIdent, Timestamp(2, 2));
    auto target = makeTimestampStoreOp("ci", kTimestampsIdent, Timestamp(13, 6));
    auto entry = makeApplyOps({crud, otherContainer, target});
    ASSERT_EQ(getTimestampStoreValidAsOfFromOplogEntry(entry), Timestamp(13, 6));
}

// A container delete to the timestamp store is malformed (deletes carry no value) and fails to
// parse as a container op.
DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest, ContainerDeleteTasserts, "12984806") {
    auto entry = makeOplogEntry(makeContainerOp("cd", kTimestampsIdent, BSONObj()));
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest,
                 DifferentContainerIdentTasserts,
                 "12984804") {
    // A write to the main metadata store (not the timestamps store) must not match.
    auto entry = makeOplogEntry(makeTimestampStoreOp("cu", kMainStoreIdent, Timestamp(9, 9)));
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest, NonContainerCrudOpTasserts, "12984804") {
    auto entry = makeOplogEntry(BSON("op" << "i"
                                          << "ns"
                                          << "test.coll"
                                          << "o" << BSON("_id" << 1)));
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest, NonApplyOpsCommandTasserts, "12984804") {
    auto entry = makeOplogEntry(BSON("op" << "c"
                                          << "ns"
                                          << "test.$cmd"
                                          << "o" << BSON("create" << "coll")));
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest,
                 ApplyOpsWithNoTimestampStoreWriteTasserts,
                 "12984802") {
    auto crud = BSON("op" << "i"
                          << "ns"
                          << "test.coll"
                          << "o" << BSON("_id" << 1));
    auto otherContainer = makeTimestampStoreOp("cu", kMainStoreIdent, Timestamp(3, 3));
    auto entry = makeApplyOps({crud, otherContainer});
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest,
                 ApplyOpsWithMultipleTimestampStoreWritesTasserts,
                 "12984803") {
    // More than one timestamp store write in a single applyOps batch should not be possible.
    auto first = makeTimestampStoreOp("cu", kTimestampsIdent, Timestamp(20, 1));
    auto second = makeTimestampStoreOp("cu", kTimestampsIdent, Timestamp(21, 1));
    auto entry = makeApplyOps({first, second});
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest, MissingContainerFieldTasserts, "12984806") {
    BSONObjBuilder o;
    o.append("k", int64_t{0});
    auto value = makeValidAsOfValue(Timestamp(4, 4));
    o.appendBinData("v", value.objsize(), BinDataType::BinDataGeneral, value.objdata());
    auto entry = makeOplogEntry(BSON("op" << "cu"
                                          << "ns"
                                          << "config.$container"
                                          << "o" << o.obj()));
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest, MissingObjectFieldTasserts, "12984806") {
    auto entry = makeOplogEntry(BSON("op" << "cu"
                                          << "ns"
                                          << "config.$container"
                                          << "container" << kTimestampsIdent));
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest, ValueMissingValidAsOfTasserts, "12984805") {
    auto entry =
        makeOplogEntry(makeContainerOp("cu", kTimestampsIdent, BSON("some-other-field" << 1)));
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest, NonBinDataValueTasserts, "12984806") {
    BSONObjBuilder o;
    o.append("k", int64_t{0});
    o.append("v", BSON(std::string{kValidAsOfKey} << Timestamp(8, 8)));
    auto entry = makeOplogEntry(BSON("op" << "cu"
                                          << "ns"
                                          << "config.$container"
                                          << "container" << kTimestampsIdent << "o" << o.obj()));
    getTimestampStoreValidAsOfFromOplogEntry(entry);
}

DEATH_TEST_REGEX(SizeCountTimestampStoreOplogDeathTest, MissingOpFieldTasserts, "12984806") {
    getTimestampStoreValidAsOfFromOplogEntry(BSONObj());
}

// Runs 'filter' (a find query predicate) against 'doc' through the real match expression engine,
// the same way the server evaluates the filter when the initial-sync fetcher issues the scan.
bool filterMatches(const BSONObj& filter, const BSONObj& doc) {
    auto expCtx = ExpressionContextBuilder{}
                      .ns(NamespaceString::createNamespaceString_forTest("local.oplog.rs"))
                      .build();
    Matcher matcher(filter, std::move(expCtx));
    return exec::matcher::matches(&matcher, doc);
}

TEST(SizeCountTimestampStoreOplog, ValidAsOfScanFilterMatchesTopLevelContainerOps) {
    const auto filter = fastCountValidAsOfScanFilter();
    ASSERT_TRUE(
        filterMatches(filter, makeTimestampStoreOp("ci", kTimestampsIdent, Timestamp(5, 1))));
    ASSERT_TRUE(
        filterMatches(filter, makeTimestampStoreOp("cu", kTimestampsIdent, Timestamp(5, 1))));
}

TEST(SizeCountTimestampStoreOplog, ValidAsOfScanFilterMatchesContainerOpNestedInApplyOps) {
    // The 'o.applyOps.container' dotted path must reach into the applyOps array.
    const auto filter = fastCountValidAsOfScanFilter();
    auto crud = BSON("op" << "i"
                          << "ns"
                          << "test.coll"
                          << "o" << BSON("_id" << 1));
    auto nested =
        makeApplyOps({crud, makeTimestampStoreOp("cu", kTimestampsIdent, Timestamp(9, 2))});
    ASSERT_TRUE(filterMatches(filter, nested));
}

TEST(SizeCountTimestampStoreOplog, ValidAsOfScanFilterDoesNotMatchOtherIdent) {
    const auto filter = fastCountValidAsOfScanFilter();
    // A write to the main metadata store ident (not the timestamps store) must not match, whether
    // top-level or nested in an applyOps.
    ASSERT_FALSE(
        filterMatches(filter, makeTimestampStoreOp("cu", kMainStoreIdent, Timestamp(5, 1))));
    auto nested = makeApplyOps({makeTimestampStoreOp("ci", kMainStoreIdent, Timestamp(5, 1))});
    ASSERT_FALSE(filterMatches(filter, nested));
}

TEST(SizeCountTimestampStoreOplog, ValidAsOfScanFilterDoesNotMatchUnrelatedEntries) {
    const auto filter = fastCountValidAsOfScanFilter();
    auto crud = BSON("op" << "i"
                          << "ns"
                          << "test.coll"
                          << "o" << BSON("_id" << 1));
    ASSERT_FALSE(filterMatches(filter, crud));
    ASSERT_FALSE(filterMatches(filter, makeApplyOps({crud})));
}

}  // namespace
}  // namespace mongo::replicated_fast_count
