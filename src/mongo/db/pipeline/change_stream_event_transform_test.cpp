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

#include "mongo/db/pipeline/change_stream_event_transform.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/change_stream_test_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/time_support.h"

#include <vector>

#include <boost/none.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace change_stream_test_helper;

repl::MutableOplogEntry buildMovePrimaryOplogEntry(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   const ShardId& oldPrimary,
                                                   const ShardId& newPrimary) {
    repl::MutableOplogEntry oplogEntry;
    const auto dbNameStr =
        DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());

    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(NamespaceString(dbName));
    oplogEntry.setObject(BSON("msg" << BSON("movePrimary" << dbNameStr)));
    oplogEntry.setObject2(
        BSON("movePrimary" << dbNameStr << "from" << oldPrimary << "to" << newPrimary));
    oplogEntry.setOpTime(repl::OpTime(kDefaultTs, 0));
    oplogEntry.setWallClockTime(Date_t());

    return oplogEntry;
}

Document applyTransformation(const repl::OplogEntry& oplogEntry,
                             NamespaceString ns = nss,
                             const std::vector<std::string>& supportedEvents = {}) {
    const auto oplogDoc = Document(oplogEntry.getEntry().toBSON());
    DocumentSourceChangeStreamSpec spec;
    spec.setStartAtOperationTime(kDefaultTs);
    spec.setSupportedEvents(supportedEvents);
    spec.setShowExpandedEvents(true);

    ChangeStreamEventTransformer transformer(make_intrusive<ExpressionContextForTest>(ns), spec);
    return transformer.applyTransformation(oplogDoc);
}

TEST(ChangeStreamEventTransformTest, TestDefaultUpdateTransform) {
    const auto documentKey = Document{{"x", 1}, {"y", 1}};
    auto updateField =
        makeOplogEntry(repl::OpTypeEnum::kUpdate,                                 // op type
                       nss,                                                       // namespace
                       BSON("$v" << 2 << "diff" << BSON("u" << BSON("y" << 2))),  // o
                       testUuid(),                                                // uuid
                       boost::none,                                               // fromMigrate
                       documentKey.toBson());                                     // o2

    // Update fields
    Document expectedUpdateField{
        {DocumentSourceChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         testUuid(),
                         Value(documentKey),
                         DocumentSourceChangeStream::kUpdateOpType)},
        {DocumentSourceChangeStream::kOperationTypeField,
         DocumentSourceChangeStream::kUpdateOpType},
        {DocumentSourceChangeStream::kClusterTimeField, kDefaultTs},
        {DocumentSourceChangeStream::kCollectionUuidField, testUuid()},
        {DocumentSourceChangeStream::kWallTimeField, Date_t()},
        {DocumentSourceChangeStream::kNamespaceField,
         Document{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DocumentSourceChangeStream::kDocumentKeyField, documentKey},
        {
            "updateDescription",
            Document{{"updatedFields", Document{{"y", 2}}},
                     {"removedFields", std::vector<Value>()},
                     {"truncatedArrays", std::vector<Value>()},
                     {"disambiguatedPaths", Document{}}},
        },
    };

    ASSERT_DOCUMENT_EQ(applyTransformation(updateField), expectedUpdateField);
}

TEST(ChangeStreamEventTransformTest, TestCreateCollectionTransform) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "testDB.coll.name");
    // Namespace for the command, i.e. "testDB.$cmd".
    const NamespaceString commandNss = NamespaceString::makeCommandNamespace(nss.dbName());
    const auto opDescription = Value(fromjson("{idIndex: {v: 2, key: {_id: 1}, name: '_id_'}}"));
    const auto idIndex = Value(fromjson("{v: 2, key: {_id: 1}, name: '_id_'}"));
    auto oplogEntry = makeOplogEntry(repl::OpTypeEnum::kCommand,  // op type
                                     commandNss,                  // namespace
                                     BSON("create" << nss.coll() << "idIndex" << idIndex),  // o
                                     testUuid(),                                            // uuid
                                     boost::none,   // fromMigrate
                                     boost::none);  // o2

    Document expectedDoc{
        {DocumentSourceChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), opDescription, DocumentSourceChangeStream::kCreateOpType)},
        {DocumentSourceChangeStream::kOperationTypeField,
         DocumentSourceChangeStream::kCreateOpType},
        {DocumentSourceChangeStream::kClusterTimeField, kDefaultTs},
        {DocumentSourceChangeStream::kCollectionUuidField, testUuid()},
        {DocumentSourceChangeStream::kWallTimeField, Date_t()},
        {DocumentSourceChangeStream::kNamespaceField,
         Document{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DocumentSourceChangeStream::kOperationDescriptionField, opDescription},
        {DocumentSourceChangeStream::kNsTypeField, "collection"_sd}};

    ASSERT_DOCUMENT_EQ(applyTransformation(oplogEntry), expectedDoc);
}

TEST(ChangeStreamEventTransformTest, TestCreateIndexTransform) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "testDB.coll.name");
    // Namespace for the command, i.e. "testDB.$cmd".
    const NamespaceString commandNss = NamespaceString::makeCommandNamespace(nss.dbName());
    const auto opDescription = Value(fromjson("{indexes: [{v: 2, key: {a: 1}, name: 'a_1'}]}"));
    auto oplogEntry = makeOplogEntry(repl::OpTypeEnum::kCommand,  // op type
                                     commandNss,                  // namespace
                                     BSON("createIndexes" << nss.coll() << "v" << 2 << "key"
                                                          << BSON("a" << 1) << "name"
                                                          << "a_1"),  // o
                                     testUuid(),                      // uuid
                                     boost::none,                     // fromMigrate
                                     boost::none);                    // o2

    Document expectedDoc{{DocumentSourceChangeStream::kIdField,
                          makeResumeToken(kDefaultTs,
                                          testUuid(),
                                          opDescription,
                                          DocumentSourceChangeStream::kCreateIndexesOpType)},
                         {DocumentSourceChangeStream::kOperationTypeField,
                          DocumentSourceChangeStream::kCreateIndexesOpType},
                         {DocumentSourceChangeStream::kClusterTimeField, kDefaultTs},
                         {DocumentSourceChangeStream::kCollectionUuidField, testUuid()},
                         {DocumentSourceChangeStream::kWallTimeField, Date_t()},
                         {DocumentSourceChangeStream::kNamespaceField,
                          Document{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
                         {DocumentSourceChangeStream::kOperationDescriptionField, opDescription}};

    ASSERT_DOCUMENT_EQ(applyTransformation(oplogEntry), expectedDoc);
}

TEST(ChangeStreamEventTransformTest, TestCreateViewTransform) {
    const NamespaceString systemViewNss = NamespaceString::makeSystemDotViewsNamespace(
        DatabaseName::createDatabaseName_forTest(boost::none, "viewDB"));
    const NamespaceString viewNss =
        NamespaceString::createNamespaceString_forTest(boost::none, "viewDB.view.name");
    const auto viewPipeline =
        Value(fromjson("[{$match: {field: 'value'}}, {$project: {field: 1}}]"));
    const auto opDescription = Document{{"viewOn", "baseColl"_sd}, {"pipeline", viewPipeline}};
    auto oplogEntry = makeOplogEntry(repl::OpTypeEnum::kInsert,  // op type
                                     systemViewNss,              // namespace
                                     BSON("_id" << viewNss.toString_forTest() << "viewOn"
                                                << "baseColl"
                                                << "pipeline" << viewPipeline),  // o
                                     testUuid(),                                 // uuid
                                     boost::none,                                // fromMigrate
                                     boost::none);                               // o2

    Document expectedDoc{
        {DocumentSourceChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), opDescription, DocumentSourceChangeStream::kCreateOpType)},
        {DocumentSourceChangeStream::kOperationTypeField,
         DocumentSourceChangeStream::kCreateOpType},
        {DocumentSourceChangeStream::kClusterTimeField, kDefaultTs},
        {DocumentSourceChangeStream::kWallTimeField, Date_t()},
        {DocumentSourceChangeStream::kNamespaceField,
         Document{{"db", viewNss.db_forTest()}, {"coll", viewNss.coll()}}},
        {DocumentSourceChangeStream::kOperationDescriptionField, opDescription},
        {DocumentSourceChangeStream::kNsTypeField, "view"_sd}};

    ASSERT_DOCUMENT_EQ(
        applyTransformation(oplogEntry,
                            NamespaceString::makeCollectionlessAggregateNSS(
                                DatabaseName::createDatabaseName_forTest(boost::none, "viewDB"))),
        expectedDoc);
}

TEST(ChangeStreamEventTransformTest, TestCreateTimeseriesTransform) {
    const NamespaceString systemViewNss = NamespaceString::makeSystemDotViewsNamespace(
        DatabaseName::createDatabaseName_forTest(boost::none, "timeseriesDB"));
    const NamespaceString collNss =
        NamespaceString::createNamespaceString_forTest("timeseriesDB"_sd, "timeseriesColl"_sd);
    const NamespaceString viewNss = collNss.makeTimeseriesBucketsNamespace();
    const auto viewPipeline = Value(
        fromjson("[{_internalUnpackBucket: {timeField: 'time', bucketMaxSpanSeconds: 3600}}]"));
    const auto opDescription = Document{{"viewOn", viewNss.coll()}, {"pipeline", viewPipeline}};
    auto oplogEntry = makeOplogEntry(
        repl::OpTypeEnum::kInsert,  // op type
        NamespaceString::makeSystemDotViewsNamespace(
            DatabaseName::createDatabaseName_forTest(boost::none, "timeseriesDB"_sd)),  // namespace
        BSON("_id" << collNss.toString_forTest() << "viewOn" << viewNss.coll() << "pipeline"
                   << viewPipeline),  // o
        testUuid(),                   // uuid
        boost::none,                  // fromMigrate
        BSON("_id" << collNss.toString_forTest()));

    Document expectedDoc{
        {DocumentSourceChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), opDescription, DocumentSourceChangeStream::kCreateOpType)},
        {DocumentSourceChangeStream::kOperationTypeField,
         DocumentSourceChangeStream::kCreateOpType},
        {DocumentSourceChangeStream::kClusterTimeField, kDefaultTs},
        {DocumentSourceChangeStream::kWallTimeField, Date_t()},
        {DocumentSourceChangeStream::kNamespaceField,
         Document{{"db", viewNss.db_forTest()}, {"coll", collNss.coll()}}},
        {DocumentSourceChangeStream::kOperationDescriptionField, opDescription},
        {DocumentSourceChangeStream::kNsTypeField, "timeseries"_sd}};

    ASSERT_DOCUMENT_EQ(applyTransformation(oplogEntry,
                                           NamespaceString::makeCollectionlessAggregateNSS(
                                               DatabaseName::createDatabaseName_forTest(
                                                   boost::none, "timeseriesDB"))),
                       expectedDoc);
}

TEST(ChangeStreamEventTransformTest, TestCreateViewOnSingleCollection) {
    const NamespaceString systemViewNss = NamespaceString::makeSystemDotViewsNamespace(
        DatabaseName::createDatabaseName_forTest(boost::none, "viewDB"));
    const NamespaceString viewNss =
        NamespaceString::createNamespaceString_forTest(boost::none, "viewDB.view.name");
    const auto viewPipeline =
        Value(fromjson("[{$match: {field: 'value'}}, {$project: {field: 1}}]"));
    const auto document = BSON("_id" << viewNss.toString_forTest() << "viewOn"
                                     << "baseColl"
                                     << "pipeline" << viewPipeline);
    const auto documentKey = Value(Document{{"_id", document["_id"]}});
    auto oplogEntry = makeOplogEntry(repl::OpTypeEnum::kInsert,  // op type
                                     systemViewNss,              // namespace
                                     document,                   // o
                                     testUuid(),                 // uuid
                                     boost::none,                // fromMigrate
                                     boost::none);               // o2

    Document expectedDoc{
        {DocumentSourceChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), documentKey, DocumentSourceChangeStream::kInsertOpType)},
        {DocumentSourceChangeStream::kOperationTypeField,
         DocumentSourceChangeStream::kInsertOpType},
        {DocumentSourceChangeStream::kClusterTimeField, kDefaultTs},
        {DocumentSourceChangeStream::kCollectionUuidField, testUuid()},
        {DocumentSourceChangeStream::kWallTimeField, Date_t()},
        {DocumentSourceChangeStream::kFullDocumentField, Document(document)},
        {DocumentSourceChangeStream::kNamespaceField,
         Document{{"db", systemViewNss.db_forTest()}, {"coll", systemViewNss.coll()}}},
        {DocumentSourceChangeStream::kDocumentKeyField, documentKey}};

    ASSERT_DOCUMENT_EQ(applyTransformation(oplogEntry), expectedDoc);
}

TEST(ChangeStreamEventTransformTest,
     Given_NoopOplogEntry_When_CallingTransform_Then_FieldsAreNotCopied) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "testDB.coll.name");

    // Create a noop oplog entry that represents a 'shardCollection' event.
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(nss);
    oplogEntry.setObject(BSON("msg" << BSON("shardCollection" << nss.toString_forTest())));
    oplogEntry.setObject2(BSON("shardCollection" << nss.toString_forTest() << "key"
                                                 << BSON("x" << 1) << "unique" << false));
    oplogEntry.setOpTime(repl::OpTime(kDefaultTs, 0));
    oplogEntry.setWallClockTime(Date_t());

    // Expect fields from the oplog entry to be present in the 'operationDescription' field.
    Document expectedDoc{
        {DocumentSourceChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         Value(),
                         Document{{"key", Document{{"x", 1}}}, {"unique", false}},
                         DocumentSourceChangeStream::kShardCollectionOpType)},
        {DocumentSourceChangeStream::kOperationTypeField,
         DocumentSourceChangeStream::kShardCollectionOpType},
        {DocumentSourceChangeStream::kClusterTimeField, kDefaultTs},
        {DocumentSourceChangeStream::kWallTimeField, Date_t()},
        {DocumentSourceChangeStream::kNamespaceField,
         Document{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DocumentSourceChangeStream::kOperationDescriptionField,
         Document{{"key", Document{{"x", 1}}}, {"unique", false}}},
    };

    repl::OplogEntry immutableEntry(oplogEntry.toBSON());
    ASSERT_DOCUMENT_EQ(applyTransformation(immutableEntry, nss), expectedDoc);
}

TEST(
    ChangeStreamEventTransformTest,
    Given_NoopOplogEntryWhichIsNotBuiltIn_When_CallingTransform_Then_OperationDescriptionIsPresent) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "testDB.coll.name");
    auto serviceContext = std::make_unique<QueryTestServiceContext>();
    auto opCtx = serviceContext->makeOperationContext();
    auto oplogEntry = buildMovePrimaryOplogEntry(
        opCtx.get(), nss.dbName(), ShardId("oldPrimary"), ShardId("newPrimary"));
    auto opDescription = Document{{
        {"from"_sd, "oldPrimary"_sd},
        {"to"_sd, "newPrimary"_sd},
    }};

    Document expectedDoc{
        {DocumentSourceChangeStream::kIdField,
         makeResumeToken(kDefaultTs, Value(), opDescription, "movePrimary")},
        {DocumentSourceChangeStream::kOperationTypeField, "movePrimary"_sd},
        {DocumentSourceChangeStream::kClusterTimeField, kDefaultTs},
        {DocumentSourceChangeStream::kWallTimeField, Date_t()},
        {DocumentSourceChangeStream::kNamespaceField, Document{{"db", nss.db_forTest()}}},
        {DocumentSourceChangeStream::kOperationDescriptionField, opDescription}};

    repl::OplogEntry immutableEntry(oplogEntry.toBSON());
    ASSERT_DOCUMENT_EQ(applyTransformation(immutableEntry, nss, {"movePrimary"}), expectedDoc);
}

}  // namespace
}  // namespace mongo
