/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <fmt/format.h>
#include <memory>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_base_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/matcher.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {
namespace {

const OpTime entryOpTime{Timestamp(3, 4), 5};
const NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo", "bar");
const int docId = 17;

TEST(OplogEntryTest, Update) {
    const BSONObj doc = BSON("_id" << docId);
    const BSONObj update = BSON("$set" << BSON("a" << 4));
    const auto entry = makeUpdateDocumentOplogEntry(entryOpTime, nss, doc, update);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT(entry.isCrudOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), doc);
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), update);
    ASSERT_BSONOBJ_EQ(entry.getObjectContainingDocumentKey(), doc);
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST(OplogEntryTest, Insert) {
    const BSONObj doc = BSON("_id" << docId << "a" << 5);
    const auto entry = makeInsertDocumentOplogEntry(entryOpTime, nss, doc);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT(entry.isCrudOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), BSON("_id" << docId));
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), doc);
    ASSERT_BSONOBJ_EQ(entry.getObjectContainingDocumentKey(), doc);
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST(OplogEntryTest, Delete) {
    const BSONObj doc = BSON("_id" << docId);
    const auto entry = makeDeleteDocumentOplogEntry(entryOpTime, nss, doc);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT(entry.isCrudOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), doc);
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), doc);
    ASSERT_BSONOBJ_EQ(entry.getObjectContainingDocumentKey(), doc);
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST(OplogEntryTest, Create) {
    CollectionOptions opts;
    opts.capped = true;
    opts.cappedSize = 15;

    const auto entry = makeCreateCollectionOplogEntry(entryOpTime, nss, opts.toBSON());

    ASSERT(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT_FALSE(entry.isCrudOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(),
                      BSON("create" << nss.coll() << "capped" << true << "size" << 15));
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kCreate);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST(OplogEntryTest, ApplyOpsNotInSession) {
    UUID uuid(UUID::gen());
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss.ns_forTest() << "ui" << uuid
                                                        << "o" << BSON("_id" << 1)))));
    auto applyOpsEntry = unittest::assertGet(OplogEntry::parse(applyOpsBson));
    ASSERT_TRUE(applyOpsEntry.isCommand());
    ASSERT_FALSE(applyOpsEntry.isInTransaction());
    ASSERT_TRUE(applyOpsEntry.isTerminalApplyOps());
    ASSERT_FALSE(applyOpsEntry.isSingleOplogEntryTransaction());
    ASSERT_FALSE(applyOpsEntry.isPartialTransaction());
    ASSERT_FALSE(applyOpsEntry.isEndOfLargeTransaction());
    ASSERT_FALSE(applyOpsEntry.applyOpsIsLinkedTransactionally());
}

TEST(OplogEntryTest, ApplyOpsSingleEntryTransaction) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss.ns_forTest() << "ui" << uuid
                                                        << "o" << BSON("_id" << 1))))
                  << "lsid" << sessionId.toBSON() << "txnNumber" << TxnNumber(5) << "stmtId"
                  << StmtId(0) << "prevOpTime" << OpTime());
    auto applyOpsEntry = unittest::assertGet(OplogEntry::parse(applyOpsBson));
    ASSERT_TRUE(applyOpsEntry.isCommand());
    ASSERT_TRUE(applyOpsEntry.isInTransaction());
    ASSERT_TRUE(applyOpsEntry.isTerminalApplyOps());
    ASSERT_TRUE(applyOpsEntry.isSingleOplogEntryTransaction());
    ASSERT_FALSE(applyOpsEntry.isPartialTransaction());
    ASSERT_FALSE(applyOpsEntry.isEndOfLargeTransaction());
    ASSERT_TRUE(applyOpsEntry.applyOpsIsLinkedTransactionally());
}

TEST(OplogEntryTest, ApplyOpsStartMultiEntryTransaction) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss.ns_forTest() << "ui" << uuid
                                                        << "o" << BSON("_id" << 1)))
                                     << "partialTxn" << true)
                  << "lsid" << sessionId.toBSON() << "txnNumber" << TxnNumber(5) << "stmtId"
                  << StmtId(0) << "prevOpTime" << OpTime());
    auto applyOpsEntry = unittest::assertGet(OplogEntry::parse(applyOpsBson));
    ASSERT_TRUE(applyOpsEntry.isCommand());
    ASSERT_TRUE(applyOpsEntry.isInTransaction());
    ASSERT_FALSE(applyOpsEntry.isTerminalApplyOps());
    ASSERT_FALSE(applyOpsEntry.isSingleOplogEntryTransaction());
    ASSERT_TRUE(applyOpsEntry.isPartialTransaction());
    ASSERT_FALSE(applyOpsEntry.isEndOfLargeTransaction());
    ASSERT_TRUE(applyOpsEntry.applyOpsIsLinkedTransactionally());
}

TEST(OplogEntryTest, ApplyOpsMiddleMultiEntryTransaction) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 2) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss.ns_forTest() << "ui" << uuid
                                                        << "o" << BSON("_id" << 1)))
                                     << "partialTxn" << true)
                  << "lsid" << sessionId.toBSON() << "txnNumber" << TxnNumber(5) << "stmtId"
                  << StmtId(0) << "prevOpTime" << OpTime(Timestamp(1, 1), 1));
    auto applyOpsEntry = unittest::assertGet(OplogEntry::parse(applyOpsBson));
    ASSERT_TRUE(applyOpsEntry.isCommand());
    ASSERT_TRUE(applyOpsEntry.isInTransaction());
    ASSERT_FALSE(applyOpsEntry.isTerminalApplyOps());
    ASSERT_FALSE(applyOpsEntry.isSingleOplogEntryTransaction());
    ASSERT_TRUE(applyOpsEntry.isPartialTransaction());
    ASSERT_FALSE(applyOpsEntry.isEndOfLargeTransaction());
    ASSERT_TRUE(applyOpsEntry.applyOpsIsLinkedTransactionally());
}

TEST(OplogEntryTest, ApplyOpsEndMultiEntryTransaction) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 2) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss.ns_forTest() << "ui" << uuid
                                                        << "o" << BSON("_id" << 1))))
                  << "lsid" << sessionId.toBSON() << "txnNumber" << TxnNumber(5) << "stmtId"
                  << StmtId(0) << "prevOpTime" << OpTime(Timestamp(1, 1), 1));
    auto applyOpsEntry = unittest::assertGet(OplogEntry::parse(applyOpsBson));
    ASSERT_TRUE(applyOpsEntry.isInTransaction());
    ASSERT_TRUE(applyOpsEntry.isCommand());
    ASSERT_TRUE(applyOpsEntry.isTerminalApplyOps());
    ASSERT_FALSE(applyOpsEntry.isSingleOplogEntryTransaction());
    ASSERT_FALSE(applyOpsEntry.isPartialTransaction());
    ASSERT_TRUE(applyOpsEntry.isEndOfLargeTransaction());
    ASSERT_TRUE(applyOpsEntry.applyOpsIsLinkedTransactionally());
}

TEST(OplogEntryTest, ApplyOpsFirstOrOnlyRetryableWrite) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss.ns_forTest() << "ui" << uuid
                                                        << "o" << BSON("_id" << 1))))
                  << "lsid" << sessionId.toBSON() << "txnNumber" << TxnNumber(5) << "stmtId"
                  << StmtId(0) << "prevOpTime" << OpTime() << "multiOpType" << 1);
    auto applyOpsEntry = unittest::assertGet(OplogEntry::parse(applyOpsBson));
    ASSERT_FALSE(applyOpsEntry.isInTransaction());
    ASSERT_TRUE(applyOpsEntry.isCommand());
    ASSERT_TRUE(applyOpsEntry.isTerminalApplyOps());
    ASSERT_FALSE(applyOpsEntry.isSingleOplogEntryTransaction());
    ASSERT_FALSE(applyOpsEntry.isPartialTransaction());
    ASSERT_FALSE(applyOpsEntry.isEndOfLargeTransaction());
    ASSERT_FALSE(applyOpsEntry.applyOpsIsLinkedTransactionally());
}

TEST(OplogEntryTest, ApplyOpsSubsequentRetryableWrite) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 2) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "i"
                                                        << "ns" << nss.ns_forTest() << "ui" << uuid
                                                        << "o" << BSON("_id" << 1))))
                  << "lsid" << sessionId.toBSON() << "txnNumber" << TxnNumber(5) << "stmtId"
                  << StmtId(0) << "prevOpTime" << OpTime(Timestamp(1, 1), 1) << "multiOpType" << 1);
    auto applyOpsEntry = unittest::assertGet(OplogEntry::parse(applyOpsBson));
    ASSERT_TRUE(applyOpsEntry.isCommand());
    ASSERT_FALSE(applyOpsEntry.isInTransaction());
    // All retryable-write applyOps are "terminal", because that just means they can be applied
    // when we get them; we don't have to wait for a later oplog entry.
    ASSERT_TRUE(applyOpsEntry.isTerminalApplyOps());
    ASSERT_FALSE(applyOpsEntry.isSingleOplogEntryTransaction());
    ASSERT_FALSE(applyOpsEntry.isPartialTransaction());
    ASSERT_FALSE(applyOpsEntry.isEndOfLargeTransaction());
    ASSERT_FALSE(applyOpsEntry.applyOpsIsLinkedTransactionally());
}

TEST(OplogEntryTest, OpTimeBaseNonStrictParsing) {
    const BSONObj oplogEntryExtraField = BSON("ts" << Timestamp(0, 0) << "t" << 0LL << "op"
                                                   << "c"
                                                   << "ns" << nss.ns_forTest() << "wall" << Date_t()
                                                   << "o" << BSON("_id" << 1) << "extraField" << 3);

    // OpTimeBase should be successfully created from an OplogEntry, even though it has
    // extraneous fields.
    UNIT_TEST_INTERNALS_IGNORE_UNUSED_RESULT_WARNINGS(
        OpTimeBase::parse(IDLParserContext("OpTimeBase"), oplogEntryExtraField));

    // OplogEntryBase should still use strict parsing and throw an error when it has extraneous
    // fields.
    ASSERT_THROWS_CODE(
        OplogEntryBase::parse(IDLParserContext("OplogEntryBase"), oplogEntryExtraField),
        AssertionException,
        40415);

    const BSONObj oplogEntryMissingTimestamp =
        BSON("t" << 0LL << "op"
                 << "c"
                 << "ns" << nss.ns_forTest() << "wall" << Date_t() << "o" << BSON("_id" << 1));

    // When an OplogEntryBase is created with a missing required field in a chained struct, it
    // should throw an exception.
    ASSERT_THROWS_CODE(
        OplogEntryBase::parse(IDLParserContext("OplogEntryBase"), oplogEntryMissingTimestamp),
        AssertionException,
        ErrorCodes::IDLFailedToParse);
}

TEST(OplogEntryTest, InsertIncludesTidField) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    const BSONObj doc = BSON("_id" << docId << "a" << 5);
    TenantId tid(OID::gen());
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "foo", "bar");
    const auto entry =
        makeOplogEntry(entryOpTime, OpTypeEnum::kInsert, nss, doc, boost::none, {}, Date_t::now());

    ASSERT(entry.getTid());
    ASSERT_EQ(*entry.getTid(), tid);
    ASSERT_EQ(entry.getNss(), nss);
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), BSON("_id" << docId));
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), doc);
}

TEST(OplogEntryTest, ParseMutableOplogEntryIncludesTidField) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    const TenantId tid(OID::gen());

    const NamespaceString nssWithTid =
        NamespaceString::createNamespaceString_forTest(tid, nss.ns_forTest());

    const BSONObj oplogBson = [&] {
        BSONObjBuilder bob;
        bob.append("ts", Timestamp(0, 0));
        bob.append("t", 0LL);
        bob.append("op", "c");
        tid.serializeToBSON("tid", &bob);
        bob.append("ns", nssWithTid.ns_forTest());
        bob.append("wall", Date_t());
        BSONObjBuilder{bob.subobjStart("o")}.append("_id", 1);
        return bob.obj();
    }();

    auto oplogEntry = unittest::assertGet(MutableOplogEntry::parse(oplogBson));
    ASSERT(oplogEntry.getTid());
    ASSERT_EQ(oplogEntry.getTid(), tid);
    ASSERT_EQ(oplogEntry.getNss(), nssWithTid);
}

TEST(OplogEntryTest, ParseDurableOplogEntryIncludesTidField) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    const TenantId tid(OID::gen());
    const NamespaceString nssWithTid =
        NamespaceString::createNamespaceString_forTest(tid, nss.ns_forTest());

    const BSONObj oplogBson = [&] {
        BSONObjBuilder bob;
        bob.append("ts", Timestamp(0, 0));
        bob.append("t", 0LL);
        bob.append("op", "i");
        tid.serializeToBSON("tid", &bob);
        bob.append("ns", nssWithTid.ns_forTest());
        bob.append("wall", Date_t());
        BSONObjBuilder{bob.subobjStart("o")}.append("_id", 1).append("data", "x");
        BSONObjBuilder{bob.subobjStart("o2")}.append("_id", 1);
        return bob.obj();
    }();

    auto oplogEntry = unittest::assertGet(DurableOplogEntry::parse(oplogBson));
    ASSERT(oplogEntry.getTid());
    ASSERT_EQ(oplogEntry.getTid(), tid);
    ASSERT_EQ(oplogEntry.getNss(), nssWithTid);
}

TEST(OplogEntryTest, ParseReplOperationIncludesTidField) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    UUID uuid(UUID::gen());
    TenantId tid(OID::gen());
    NamespaceString nssWithTid =
        NamespaceString::createNamespaceString_forTest(tid, nss.ns_forTest());

    auto op = repl::DurableOplogEntry::makeInsertOperation(
        nssWithTid,
        uuid,
        BSONObjBuilder{}.append("_id", 1).append("data", "x").obj(),
        BSONObjBuilder{}.append("_id", 1).obj());
    BSONObj oplogBson = op.toBSON();

    const auto vts = auth::ValidatedTenancyScopeFactory::create(
        tid,
        auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TenantForTestingTag{});
    auto replOp = ReplOperation::parse(
        IDLParserContext("ReplOperation", false, vts, tid, SerializationContext::stateDefault()),
        oplogBson);
    ASSERT(replOp.getTid());
    ASSERT_EQ(replOp.getTid(), tid);
    ASSERT_EQ(replOp.getNss(), nssWithTid);
}

TEST(OplogEntryTest, ConvertMutableOplogEntryToReplOperation) {
    // Required by setTid to take effect
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    RAIIServerParameterControllerForTest multitenancySupportController("multitenancySupport", true);
    auto tid = TenantId(OID::gen());
    auto nssWithTid = NamespaceString::createNamespaceString_forTest(tid, nss.ns_forTest());
    auto opType = repl::OpTypeEnum::kCommand;
    auto uuid = UUID::gen();
    std::vector<StmtId> stmtIds{StmtId(0), StmtId(1), StmtId(2)};
    const auto doc = BSON("x" << 1);

    MutableOplogEntry entry;
    entry.setTid(tid);
    entry.setNss(nssWithTid);
    entry.setTimestamp(Timestamp(1, 1));    // only exists in OplogEntryBase
    entry.setWallClockTime(Date_t::now());  // only exists in OplogEntryBase
    entry.setTerm(1);                       // only exists in OplogEntryBase
    entry.setUuid(uuid);
    entry.setOpType(opType);
    entry.setObject(doc);
    entry.setStatementIds(stmtIds);

    auto replOp = entry.toReplOperation();

    ASSERT_EQ(replOp.getTid(), tid);
    ASSERT_EQ(replOp.getTid(), entry.getTid());
    ASSERT_EQ(replOp.getUuid(), uuid);
    ASSERT_EQ(replOp.getUuid(), entry.getUuid());
    ASSERT_EQ(replOp.getOpType(), opType);
    ASSERT_EQ(replOp.getOpType(), entry.getOpType());
    ASSERT_EQ(replOp.getNss(), nssWithTid);
    ASSERT_EQ(replOp.getNss(), entry.getNss());
    ASSERT_FALSE(replOp.getFromMigrate());
    ASSERT_EQ(replOp.getFromMigrate(), entry.getFromMigrate());
    ASSERT_BSONOBJ_EQ(replOp.getObject(), doc);
    ASSERT_BSONOBJ_EQ(replOp.getObject(), entry.getObject());
    ASSERT_EQ(replOp.getStatementIds().size(), stmtIds.size());
    ASSERT_EQ(replOp.getStatementIds().size(), entry.getStatementIds().size());
    ASSERT_THAT(replOp.getStatementIds(), unittest::match::Eq(stmtIds));
    ASSERT_THAT(replOp.getStatementIds(), unittest::match::Eq(entry.getStatementIds()));

    // While overwhelmingly set to false, a few sharding scenarios
    // set 'fromMigrate' to true. Therefore, testing it.
    entry.setFromMigrateIfTrue(true);
    auto replOp2 = entry.toReplOperation();
    ASSERT_EQ(replOp2.getFromMigrate(), entry.getFromMigrate());

    // Tests when 'checkExistenceForDiffInsert' is set to true.
    entry.setCheckExistenceForDiffInsert();
    auto replOp3 = entry.toReplOperation();
    ASSERT_EQ(replOp3.getCheckExistenceForDiffInsert(), entry.getCheckExistenceForDiffInsert());
}

TEST(OplogEntryTest, StatementIDParseAndSerialization) {
    UnorderedFieldsBSONObjComparator bsonCompare;
    const BSONObj oplogEntryWithNoStmtId = BSON("op"
                                                << "c"
                                                << "ns" << nss.ns_forTest() << "o"
                                                << BSON("_id" << 1) << "v" << 2 << "ts"
                                                << Timestamp(0, 0) << "t" << 0LL << "wall"
                                                << Date_t());

    auto oplogEntryBaseNoStmtId =
        OplogEntryBase::parse(IDLParserContext("OplogEntry"), oplogEntryWithNoStmtId);
    ASSERT_TRUE(oplogEntryBaseNoStmtId.getStatementIds().empty());
    auto rtOplogEntryWithNoStmtId = oplogEntryBaseNoStmtId.toBSON();
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithNoStmtId, rtOplogEntryWithNoStmtId), 0)
        << "Did not round trip: " << oplogEntryWithNoStmtId << " should be equal to "
        << rtOplogEntryWithNoStmtId;

    const BSONObj oplogEntryWithOneStmtId = BSON("op"
                                                 << "c"
                                                 << "ns" << nss.ns_forTest() << "o"
                                                 << BSON("_id" << 1) << "v" << 2 << "ts"
                                                 << Timestamp(0, 0) << "t" << 0LL << "wall"
                                                 << Date_t() << "stmtId" << 99);
    auto oplogEntryBaseOneStmtId =
        OplogEntryBase::parse(IDLParserContext("OplogEntry"), oplogEntryWithOneStmtId);
    ASSERT_EQ(oplogEntryBaseOneStmtId.getStatementIds(), std::vector<StmtId>{99});
    auto rtOplogEntryWithOneStmtId = oplogEntryBaseOneStmtId.toBSON();
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithOneStmtId, rtOplogEntryWithOneStmtId), 0)
        << "Did not round trip: " << oplogEntryWithOneStmtId << " should be equal to "
        << rtOplogEntryWithOneStmtId;
    // Statement id should be NumberInt, not NumberLong or some other numeric.
    ASSERT_EQ(rtOplogEntryWithOneStmtId["stmtId"].type(), NumberInt);

    const BSONObj oplogEntryWithMultiStmtId = BSON("op"
                                                   << "c"
                                                   << "ns" << nss.ns_forTest() << "o"
                                                   << BSON("_id" << 1) << "v" << 2 << "ts"
                                                   << Timestamp(0, 0) << "t" << 0LL << "wall"
                                                   << Date_t() << "stmtId"
                                                   << BSON_ARRAY(101 << 102 << 103));
    auto oplogEntryBaseMultiStmtId =
        OplogEntryBase::parse(IDLParserContext("OplogEntry"), oplogEntryWithMultiStmtId);
    ASSERT_EQ(oplogEntryBaseMultiStmtId.getStatementIds(), (std::vector<StmtId>{101, 102, 103}));
    auto rtOplogEntryWithMultiStmtId = oplogEntryBaseMultiStmtId.toBSON();
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithMultiStmtId, rtOplogEntryWithMultiStmtId), 0)
        << "Did not round trip: " << oplogEntryWithMultiStmtId << " should be equal to "
        << rtOplogEntryWithMultiStmtId;
    // Array entries should be NumberInt, not NumberLong or some other numeric.
    ASSERT_EQ(rtOplogEntryWithMultiStmtId["stmtId"]["0"].type(), NumberInt);

    // A non-canonical entry with an empty stmtId array.
    const BSONObj oplogEntryWithEmptyStmtId = BSON("op"
                                                   << "c"
                                                   << "ns" << nss.ns_forTest() << "o"
                                                   << BSON("_id" << 1) << "v" << 2 << "ts"
                                                   << Timestamp(0, 0) << "t" << 0LL << "wall"
                                                   << Date_t() << "stmtId" << BSONArray());

    auto oplogEntryBaseEmptyStmtId =
        OplogEntryBase::parse(IDLParserContext("OplogEntry"), oplogEntryWithEmptyStmtId);
    ASSERT_TRUE(oplogEntryBaseEmptyStmtId.getStatementIds().empty());
    auto rtOplogEntryWithEmptyStmtId = oplogEntryBaseEmptyStmtId.toBSON();
    // This round-trips to the canonical version with no statement ID.
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithNoStmtId, rtOplogEntryWithEmptyStmtId), 0)
        << "Did not round trip: " << oplogEntryWithNoStmtId << " should be equal to "
        << rtOplogEntryWithEmptyStmtId;

    // A non-canonical entry with a singleton stmtId array.
    const BSONObj oplogEntryWithSingletonStmtId = BSON("op"
                                                       << "c"
                                                       << "ns" << nss.ns_forTest() << "o"
                                                       << BSON("_id" << 1) << "v" << 2 << "ts"
                                                       << Timestamp(0, 0) << "t" << 0LL << "wall"
                                                       << Date_t() << "stmtId" << BSON_ARRAY(99));

    auto oplogEntryBaseSingletonStmtId =
        OplogEntryBase::parse(IDLParserContext("OplogEntry"), oplogEntryWithSingletonStmtId);
    ASSERT_EQ(oplogEntryBaseSingletonStmtId.getStatementIds(), std::vector<StmtId>{99});
    auto rtOplogEntryWithSingletonStmtId = oplogEntryBaseSingletonStmtId.toBSON();
    // This round-trips to the canonical version with a non-array statement ID.
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithOneStmtId, rtOplogEntryWithSingletonStmtId), 0)
        << "Did not round trip: " << oplogEntryWithNoStmtId << " should be equal to "
        << rtOplogEntryWithSingletonStmtId;
}

}  // namespace
}  // namespace repl
}  // namespace mongo
