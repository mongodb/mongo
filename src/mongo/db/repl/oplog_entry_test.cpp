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

#include "mongo/db/repl/oplog_entry.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/index_builds/index_build_oplog_entry.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/create_oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_base_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <memory>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace repl {
namespace {

class OplogEntryTest : public ServiceContextMongoDTest {
protected:
    void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();
        _opCtx = cc().makeOperationContext();
    }

protected:
    const OpTime entryOpTime{Timestamp(3, 4), 5};
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const int docId{17};
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(OplogEntryTest, Update) {
    const BSONObj doc = BSON("_id" << docId);
    const BSONObj update = BSON("$set" << BSON("a" << 4));
    const auto entry = makeUpdateDocumentOplogEntry(entryOpTime, nss, doc, update);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT(entry.isCrudOpType());
    ASSERT_FALSE(entry.isContainerOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), doc);
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), update);
    ASSERT_BSONOBJ_EQ(entry.getObjectContainingDocumentKey(), doc);
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST_F(OplogEntryTest, Insert) {
    const BSONObj doc = BSON("_id" << docId << "a" << 5);
    const auto entry = makeInsertDocumentOplogEntry(entryOpTime, nss, doc);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT(entry.isCrudOpType());
    ASSERT_FALSE(entry.isContainerOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), BSON("_id" << docId));
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), doc);
    ASSERT_BSONOBJ_EQ(entry.getObjectContainingDocumentKey(), doc);
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST_F(OplogEntryTest, Delete) {
    const BSONObj doc = BSON("_id" << docId);
    const auto entry = makeDeleteDocumentOplogEntry(entryOpTime, nss, doc);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT(entry.isCrudOpType());
    ASSERT_FALSE(entry.isContainerOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), doc);
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), doc);
    ASSERT_BSONOBJ_EQ(entry.getObjectContainingDocumentKey(), doc);
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT(!entry.getTid());
}

TEST_F(OplogEntryTest, Create) {
    CollectionOptions opts;
    opts.capped = true;
    opts.cappedSize = 15;
    opts.uuid = UUID::gen();

    // The 'object' document shouldn't contain the 'uuid' as it is specified at the top level.
    const auto oplogEntryObjectDoc =
        MutableOplogEntry::makeCreateCollObject(nss, opts, BSONObj() /* idIndex */);
    ASSERT_BSONOBJ_EQ(oplogEntryObjectDoc,
                      BSON("create" << nss.coll() << "capped" << true << "size" << 15));

    const auto entry = makeCommandOplogEntry(
        entryOpTime, nss, oplogEntryObjectDoc, boost::none /* object2 */, opts.uuid);

    ASSERT(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT_FALSE(entry.isCrudOpType());
    ASSERT_FALSE(entry.isContainerOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(oplogEntryObjectDoc, entry.getOperationToApply());
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kCreate);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT_EQ(entry.getUuid(), opts.uuid);
    ASSERT(!entry.getTid());
    ASSERT(!entry.getObject2());
}

TEST_F(OplogEntryTest, CreateWithCatalogIdentifier) {
    CollectionOptions opts;
    opts.capped = true;
    opts.cappedSize = 15;
    opts.uuid = UUID::gen();

    const auto oplogEntryObjectDoc =
        MutableOplogEntry::makeCreateCollObject(nss, opts, BSONObj() /* idIndex */);
    ASSERT_BSONOBJ_EQ(oplogEntryObjectDoc,
                      BSON("create" << nss.coll() << "capped" << true << "size" << 15));

    // Oplog entries generated with 'featureFlagReplicateLocalCatalogIdentifiers' enabled include
    // catalog identifier information in the 'o2' field.
    RecordId catalogId = RecordId(1);
    std::string ident = "collection_ident";
    std::string idIndexIdent = "id_index_ident";
    bool directoryPerDB = true;
    bool directoryForIndexes = true;
    const auto oplogEntryObject2Doc = MutableOplogEntry::makeCreateCollObject2(
        catalogId, ident, idIndexIdent, directoryPerDB, directoryForIndexes);
    ASSERT_BSONOBJ_EQ(oplogEntryObject2Doc,
                      BSON("catalogId" << 1 << "ident" << ident << "idIndexIdent" << idIndexIdent
                                       << "directoryPerDB" << true << "directoryForIndexes"
                                       << true));

    const auto entry = makeCommandOplogEntry(
        entryOpTime, nss, oplogEntryObjectDoc, oplogEntryObject2Doc, opts.uuid);

    ASSERT(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT_FALSE(entry.isCrudOpType());
    ASSERT_FALSE(entry.isContainerOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(oplogEntryObjectDoc, entry.getOperationToApply());
    ASSERT(entry.getCommandType() == OplogEntry::CommandType::kCreate);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT_EQ(entry.getUuid(), opts.uuid);
    ASSERT(!entry.getTid());
    ASSERT(entry.getObject2());
    ASSERT_BSONOBJ_EQ(*entry.getObject2(), oplogEntryObject2Doc);
}

TEST_F(OplogEntryTest, CreateO2RoundTrip) {
    // Tests the 'o2' object for a create oplog entry can be parsed and serialized back to its
    // original form when supplied with catalog identifiers.

    RecordId catalogId = RecordId(1);
    std::string ident = "collection_ident";
    std::string idIndexIdent = "id_index_ident";
    bool directoryPerDB = true;
    bool directoryForIndexes = true;
    const auto rawO2 = MutableOplogEntry::makeCreateCollObject2(
        catalogId, ident, idIndexIdent, directoryPerDB, directoryForIndexes);

    // Test parsing of 'o2' BSON.
    const auto parsedO2 = CreateOplogEntryO2::parse(rawO2, IDLParserContext("createOplogEntryO2"));
    const auto& parsedCatalogId = parsedO2.getCatalogId();
    const auto& parsedIdent = parsedO2.getIdent();
    const auto& parsedIdIndexIdent = parsedO2.getIdIndexIdent();
    const auto& parsedDirectoryPerDB = parsedO2.getDirectoryPerDB();
    const auto& parsedDirectoryForIndexes = parsedO2.getDirectoryForIndexes();

    ASSERT_EQ(catalogId, parsedCatalogId);
    ASSERT_EQ(ident, parsedIdent);
    ASSERT(parsedIdIndexIdent);
    ASSERT_EQ(idIndexIdent, *parsedIdIndexIdent);
    ASSERT(parsedDirectoryPerDB);
    ASSERT(parsedDirectoryForIndexes);

    // Confirm the parsed information can be round-tripped back to BSON.
    const auto serializedO2 = parsedO2.toBSON();
    ASSERT_BSONOBJ_EQ(rawO2, serializedO2);
}

TEST_F(OplogEntryTest, CreateIndexesO2RoundTrip) {
    // Tests the 'o2' object for a createIndexes oplog entry can be parsed and serialized back to
    // its original form when supplied with catalog identifiers.

    std::string indexIdent = "index_ident";
    bool directoryPerDB = true;
    bool directoryForIndexes = true;
    const auto rawO2 = BSON("indexIdent" << indexIdent << "directoryPerDB" << directoryPerDB
                                         << "directoryForIndexes" << directoryForIndexes);
    // Test parsing of 'o2' BSON.
    const auto parsedO2 =
        CreateIndexesOplogEntryO2::parse(rawO2, IDLParserContext("createIndexesOplogEntryO2"));
    const auto& parsedIndexIdent = parsedO2.getIndexIdent();
    const auto& parsedDirectoryPerDB = parsedO2.getDirectoryPerDB();
    const auto& parsedDirectoryForIndexes = parsedO2.getDirectoryForIndexes();

    ASSERT_EQ(indexIdent, parsedIndexIdent);
    ASSERT(parsedDirectoryPerDB);
    ASSERT(parsedDirectoryForIndexes);

    // Confirm the parsed information can be round-tripped back to BSON.
    const auto serializedO2 = parsedO2.toBSON();
    ASSERT_BSONOBJ_EQ(rawO2, serializedO2);
}

TEST_F(OplogEntryTest, StartIndexBuildO2RoundTrip) {
    // Tests the 'o2' object for a startIndexBuild oplog entry can be parsed and serialized back to
    // its original form when supplied with catalog identifiers.

    std::string indexIdent = "index_ident";
    bool directoryPerDB = true;
    bool directoryForIndexes = true;
    const auto rawO2 =
        BSON("indexes" << BSON_ARRAY(BSON("indexIdent" << indexIdent)) << "directoryPerDB"
                       << directoryPerDB << "directoryForIndexes" << directoryForIndexes);
    // Test parsing of 'o2' BSON.
    const auto parsedO2 =
        StartIndexBuildOplogEntryO2::parse(rawO2, IDLParserContext("startIndexBuildOplogEntryO2"));
    const auto& parsedIndexes = parsedO2.getIndexes();
    const auto& parsedDirectoryPerDB = parsedO2.getDirectoryPerDB();
    const auto& parsedDirectoryForIndexes = parsedO2.getDirectoryForIndexes();

    ASSERT_EQ(parsedIndexes.size(), 1);
    ASSERT_EQ(parsedIndexes[0].getIndexIdent(), indexIdent);
    ASSERT(parsedDirectoryPerDB);
    ASSERT(parsedDirectoryForIndexes);

    // Confirm the parsed information can be round-tripped back to BSON.
    const auto serializedO2 = parsedO2.toBSON();
    ASSERT_BSONOBJ_EQ(rawO2, serializedO2);
}

TEST_F(OplogEntryTest, ContainerInsert) {
    StringData containerIdent = "container_ident";
    auto key = BSONBinData("k", 1, BinDataType::BinDataGeneral);
    auto value = BSONBinData("v", 1, BinDataType::BinDataGeneral);
    auto entry = makeContainerInsertOplogEntry(entryOpTime, nss, containerIdent, key, value);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT_FALSE(entry.isCrudOpType());
    ASSERT_TRUE(entry.isContainerOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(),
                      BSON("k" << BSONBinData(key.data, key.length, key.type) << "v"
                               << BSONBinData(value.data, value.length, value.type)));
    ASSERT_EQ(entry.getCommandType(), OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT_FALSE(entry.getTid());
}

TEST_F(OplogEntryTest, ContainerDelete) {
    StringData containerIdent = "container_ident";
    auto key = BSONBinData("k", 1, BinDataType::BinDataGeneral);
    auto entry = makeContainerDeleteOplogEntry(entryOpTime, nss, containerIdent, key);

    ASSERT_FALSE(entry.isCommand());
    ASSERT_FALSE(entry.isPartialTransaction());
    ASSERT_FALSE(entry.isCrudOpType());
    ASSERT_TRUE(entry.isContainerOpType());
    ASSERT_FALSE(entry.shouldPrepare());
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(),
                      BSON("k" << BSONBinData(key.data, key.length, key.type)));
    ASSERT_EQ(entry.getCommandType(), OplogEntry::CommandType::kNotCommand);
    ASSERT_EQ(entry.getOpTime(), entryOpTime);
    ASSERT_FALSE(entry.getTid());
}

TEST_F(OplogEntryTest, ContainerInsertParse) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");

    const std::string ident = "test_ident";

    const BSONObj oplogBson = [&] {
        BSONObjBuilder bob;
        bob.append("ts", Timestamp(1, 1));
        bob.append("t", 1LL);
        bob.append("op", "ci");
        bob.append("ns", nss.ns_forTest());
        bob.append("container", ident);
        bob.append("wall", Date_t());

        BSONObjBuilder oBuilder(bob.subobjStart("o"));
        oBuilder.appendBinData("k", 3, BinDataGeneral, "abc");
        oBuilder.appendBinData("v", 4, BinDataGeneral, "defg");
        oBuilder.done();

        return bob.obj();
    }();

    auto entry = unittest::assertGet(DurableOplogEntry::parse(oplogBson));
    ASSERT_EQ(entry.getOpType(), repl::OpTypeEnum::kContainerInsert);
    ASSERT_EQ(entry.getNss(), nss);
    ASSERT_TRUE(entry.getContainer());
    ASSERT_EQ(*entry.getContainer(), ident);
}

TEST_F(OplogEntryTest, ContainerDeleteParse) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");

    const std::string ident = "test_ident";

    const BSONObj oplogBson = [&] {
        BSONObjBuilder bob;
        bob.append("ts", Timestamp(1, 1));
        bob.append("t", 1LL);
        bob.append("op", "cd");
        bob.append("ns", nss.ns_forTest());
        bob.append("container", ident);
        bob.append("wall", Date_t());

        BSONObjBuilder oBuilder(bob.subobjStart("o"));
        oBuilder.appendBinData("k", 3, BinDataGeneral, "abc");
        oBuilder.done();

        return bob.obj();
    }();

    auto entry = unittest::assertGet(DurableOplogEntry::parse(oplogBson));
    ASSERT_EQ(entry.getOpType(), repl::OpTypeEnum::kContainerDelete);
    ASSERT_EQ(entry.getNss(), nss);
    ASSERT_TRUE(entry.getContainer());
    ASSERT_EQ(*entry.getContainer(), ident);
}

TEST_F(OplogEntryTest, ContainerOpMissingContainer) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");

    const BSONObj oplogBson = [&] {
        BSONObjBuilder bob;
        bob.append("ts", Timestamp(1, 1));
        bob.append("t", 1LL);
        bob.append("op", "ci");
        bob.append("ns", nss.ns_forTest());
        bob.append("wall", Date_t());
        BSONObjBuilder oBuilder(bob.subobjStart("o"));
        oBuilder.appendBinData("k", 3, BinDataGeneral, "abc");
        oBuilder.appendBinData("v", 4, BinDataGeneral, "defg");
        oBuilder.done();

        return bob.obj();
    }();

    auto result = DurableOplogEntry::parse(oplogBson);
    ASSERT_EQ(result.getStatus().code(), 10704701);
}

TEST_F(OplogEntryTest, ApplyOpsNotInSession) {
    UUID uuid(UUID::gen());
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                             << "ns" << nss.ns_forTest() << "ui"
                                                             << uuid << "o" << BSON("_id" << 1)))));
    auto applyOpsEntry = unittest::assertGet(OplogEntry::parse(applyOpsBson));
    ASSERT_TRUE(applyOpsEntry.isCommand());
    ASSERT_FALSE(applyOpsEntry.isInTransaction());
    ASSERT_TRUE(applyOpsEntry.isTerminalApplyOps());
    ASSERT_FALSE(applyOpsEntry.isSingleOplogEntryTransaction());
    ASSERT_FALSE(applyOpsEntry.isPartialTransaction());
    ASSERT_FALSE(applyOpsEntry.isEndOfLargeTransaction());
    ASSERT_FALSE(applyOpsEntry.applyOpsIsLinkedTransactionally());
}

TEST_F(OplogEntryTest, ApplyOpsSingleEntryTransaction) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                             << "ns" << nss.ns_forTest() << "ui"
                                                             << uuid << "o" << BSON("_id" << 1))))
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

TEST_F(OplogEntryTest, ApplyOpsStartMultiEntryTransaction) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                             << "ns" << nss.ns_forTest() << "ui"
                                                             << uuid << "o" << BSON("_id" << 1)))
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

TEST_F(OplogEntryTest, ApplyOpsMiddleMultiEntryTransaction) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 2) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                             << "ns" << nss.ns_forTest() << "ui"
                                                             << uuid << "o" << BSON("_id" << 1)))
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

TEST_F(OplogEntryTest, ApplyOpsEndMultiEntryTransaction) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 2) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                             << "ns" << nss.ns_forTest() << "ui"
                                                             << uuid << "o" << BSON("_id" << 1))))
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

TEST_F(OplogEntryTest, ApplyOpsFirstOrOnlyRetryableWrite) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                             << "ns" << nss.ns_forTest() << "ui"
                                                             << uuid << "o" << BSON("_id" << 1))))
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

TEST_F(OplogEntryTest, ApplyOpsSubsequentRetryableWrite) {
    UUID uuid(UUID::gen());
    auto sessionId = makeLogicalSessionIdForTest();
    const auto applyOpsBson =
        BSON("ts" << Timestamp(1, 2) << "t" << 1LL << "op"
                  << "c"
                  << "ns"
                  << "admin.$cmd"
                  << "wall" << Date_t() << "o"
                  << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                             << "ns" << nss.ns_forTest() << "ui"
                                                             << uuid << "o" << BSON("_id" << 1))))
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

TEST_F(OplogEntryTest, OpTimeBaseNonStrictParsing) {
    const BSONObj oplogEntryExtraField = BSON("ts" << Timestamp(0, 0) << "t" << 0LL << "op"
                                                   << "c"
                                                   << "ns" << nss.ns_forTest() << "wall" << Date_t()
                                                   << "o" << BSON("_id" << 1) << "extraField" << 3);

    // OpTimeBase should be successfully created from an OplogEntry, even though it has
    // extraneous fields.
    UNIT_TEST_INTERNALS_IGNORE_UNUSED_RESULT_WARNINGS(
        OpTimeBase::parse(oplogEntryExtraField, IDLParserContext("OpTimeBase")));

    // OplogEntryBase should still use strict parsing and throw an error when it has extraneous
    // fields.
    ASSERT_THROWS_CODE(
        OplogEntryBase::parse(oplogEntryExtraField, IDLParserContext("OplogEntryBase")),
        AssertionException,
        40415);

    const BSONObj oplogEntryMissingTimestamp =
        BSON("t" << 0LL << "op"
                 << "c"
                 << "ns" << nss.ns_forTest() << "wall" << Date_t() << "o" << BSON("_id" << 1));

    // When an OplogEntryBase is created with a missing required field in a chained struct, it
    // should throw an exception.
    ASSERT_THROWS_CODE(
        OplogEntryBase::parse(oplogEntryMissingTimestamp, IDLParserContext("OplogEntryBase")),
        AssertionException,
        ErrorCodes::IDLFailedToParse);
}

TEST_F(OplogEntryTest, InsertIncludesTidField) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    const BSONObj doc = BSON("_id" << docId << "a" << 5);
    TenantId tid(OID::gen());
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "foo", "bar");
    const auto entry = makeInsertDocumentOplogEntry(entryOpTime, nss, doc);

    ASSERT(entry.getTid());
    ASSERT_EQ(*entry.getTid(), tid);
    ASSERT_EQ(entry.getNss(), nss);
    ASSERT_BSONOBJ_EQ(entry.getIdElement().wrap("_id"), BSON("_id" << docId));
    ASSERT_BSONOBJ_EQ(entry.getOperationToApply(), doc);
}

TEST_F(OplogEntryTest, ParseMutableOplogEntryIncludesTidField) {
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

TEST_F(OplogEntryTest, ParseDurableOplogEntryIncludesTidField) {
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

TEST_F(OplogEntryTest, ParseReplOperationIncludesTidField) {
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
        oplogBson,
        IDLParserContext("ReplOperation", vts, tid, SerializationContext::stateDefault()));
    ASSERT(replOp.getTid());
    ASSERT_EQ(replOp.getTid(), tid);
    ASSERT_EQ(replOp.getNss(), nssWithTid);
}

TEST_F(OplogEntryTest, ConvertMutableOplogEntryToReplOperation) {
    // Required by setTid to take effect
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    RAIIServerParameterControllerForTest multitenancySupportController("multitenancySupport", true);
    auto tid = TenantId(OID::gen());
    auto nssWithTid = NamespaceString::createNamespaceString_forTest(tid, nss.ns_forTest());
    auto opType = repl::OpTypeEnum::kCommand;
    auto uuid = UUID::gen();
    std::vector<StmtId> stmtIds{StmtId(0), StmtId(1), StmtId(2)};
    const auto doc = BSON("x" << 1);
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    VersionContext vCtx{multiversion::GenericFCV::kLastLTS};

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
    entry.setVersionContext(vCtx);

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
    ASSERT_EQ(replOp.getVersionContext(), vCtx);
    ASSERT_EQ(replOp.getVersionContext(), entry.getVersionContext());

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

TEST_F(OplogEntryTest, StatementIDParseAndSerialization) {
    UnorderedFieldsBSONObjComparator bsonCompare;
    const BSONObj oplogEntryWithNoStmtId =
        BSON("op" << "c"
                  << "ns" << nss.ns_forTest() << "o" << BSON("_id" << 1) << "v" << 2 << "ts"
                  << Timestamp(0, 0) << "t" << 0LL << "wall" << Date_t());

    auto oplogEntryBaseNoStmtId =
        OplogEntryBase::parse(oplogEntryWithNoStmtId, IDLParserContext("OplogEntry"));
    ASSERT_TRUE(oplogEntryBaseNoStmtId.getStatementIds().empty());
    auto rtOplogEntryWithNoStmtId = oplogEntryBaseNoStmtId.toBSON();
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithNoStmtId, rtOplogEntryWithNoStmtId), 0)
        << "Did not round trip: " << oplogEntryWithNoStmtId << " should be equal to "
        << rtOplogEntryWithNoStmtId;

    const BSONObj oplogEntryWithOneStmtId =
        BSON("op" << "c"
                  << "ns" << nss.ns_forTest() << "o" << BSON("_id" << 1) << "v" << 2 << "ts"
                  << Timestamp(0, 0) << "t" << 0LL << "wall" << Date_t() << "stmtId" << 99);
    auto oplogEntryBaseOneStmtId =
        OplogEntryBase::parse(oplogEntryWithOneStmtId, IDLParserContext("OplogEntry"));
    ASSERT_EQ(oplogEntryBaseOneStmtId.getStatementIds(), std::vector<StmtId>{99});
    auto rtOplogEntryWithOneStmtId = oplogEntryBaseOneStmtId.toBSON();
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithOneStmtId, rtOplogEntryWithOneStmtId), 0)
        << "Did not round trip: " << oplogEntryWithOneStmtId << " should be equal to "
        << rtOplogEntryWithOneStmtId;
    // Statement id should be NumberInt, not NumberLong or some other numeric.
    ASSERT_EQ(rtOplogEntryWithOneStmtId["stmtId"].type(), BSONType::numberInt);

    const BSONObj oplogEntryWithMultiStmtId =
        BSON("op" << "c"
                  << "ns" << nss.ns_forTest() << "o" << BSON("_id" << 1) << "v" << 2 << "ts"
                  << Timestamp(0, 0) << "t" << 0LL << "wall" << Date_t() << "stmtId"
                  << BSON_ARRAY(101 << 102 << 103));
    auto oplogEntryBaseMultiStmtId =
        OplogEntryBase::parse(oplogEntryWithMultiStmtId, IDLParserContext("OplogEntry"));
    ASSERT_EQ(oplogEntryBaseMultiStmtId.getStatementIds(), (std::vector<StmtId>{101, 102, 103}));
    auto rtOplogEntryWithMultiStmtId = oplogEntryBaseMultiStmtId.toBSON();
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithMultiStmtId, rtOplogEntryWithMultiStmtId), 0)
        << "Did not round trip: " << oplogEntryWithMultiStmtId << " should be equal to "
        << rtOplogEntryWithMultiStmtId;
    // Array entries should be NumberInt, not NumberLong or some other numeric.
    ASSERT_EQ(rtOplogEntryWithMultiStmtId["stmtId"]["0"].type(), BSONType::numberInt);

    // A non-canonical entry with an empty stmtId array.
    const BSONObj oplogEntryWithEmptyStmtId = BSON(
        "op" << "c"
             << "ns" << nss.ns_forTest() << "o" << BSON("_id" << 1) << "v" << 2 << "ts"
             << Timestamp(0, 0) << "t" << 0LL << "wall" << Date_t() << "stmtId" << BSONArray());

    auto oplogEntryBaseEmptyStmtId =
        OplogEntryBase::parse(oplogEntryWithEmptyStmtId, IDLParserContext("OplogEntry"));
    ASSERT_TRUE(oplogEntryBaseEmptyStmtId.getStatementIds().empty());
    auto rtOplogEntryWithEmptyStmtId = oplogEntryBaseEmptyStmtId.toBSON();
    // This round-trips to the canonical version with no statement ID.
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithNoStmtId, rtOplogEntryWithEmptyStmtId), 0)
        << "Did not round trip: " << oplogEntryWithNoStmtId << " should be equal to "
        << rtOplogEntryWithEmptyStmtId;

    // A non-canonical entry with a singleton stmtId array.
    const BSONObj oplogEntryWithSingletonStmtId = BSON(
        "op" << "c"
             << "ns" << nss.ns_forTest() << "o" << BSON("_id" << 1) << "v" << 2 << "ts"
             << Timestamp(0, 0) << "t" << 0LL << "wall" << Date_t() << "stmtId" << BSON_ARRAY(99));

    auto oplogEntryBaseSingletonStmtId =
        OplogEntryBase::parse(oplogEntryWithSingletonStmtId, IDLParserContext("OplogEntry"));
    ASSERT_EQ(oplogEntryBaseSingletonStmtId.getStatementIds(), std::vector<StmtId>{99});
    auto rtOplogEntryWithSingletonStmtId = oplogEntryBaseSingletonStmtId.toBSON();
    // This round-trips to the canonical version with a non-array statement ID.
    ASSERT_EQ(bsonCompare.compare(oplogEntryWithOneStmtId, rtOplogEntryWithSingletonStmtId), 0)
        << "Did not round trip: " << oplogEntryWithNoStmtId << " should be equal to "
        << rtOplogEntryWithSingletonStmtId;
}

TEST(OplogEntryParserTest, ParseOpTimeSuccess) {
    repl::OpTime opTime{Timestamp{2}, 1};
    auto const oplogEntry = opTime.toBSON();
    OplogEntryParserNonStrict parser{oplogEntry};
    ASSERT_EQ(opTime, parser.getOpTime()) << oplogEntry.toString();
}

TEST(OplogEntryParserTest, ParseOpTimeFailure) {
    auto const oplogEntry = BSON("a" << 1);
    OplogEntryParserNonStrict parser{oplogEntry};
    ASSERT_THROWS_CODE_AND_WHAT(parser.getOpTime(),
                                AssertionException,
                                ErrorCodes::IDLFailedToParse,
                                "Failed to parse opTime :: caused by :: "
                                "BSON field 'OpTimeBase.ts' is missing but a required field");
}

TEST(OplogEntryParserTest, ParseOpTypeSuccess) {
    auto const oplogEntry =
        BSON(OplogEntry::kOpTypeFieldName << OpType_serializer(repl::OpTypeEnum::kDelete));
    OplogEntryParserNonStrict parser{oplogEntry};
    ASSERT_EQ(repl::OpTypeEnum::kDelete, parser.getOpType());
}

TEST(OplogEntryParserTest, ParseOpTypeFailure) {
    {
        auto const oplogEntry = BSON(OplogEntry::kOpTypeFieldName << "zz");
        OplogEntryParserNonStrict parser{oplogEntry};
        ASSERT_THROWS_CODE_AND_WHAT(
            parser.getOpType(),
            AssertionException,
            ErrorCodes::BadValue,
            "Enumeration value 'zz' for field 'ChangeStreamEntry.op' is not a valid value.");
    }
    {
        auto const oplogEntry = BSON(OplogEntry::kOpTypeFieldName << 1);
        OplogEntryParserNonStrict parser{oplogEntry};
        ASSERT_THROWS_CODE_AND_WHAT(parser.getOpType(),
                                    AssertionException,
                                    8881100,
                                    "Invalid 'op' field type (expected String)");
    }
}

TEST(OplogEntryParserTest, ParseObjectSuccess) {
    auto const objectFieldValue = BSON("a" << 1);
    auto const oplogEntry = BSON(OplogEntry::kObjectFieldName << objectFieldValue);
    OplogEntryParserNonStrict parser{oplogEntry};
    ASSERT_BSONOBJ_BINARY_EQ(objectFieldValue, parser.getObject());
}

TEST(OplogEntryParserTest, ParseObjectFailure) {
    {
        auto const oplogEntry = BSON(OplogEntry::kObjectFieldName << "string");
        OplogEntryParserNonStrict parser{oplogEntry};
        ASSERT_THROWS_CODE_AND_WHAT(parser.getObject(),
                                    AssertionException,
                                    8881101,
                                    "Invalid 'o' field type (expected Object)");
    }
    {
        auto const oplogEntry = BSON("a" << 1);
        OplogEntryParserNonStrict parser{oplogEntry};
        ASSERT_THROWS_CODE_AND_WHAT(parser.getObject(),
                                    AssertionException,
                                    8881101,
                                    "Invalid 'o' field type (expected Object)");
    }
}

#define ASSERT_BSONOBJ_VECTOR_EQ(a, b)            \
    do {                                          \
        ASSERT_EQ((a).size(), (b).size());        \
        for (size_t i = 0; i < (a).size(); ++i) { \
            ASSERT_BSONOBJ_EQ((a)[i], (b)[i]);    \
        }                                         \
    } while (0)

TEST_F(OplogEntryTest, ParseValidIndexBuildOplogEntry) {
    const std::string ns = "test.coll";
    const auto nss = NamespaceString::createNamespaceString_forTest(ns);
    const UUID indexBuildUUID = UUID::gen();
    const std::vector<BSONObj> indexSpecs = {
        BSON("v" << 2 << "key" << BSON("x" << 1) << "name"
                 << "x_1"),
        BSON("v" << 2 << "key" << BSON("y" << 1) << "name"
                 << "y_1"),
    };
    const std::vector<std::string> indexNames = {"x_1", "y_1"};
    const std::vector<BSONObj> o2Indexes = {BSON("indexIdent" << "index-0"),
                                            BSON("indexIdent" << "index-1")};

    auto uuid = UUID::gen();

    {
        const auto o = BSON("startIndexBuild" << ns << "indexBuildUUID" << indexBuildUUID
                                              << "indexes" << indexSpecs);
        const auto o2 = BSON("indexes" << o2Indexes << "directoryPerDB" << true
                                       << "directoryForIndexes" << true);

        const auto entry = makeCommandOplogEntry(entryOpTime, nss, o, o2, uuid);
        auto parsed = unittest::assertGet(IndexBuildOplogEntry::parse(_opCtx.get(), entry));
        ASSERT_EQ(parsed.collUUID, uuid);
        ASSERT_EQ(parsed.commandType, OplogEntry::CommandType::kStartIndexBuild);
        ASSERT_EQ(parsed.commandName, "startIndexBuild");
        ASSERT_EQ(parsed.indexes.size(), 2);
        ASSERT_EQ(toIndexNames(parsed.indexes), indexNames);
        ASSERT_BSONOBJ_VECTOR_EQ(toIndexSpecs(parsed.indexes), indexSpecs);
        ASSERT_EQ(parsed.indexes[0].indexIdent, o2Indexes[0].getField("indexIdent").str());
        ASSERT_EQ(parsed.indexes[1].indexIdent, o2Indexes[1].getField("indexIdent").str());
        ASSERT_FALSE(parsed.cause);
    }

    {
        const auto o = BSON("startIndexBuild" << ns << "indexBuildUUID" << indexBuildUUID
                                              << "indexes" << indexSpecs);
        const auto entry = makeCommandOplogEntry(entryOpTime, nss, o, boost::none, uuid);
        auto parsed = unittest::assertGet(IndexBuildOplogEntry::parse(_opCtx.get(), entry));
        ASSERT_EQ(parsed.indexes.size(), 2);
        ASSERT(parsed.indexes[0].indexIdent.empty());
        ASSERT(parsed.indexes[1].indexIdent.empty());
    }

    {
        const auto o = BSON("commitIndexBuild" << ns << "indexBuildUUID" << indexBuildUUID
                                               << "indexes" << indexSpecs);
        const auto entry = makeCommandOplogEntry(entryOpTime, nss, o, boost::none, uuid);
        auto parsed = unittest::assertGet(IndexBuildOplogEntry::parse(_opCtx.get(), entry));
        ASSERT_EQ(parsed.collUUID, uuid);
        ASSERT_EQ(parsed.commandType, OplogEntry::CommandType::kCommitIndexBuild);
        ASSERT_EQ(parsed.commandName, "commitIndexBuild");
        ASSERT_EQ(toIndexNames(parsed.indexes), indexNames);
        ASSERT_BSONOBJ_VECTOR_EQ(toIndexSpecs(parsed.indexes), indexSpecs);
        ASSERT_EQ(parsed.indexes.size(), 2);
        ASSERT(parsed.indexes[0].indexIdent.empty());
        ASSERT(parsed.indexes[1].indexIdent.empty());
        ASSERT_FALSE(parsed.cause);
    }

    {
        BSONObjBuilder builder;
        builder.append("ok", false);
        const auto cause = Status(ErrorCodes::IndexBuildAborted, "aborted");
        cause.serializeErrorToBSON(&builder);
        const auto o =
            BSON("abortIndexBuild" << ns << "indexBuildUUID" << indexBuildUUID << "indexes"
                                   << indexSpecs << "cause" << builder.obj());
        const auto entry = makeCommandOplogEntry(entryOpTime, nss, o, boost::none, uuid);
        auto parsed = unittest::assertGet(IndexBuildOplogEntry::parse(_opCtx.get(), entry));
        ASSERT_EQ(parsed.collUUID, uuid);
        ASSERT_EQ(parsed.commandType, OplogEntry::CommandType::kAbortIndexBuild);
        ASSERT_EQ(parsed.commandName, "abortIndexBuild");
        ASSERT_EQ(toIndexNames(parsed.indexes), indexNames);
        ASSERT_BSONOBJ_VECTOR_EQ(toIndexSpecs(parsed.indexes), indexSpecs);
        ASSERT_EQ(parsed.indexes.size(), 2);
        ASSERT(parsed.indexes[0].indexIdent.empty());
        ASSERT(parsed.indexes[1].indexIdent.empty());
        ASSERT_EQ(parsed.cause, cause);
    }
}

TEST_F(OplogEntryTest, ParseInvalidIndexBuildOplogEntry) {
    auto parse = [&](BSONObj o, boost::optional<BSONObj> o2 = boost::none) {
        auto entry = makeCommandOplogEntry(entryOpTime, nss, o, o2, UUID::gen());
        auto parsed = IndexBuildOplogEntry::parse(_opCtx.get(), entry);
        ASSERT_NOT_OK(parsed);
        return parsed.getStatus();
    };

    BSONObj baseObj =
        BSON("startIndexBuild" << "test.coll"
                               << "indexBuildUUID" << UUID::gen() << "indexes"
                               << BSON_ARRAY(BSON("v" << 2 << "key" << BSON("x" << 1) << "name"
                                                      << "x_1")));
    auto setField = [&](StringData name, auto value) {
        return baseObj.addFields(BSON(name << value));
    };

    ASSERT_EQ(parse(baseObj.removeField("indexBuildUUID")), ErrorCodes::BadValue);
    ASSERT_EQ(parse(baseObj.removeField("indexes")), ErrorCodes::BadValue);

    ASSERT_EQ(parse(setField("startIndexBuild", 1)), ErrorCodes::InvalidNamespace);
    ASSERT_EQ(parse(setField("indexBuildUUID", "")), ErrorCodes::InvalidUUID);
    ASSERT_EQ(parse(setField("indexes", "")), ErrorCodes::BadValue);
    ASSERT_EQ(parse(setField("indexes", BSON_ARRAY(""))), ErrorCodes::BadValue);
    ASSERT_EQ(parse(setField("indexes", BSON_ARRAY(BSON("nameless" << "")))),
              ErrorCodes::NoSuchKey);
    // parse does not verify that the specs are valid beyond having names, so no further tests for
    // them here

    ASSERT_THROWS_CODE(parse(baseObj, BSONObj()), AssertionException, ErrorCodes::IDLFailedToParse);
    ASSERT_THROWS_CODE(
        parse(baseObj, BSON("indexes" << 1)), AssertionException, ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(parse(baseObj, BSON("indexes" << BSON_ARRAY(1))),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(parse(baseObj, BSON("indexes" << BSON_ARRAY("malformed-ident"))),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(
        parse(baseObj, BSON("indexes" << BSON_ARRAY(BSON("invalid-ident" << "ident")))),
        AssertionException,
        ErrorCodes::IDLFailedToParse);
    ASSERT_THROWS_CODE(parse(baseObj, BSON("indexes" << BSON_ARRAY(BSON("indexIdent" << 1)))),
                       AssertionException,
                       ErrorCodes::TypeMismatch);

    baseObj =
        BSON("abortIndexBuild" << "test.coll"
                               << "indexBuildUUID" << UUID::gen() << "indexes"
                               << BSON_ARRAY(BSON("v" << 2 << "key" << BSON("x" << 1) << "name"
                                                      << "x_1")));
    ASSERT_EQ(parse(baseObj), ErrorCodes::BadValue);
    ASSERT_EQ(parse(setField("cause", 1)), ErrorCodes::BadValue);

    // The cause field being an object which can't be interpreted as a Status results in the
    // top-level parse succeeding and the "cause" field reporting a parse error
    {
        auto entry = makeCommandOplogEntry(
            entryOpTime, nss, setField("cause", BSONObj()), boost::none, UUID::gen());
        auto parsed = IndexBuildOplogEntry::parse(_opCtx.get(), entry);
        ASSERT_OK(parsed);
        ASSERT_NOT_OK(parsed.getValue().cause);
    }
}

// The caller is expected to only call parse on command entries with a command type of
// startIndexBuild, commitIndexBuild, or abortIndexBuild.
DEATH_TEST_F(OplogEntryTest, ParseNonCommandOperation, "kCommand") {
    // Deliberately create a NON-command op in the command namespace.
    auto entry = makeInsertDocumentOplogEntry(entryOpTime, nss.getCommandNS(), BSONObj{});
    IndexBuildOplogEntry::parse(_opCtx.get(), entry).getValue();
}

DEATH_TEST_F(OplogEntryTest, ParseWrongCommandOperation, "CommandType") {
    // A valid command type, but not one supported by this function
    auto entry = makeCommandOplogEntry(
        entryOpTime, nss, BSON("applyOps" << "test.coll"), boost::none, UUID::gen());
    IndexBuildOplogEntry::parse(_opCtx.get(), entry).getValue();
}

}  // namespace
}  // namespace repl
}  // namespace mongo
