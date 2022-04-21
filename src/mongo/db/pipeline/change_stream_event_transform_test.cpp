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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/change_stream_event_transform.h"
#include "mongo/db/pipeline/change_stream_rewrite_helpers.h"
#include "mongo/db/pipeline/change_stream_test_helpers.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {
using namespace change_stream_test_helper;

Document applyTransformation(const repl::OplogEntry& oplogEntry, NamespaceString ns = nss) {
    const auto oplogDoc = Document(oplogEntry.getEntry().toBSON());
    DocumentSourceChangeStreamSpec spec;
    spec.setStartAtOperationTime(kDefaultTs);
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
         Document{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DocumentSourceChangeStream::kDocumentKeyField, documentKey},
        {
            "updateDescription",
            Document{{"updatedFields", Document{{"y", 2}}},
                     {"removedFields", std::vector<Value>()},
                     {"truncatedArrays", std::vector<Value>()}},
        },
    };

    ASSERT_DOCUMENT_EQ(applyTransformation(updateField), expectedUpdateField);
}

TEST(ChangeStreamEventTransformTest, TestCreateViewTransform) {
    const NamespaceString systemViewNss("viewDB.system.views");
    const NamespaceString viewNss("viewDB.view.name");
    const auto viewPipeline =
        Value(fromjson("[{$match: {field: 'value'}}, {$project: {field: 1}}]"));
    const auto opDescription = Document{{"viewOn", "baseColl"_sd}, {"pipeline", viewPipeline}};
    auto oplogEntry = makeOplogEntry(repl::OpTypeEnum::kInsert,  // op type
                                     systemViewNss,              // namespace
                                     BSON("_id" << viewNss.toString() << "viewOn"
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
         Document{{"db", viewNss.db()}, {"coll", viewNss.coll()}}},
        {DocumentSourceChangeStream::kOperationDescriptionField, opDescription}};

    ASSERT_DOCUMENT_EQ(applyTransformation(oplogEntry,
                                           NamespaceString::makeCollectionlessAggregateNSS(
                                               TenantDatabaseName(boost::none, "viewDB"))),
                       expectedDoc);
}

TEST(ChangeStreamEventTransformTest, TestCreateViewOnSingleCollection) {
    const NamespaceString systemViewNss("viewDB.system.views");
    const NamespaceString viewNss("viewDB.view.name");
    const auto viewPipeline =
        Value(fromjson("[{$match: {field: 'value'}}, {$project: {field: 1}}]"));
    const auto document = BSON("_id" << viewNss.toString() << "viewOn"
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
         Document{{"db", systemViewNss.db()}, {"coll", systemViewNss.coll()}}},
        {DocumentSourceChangeStream::kDocumentKeyField, documentKey}};

    ASSERT_DOCUMENT_EQ(applyTransformation(oplogEntry), expectedDoc);
}

}  // namespace
}  // namespace mongo
