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
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/change_stream_event_transform.h"
#include "mongo/db/pipeline/change_stream_rewrite_helpers.h"
#include "mongo/db/pipeline/change_stream_test_helpers.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/idl/server_parameter_test_util.h"
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
                     {"truncatedArrays", std::vector<Value>()},
                     {"disambiguatedPaths", Document{}}},
        },
    };

    ASSERT_DOCUMENT_EQ(applyTransformation(updateField), expectedUpdateField);
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

    ASSERT_DOCUMENT_EQ(
        applyTransformation(oplogEntry,
                            NamespaceString::makeCollectionlessAggregateNSS(
                                DatabaseName::createDatabaseName_forTest(boost::none, "viewDB"))),
        expectedDoc);
}

TEST(ChangeStreamEventTransformTest, TestCreateViewOnSingleCollection) {
    const NamespaceString systemViewNss = NamespaceString::makeSystemDotViewsNamespace(
        DatabaseName::createDatabaseName_forTest(boost::none, "viewDB"));
    const NamespaceString viewNss =
        NamespaceString::createNamespaceString_forTest(boost::none, "viewDB.view.name");
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

TEST(ChangeStreamEventTransformTest, TestUpdateTransformWithTenantId) {
    // Turn on multitenancySupport, but not featureFlagRequireTenantId. We expect the tenantId to be
    // part of the 'ns' field in the oplog entry, but it should not be a part of the db name in the
    // change event.
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    const auto documentKey = Document{{"x", 1}, {"y", 1}};
    const auto tenantId = TenantId(OID::gen());
    NamespaceString nssWithTenant = NamespaceString::createNamespaceString_forTest(
        tenantId, "unittests.serverless_change_stream");

    auto updateField =
        makeOplogEntry(repl::OpTypeEnum::kUpdate,                                 // op type
                       nssWithTenant,                                             // namespace
                       BSON("$v" << 2 << "diff" << BSON("u" << BSON("y" << 2))),  // o
                       testUuid(),                                                // uuid
                       boost::none,                                               // fromMigrate
                       documentKey.toBson()                                       // o2
        );

    Document expectedNamespace =
        Document{{"db", nssWithTenant.dbName().db()}, {"coll", nssWithTenant.coll()}};

    auto changeStreamDoc = applyTransformation(updateField, nssWithTenant);
    auto outputNs = changeStreamDoc[DocumentSourceChangeStream::kNamespaceField].getDocument();

    ASSERT_DOCUMENT_EQ(outputNs, expectedNamespace);

    // Now set featureFlagRequireTenantId, so we expect the tenantId to be in a separate "tid" field
    // in the oplog entry. It should still not be a part of the db name in the change event.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto oplogEntry = makeOplogEntry(repl::OpTypeEnum::kUpdate,  // op type
                                     nssWithTenant,              // namespace
                                     BSON("$v" << 2 << "diff" << BSON("u" << BSON("y" << 2))),  // o
                                     testUuid(),           // uuid
                                     boost::none,          // fromMigrate
                                     documentKey.toBson()  // o2
    );

    changeStreamDoc = applyTransformation(oplogEntry, nssWithTenant);
    outputNs = changeStreamDoc[DocumentSourceChangeStream::kNamespaceField].getDocument();

    ASSERT_DOCUMENT_EQ(outputNs, expectedNamespace);
}

TEST(ChangeStreamEventTransformTest, TestRenameTransformWithTenantId) {
    // Turn on multitenancySupport, but not featureFlagRequireTenantId. We expect the tenantId to be
    // part of the 'ns' field in the oplog entry, but it should not be a part of the db name in the
    // change event.
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    const auto tenantId = TenantId(OID::gen());
    NamespaceString renameFrom = NamespaceString::createNamespaceString_forTest(
        tenantId, "unittests.serverless_change_stream");
    NamespaceString renameTo =
        NamespaceString::createNamespaceString_forTest(tenantId, "unittests.rename_coll");

    auto renameField = makeOplogEntry(
        repl::OpTypeEnum::kCommand,  // op type
        renameFrom.getCommandNS(),   // namespace
        BSON("renameCollection" << renameFrom.toString() << "to" << renameTo.toString()),  // o
        testUuid()                                                                         // uuid
    );

    Document expectedDoc{{DocumentSourceChangeStream::kNamespaceField,
                          Document{{"db", renameFrom.dbName().db()}, {"coll", renameFrom.coll()}}},
                         {DocumentSourceChangeStream::kRenameTargetNssField,
                          Document{{"db", renameTo.dbName().db()}, {"coll", renameTo.coll()}}},
                         {DocumentSourceChangeStream::kOperationDescriptionField,
                          Document{BSON("to" << BSON("db" << renameTo.dbName().db() << "coll"
                                                          << renameTo.coll()))}}};

    auto changeStreamDoc = applyTransformation(renameField, renameFrom);
    auto renameDoc = Document{
        {DocumentSourceChangeStream::kNamespaceField,
         changeStreamDoc.getField(DocumentSourceChangeStream::kNamespaceField)},
        {DocumentSourceChangeStream::kRenameTargetNssField,
         changeStreamDoc.getField(DocumentSourceChangeStream::kRenameTargetNssField)},
        {DocumentSourceChangeStream::kOperationDescriptionField,
         changeStreamDoc.getField(DocumentSourceChangeStream::kOperationDescriptionField)}};

    ASSERT_DOCUMENT_EQ(renameDoc, expectedDoc);

    // Now set featureFlagRequireTenantId, so we expect the tenantId to be in a separate "tid" field
    // in the oplog entry. It should still not be a part of the db name in the change event.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto oplogEntry = makeOplogEntry(
        repl::OpTypeEnum::kCommand,  // op type
        renameFrom.getCommandNS(),   // namespace
        BSON("renameCollection" << renameFrom.toString() << "to" << renameTo.toString()),  // o
        testUuid()                                                                         // uuid
    );

    changeStreamDoc = applyTransformation(oplogEntry, renameFrom);
    renameDoc = Document{
        {DocumentSourceChangeStream::kNamespaceField,
         changeStreamDoc.getField(DocumentSourceChangeStream::kNamespaceField)},
        {DocumentSourceChangeStream::kRenameTargetNssField,
         changeStreamDoc.getField(DocumentSourceChangeStream::kRenameTargetNssField)},
        {DocumentSourceChangeStream::kOperationDescriptionField,
         changeStreamDoc.getField(DocumentSourceChangeStream::kOperationDescriptionField)}};

    ASSERT_DOCUMENT_EQ(renameDoc, expectedDoc);
}

TEST(ChangeStreamEventTransformTest, TestDropDatabaseTransformWithTenantId) {
    // Turn on multitenancySupport, but not featureFlagRequireTenantId. We expect the tenantId to be
    // part of the 'ns' field in the oplog entry, but it should not be a part of the db name in the
    // change event.
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    const auto tenantId = TenantId(OID::gen());
    NamespaceString dbToDrop =
        NamespaceString::createNamespaceString_forTest(tenantId, "unittests");

    auto dropDbField = makeOplogEntry(repl::OpTypeEnum::kCommand,  // op type
                                      dbToDrop.getCommandNS(),     // namespace
                                      BSON("dropDatabase" << 1),   // o
                                      testUuid()                   // uuid
    );

    Document expectedNamespace = Document{{"db", dbToDrop.dbName().db()}};

    auto changeStreamDoc = applyTransformation(dropDbField, dbToDrop);
    auto outputNs = changeStreamDoc[DocumentSourceChangeStream::kNamespaceField].getDocument();

    ASSERT_DOCUMENT_EQ(outputNs, expectedNamespace);

    // Now set featureFlagRequireTenantId, so we expect the tenantId to be in a separate "tid" field
    // in the oplog entry. It should still not be a part of the db name in the change event.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto oplogEntry = makeOplogEntry(repl::OpTypeEnum::kCommand,  // op type
                                     dbToDrop.getCommandNS(),     // namespace
                                     BSON("dropDatabase" << 1),   // o
                                     testUuid()                   // uuid
    );

    changeStreamDoc = applyTransformation(oplogEntry, dbToDrop);
    outputNs = changeStreamDoc[DocumentSourceChangeStream::kNamespaceField].getDocument();

    ASSERT_DOCUMENT_EQ(outputNs, expectedNamespace);
}

TEST(ChangeStreamEventTransformTest, TestCreateTransformWithTenantId) {
    // Turn on multitenancySupport, but not featureFlagRequireTenantId. We expect the tenantId to be
    // part of the 'ns' field in the oplog entry, but it should not be a part of the db name in the
    // change event.
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    const auto tenantId = TenantId(OID::gen());
    NamespaceString nssWithTenant = NamespaceString::createNamespaceString_forTest(
        tenantId, "unittests.serverless_change_stream");

    auto createField = makeOplogEntry(repl::OpTypeEnum::kCommand,              // op type
                                      nssWithTenant.getCommandNS(),            // namespace
                                      BSON("create" << nssWithTenant.coll()),  // o
                                      testUuid()                               // uuid
    );

    Document expectedNamespace =
        Document{{"db", nssWithTenant.dbName().db()}, {"coll", nssWithTenant.coll()}};

    auto changeStreamDoc = applyTransformation(createField, nssWithTenant);
    auto outputNs = changeStreamDoc[DocumentSourceChangeStream::kNamespaceField].getDocument();

    ASSERT_DOCUMENT_EQ(outputNs, expectedNamespace);

    // Now set featureFlagRequireTenantId, so we expect the tenantId to be in a separate "tid" field
    // in the oplog entry. It should still not be a part of the db name in the change event.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto oplogEntry = makeOplogEntry(repl::OpTypeEnum::kCommand,              // op type
                                     nssWithTenant.getCommandNS(),            // namespace
                                     BSON("create" << nssWithTenant.coll()),  // o
                                     testUuid()                               // uuid
    );

    changeStreamDoc = applyTransformation(oplogEntry, nssWithTenant);
    outputNs = changeStreamDoc[DocumentSourceChangeStream::kNamespaceField].getDocument();

    ASSERT_DOCUMENT_EQ(outputNs, expectedNamespace);
}

TEST(ChangeStreamEventTransformTest, TestCreateViewTransformWithTenantId) {
    // Turn on multitenancySupport, but not featureFlagRequireTenantId. We expect the tenantId to be
    // part of the 'ns' field in the oplog entry, but it should not be a part of the db name in the
    // change event.
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    const auto tenantId = TenantId(OID::gen());

    const NamespaceString systemViewNss = NamespaceString::makeSystemDotViewsNamespace(
        DatabaseName::createDatabaseName_forTest(tenantId, "viewDB"));
    const NamespaceString viewNss =
        NamespaceString::createNamespaceString_forTest(tenantId, "viewDB.view.name");
    const auto viewPipeline =
        Value(fromjson("[{$match: {field: 'value'}}, {$project: {field: 1}}]"));
    const auto opDescription = Document{{"viewOn", "baseColl"_sd}, {"pipeline", viewPipeline}};
    auto createView = makeOplogEntry(repl::OpTypeEnum::kInsert,  // op type
                                     systemViewNss,              // namespace
                                     BSON("_id" << viewNss.toString() << "viewOn"
                                                << "baseColl"
                                                << "pipeline" << viewPipeline),  // o
                                     testUuid());                                // uuid

    Document expectedNamespace = Document{{"db", viewNss.dbName().db()}, {"coll", viewNss.coll()}};

    auto changeStreamDoc = applyTransformation(
        createView, NamespaceString::makeCollectionlessAggregateNSS(viewNss.dbName()));
    auto outputNs = changeStreamDoc[DocumentSourceChangeStream::kNamespaceField].getDocument();

    ASSERT_DOCUMENT_EQ(outputNs, expectedNamespace);

    // Now set featureFlagRequireTenantId, so we expect the tenantId to be in a separate "tid" field
    // in the oplog entry. It should still not be a part of the db name in the change event.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto oplogEntry = makeOplogEntry(repl::OpTypeEnum::kInsert,  // op type
                                     systemViewNss,              // namespace
                                     BSON("_id" << viewNss.toString() << "viewOn"
                                                << "baseColl"
                                                << "pipeline" << viewPipeline),  // o
                                     testUuid());

    changeStreamDoc = applyTransformation(
        oplogEntry, NamespaceString::makeCollectionlessAggregateNSS(viewNss.dbName()));
    outputNs = changeStreamDoc[DocumentSourceChangeStream::kNamespaceField].getDocument();

    ASSERT_DOCUMENT_EQ(outputNs, expectedNamespace);
}

}  // namespace
}  // namespace mongo
