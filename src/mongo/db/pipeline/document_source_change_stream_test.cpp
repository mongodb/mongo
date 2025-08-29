/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_change_stream.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/change_stream_transform_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/change_stream_stage_test_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_check_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_inject_control_events.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_split_large_event.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <array>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <set>
#include <vector>

// #include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
using namespace change_stream_test_helper;

using boost::intrusive_ptr;
using repl::OplogEntry;
using repl::OpTypeEnum;

using D = Document;
using V = Value;

using DSChangeStream = DocumentSourceChangeStream;

// Deterministic values used for testing
const UUID testConstUuid = UUID::parse("6948DF80-14BD-4E04-8842-7668D9C001F5").getValue();

void assertCommitTimestamp(bool showCommitTimestamp, const Document& doc) {
    if (showCommitTimestamp) {
        ASSERT_EQ(kDefaultCommitTs, doc[DSChangeStream::kCommitTimestampField].getTimestamp());
    } else {
        ASSERT_TRUE(doc[DSChangeStream::kCommitTimestampField].missing());
    }
}

TEST_F(ChangeStreamStageTest, ShouldRejectNonObjectArg) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << "invalid").firstElement(), expCtx),
                       AssertionException,
                       50808);

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << 12345).firstElement(), expCtx),
                       AssertionException,
                       50808);
}

TEST_F(ChangeStreamStageTest, ShouldRejectUnrecognizedOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("unexpected" << 4)).firstElement(), expCtx),
        AssertionException,
        40415);

    // In older versions this option was accepted.
    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << BSON(
                                    "$_resumeAfterClusterTime" << BSON("ts" << Timestamp(0, 1))))
                               .firstElement(),
                           expCtx),
                       AssertionException,
                       40415);
}

TEST_F(ChangeStreamStageTest, ShouldRejectNonStringFullDocumentOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("fullDocument" << true)).firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::TypeMismatch);
}

TEST_F(ChangeStreamStageTest, ShouldRejectUnrecognizedFullDocumentOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(BSON(DSChangeStream::kStageName << BSON(
                                                               "fullDocument" << "unrecognized"))
                                                          .firstElement(),
                                                      expCtx),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAtOperationTimeAndResumeAfterOptions) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the collection catalog so the resume token is valid.
    {
        Lock::GlobalWrite lk(expCtx->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
        CollectionCatalog::write(expCtx->getOperationContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(
                expCtx->getOperationContext(), std::move(collection), /*ts=*/boost::none);
        });
    }

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << BSON(
                                    "resumeAfter" << makeResumeToken(kDefaultTs,
                                                                     testUuid(),
                                                                     BSON("x" << 2 << "_id" << 1),
                                                                     DSChangeStream::kInsertOpType)
                                                  << "startAtOperationTime" << kDefaultTs))
                               .firstElement(),
                           expCtx),
                       AssertionException,
                       40674);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAfterAndResumeAfterOptions) {
    auto expCtx = getExpCtx();
    auto opCtx = expCtx->getOperationContext();

    // Need to put the collection in the collection catalog so the resume token is validcollection
    {
        Lock::GlobalWrite lk(opCtx);
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.registerCollection(opCtx, std::move(collection), /*ts=*/boost::none);
        });
    }

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName
                 << BSON("resumeAfter" << makeResumeToken(kDefaultTs,
                                                          testUuid(),
                                                          BSON("x" << 2 << "_id" << 1),
                                                          DSChangeStream::kInsertOpType)
                                       << "startAfter"
                                       << makeResumeToken(kDefaultTs,
                                                          testUuid(),
                                                          BSON("x" << 2 << "_id" << 1),
                                                          DSChangeStream::kInsertOpType)))
                .firstElement(),
            expCtx),
        AssertionException,
        50865);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAtOperationTimeAndStartAfterOptions) {
    auto expCtx = getExpCtx();
    auto opCtx = expCtx->getOperationContext();

    // Need to put the collection in the collection catalog so the resume token is valid.
    {
        Lock::GlobalWrite lk(opCtx);
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.registerCollection(opCtx, std::move(collection), /*ts=*/boost::none);
        });
    }

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName << BSON(
                                    "startAfter" << makeResumeToken(kDefaultTs,
                                                                    testUuid(),
                                                                    BSON("x" << 2 << "_id" << 1),
                                                                    DSChangeStream::kInsertOpType)
                                                 << "startAtOperationTime" << kDefaultTs))
                               .firstElement(),
                           expCtx),
                       AssertionException,
                       40674);
}

TEST_F(ChangeStreamStageTest, ShouldRejectResumeAfterWithResumeTokenMissingUUID) {
    auto expCtx = getExpCtx();
    auto opCtx = expCtx->getOperationContext();

    // Need to put the collection in the collection catalog so the resume token is valid.
    {
        Lock::GlobalWrite lk(opCtx);
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.registerCollection(opCtx, std::move(collection), /*ts=*/boost::none);
        });
    }

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName
                 << BSON("resumeAfter" << makeResumeToken(
                             kDefaultTs, Value(), Value(), DSChangeStream::kInsertOpType)))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageTestNoSetup, FailsWithNoReplicationCoordinator) {
    const auto spec = fromjson("{$changeStream: {}}");

    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       40573);
}

TEST_F(ChangeStreamStageTest, CannotCreateStageForSystemCollection) {
    auto expressionContext = getExpCtx();
    expressionContext->setNamespaceString(
        NamespaceString::createNamespaceString_forTest("db", "system.namespace"));
    const auto spec = fromjson("{$changeStream: {allowToRunOnSystemNS: false}}");
    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ChangeStreamStageTest, CanCreateStageForSystemCollectionWhenAllowToRunOnSystemNSIsTrue) {
    auto expressionContext = getExpCtx();
    expressionContext->setNamespaceString(
        NamespaceString::createNamespaceString_forTest("db", "system.namespace"));
    expressionContext->setInRouter(false);
    const auto spec = fromjson("{$changeStream: {allowToRunOnSystemNS: true}}");
    DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx());
}

TEST_F(ChangeStreamStageTest,
       CannotCreateStageForSystemCollectionWhenAllowToRunOnSystemNSIsTrueAndInMongos) {
    auto expressionContext = getExpCtx();
    expressionContext->setNamespaceString(
        NamespaceString::createNamespaceString_forTest("db", "system.namespace"));
    expressionContext->setInRouter(true);
    const auto spec = fromjson("{$changeStream: {allowToRunOnSystemNS: true}}");
    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ChangeStreamStageTest, CanCreateStageForNonSystemCollection) {
    const auto spec = fromjson("{$changeStream: {}}");
    DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx());
}

TEST_F(ChangeStreamStageTest, ShowMigrationsFailsOnMongos) {
    auto expCtx = getExpCtx();
    expCtx->setInRouter(true);
    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(spec.firstElement(), expCtx), AssertionException, 31123);
}

TEST_F(ChangeStreamStageTest, ChangeStreamRegexEscape) {
    ASSERT_EQ(""_sd, DocumentSourceChangeStream::regexEscapeNsForChangeStream(""));
    ASSERT_EQ(" "_sd, DocumentSourceChangeStream::regexEscapeNsForChangeStream(" "));
    ASSERT_EQ("foo bar"_sd, DocumentSourceChangeStream::regexEscapeNsForChangeStream("foo bar"));
    ASSERT_EQ("qux-QUX "_sd, DocumentSourceChangeStream::regexEscapeNsForChangeStream("qux-QUX "));
    ASSERT_EQ("\\^foo\\*bar\\$"_sd,
              DocumentSourceChangeStream::regexEscapeNsForChangeStream("^foo*bar$"));
    ASSERT_EQ("\\*\\+\\|\\(\\)\\^\\?\\[\\]\\.\\/\\\\\\$"_sd,
              DocumentSourceChangeStream::regexEscapeNsForChangeStream("*+|()^?[]./\\$"));
}

TEST_F(ChangeStreamStageTest, ChangeStreamBuiltInRegexesSingleCollection) {
    auto expCtx = getExpCtx();

    auto nss = NamespaceString::createNamespaceString_forTest("unittest"_sd, "someCollection"_sd);
    expCtx->setNamespaceString(nss);

    ASSERT_EQ("^unittest\\.someCollection$",
              DocumentSourceChangeStream::getNsRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(BSON("" << BSONRegEx("^unittest\\.someCollection$")),
                      DocumentSourceChangeStream::getNsMatchObjForChangeStream(expCtx));

    ASSERT_EQ("^someCollection$", DocumentSourceChangeStream::getCollRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(BSON("" << BSONRegEx("^someCollection$")),
                      DocumentSourceChangeStream::getCollMatchObjForChangeStream(expCtx));

    ASSERT_EQ("^unittest\\.\\$cmd$",
              DocumentSourceChangeStream::getCmdNsRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(BSON("" << BSONRegEx("^unittest\\.\\$cmd$")),
                      DocumentSourceChangeStream::getCmdNsMatchObjForChangeStream(expCtx));
}

TEST_F(ChangeStreamStageTest, ChangeStreamBuiltInRegexesSingleDatabase) {
    auto expCtx = getExpCtx();

    auto nss = NamespaceString::makeCollectionlessAggregateNSS(
        NamespaceString::createNamespaceString_forTest("unittest"_sd).dbName());
    expCtx->setNamespaceString(nss);

    ASSERT_EQ(fmt::format("^unittest\\.{}", DocumentSourceChangeStream::kRegexAllCollections),
              DocumentSourceChangeStream::getNsRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(
        BSON("" << BSONRegEx(
                 fmt::format("^unittest\\.{}", DocumentSourceChangeStream::kRegexAllCollections))),
        DocumentSourceChangeStream::getNsMatchObjForChangeStream(expCtx));

    ASSERT_EQ("^unittest\\.system\\.views$",
              DocumentSourceChangeStream::getViewNsRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(BSON("" << BSONRegEx("^unittest\\.system\\.views$")),
                      DocumentSourceChangeStream::getViewNsMatchObjForChangeStream(expCtx));

    ASSERT_EQ(fmt::format("^{}", DocumentSourceChangeStream::kRegexAllCollections),
              DocumentSourceChangeStream::getCollRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(
        BSON("" << BSONRegEx(fmt::format("^{}", DocumentSourceChangeStream::kRegexAllCollections))),
        DocumentSourceChangeStream::getCollMatchObjForChangeStream(expCtx));

    ASSERT_EQ("^unittest\\.\\$cmd$",
              DocumentSourceChangeStream::getCmdNsRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(BSON("" << BSONRegEx("^unittest\\.\\$cmd$")),
                      DocumentSourceChangeStream::getCmdNsMatchObjForChangeStream(expCtx));
}

TEST_F(ChangeStreamStageTest, ChangeStreamBuiltInRegexesWholeCluster) {
    auto expCtx = getExpCtx();

    auto nss = NamespaceString::createNamespaceString_forTest("admin"_sd);
    expCtx->setNamespaceString(nss);

    ASSERT_EQ(fmt::format("{}\\.{}",
                          DocumentSourceChangeStream::kRegexAllDBs,
                          DocumentSourceChangeStream::kRegexAllCollections),
              DocumentSourceChangeStream::getNsRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(
        BSON("" << BSONRegEx(fmt::format("{}\\.{}",
                                         DocumentSourceChangeStream::kRegexAllDBs,
                                         DocumentSourceChangeStream::kRegexAllCollections))),
        DocumentSourceChangeStream::getNsMatchObjForChangeStream(expCtx));

    ASSERT_EQ(fmt::format("{}\\.system\\.views$", DocumentSourceChangeStream::kRegexAllDBs),
              DocumentSourceChangeStream::getViewNsRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(
        BSON("" << BSONRegEx(fmt::format(
                 "{}\\.system\\.views$",
                 DocumentSourceChangeStream::DocumentSourceChangeStream::kRegexAllDBs))),
        DocumentSourceChangeStream::getViewNsMatchObjForChangeStream(expCtx));

    ASSERT_EQ(fmt::format("^{}", DocumentSourceChangeStream::kRegexAllCollections),
              DocumentSourceChangeStream::getCollRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(
        BSON("" << BSONRegEx(fmt::format("^{}", DocumentSourceChangeStream::kRegexAllCollections))),
        DocumentSourceChangeStream::getCollMatchObjForChangeStream(expCtx));

    ASSERT_EQ(fmt::format("{}\\.{}",
                          DocumentSourceChangeStream::kRegexAllDBs,
                          DocumentSourceChangeStream::kRegexCmdColl),
              DocumentSourceChangeStream::getCmdNsRegexForChangeStream(expCtx));
    ASSERT_BSONOBJ_EQ(BSON("" << BSONRegEx(fmt::format("{}\\.{}",
                                                       DocumentSourceChangeStream::kRegexAllDBs,
                                                       DocumentSourceChangeStream::kRegexCmdColl))),
                      DocumentSourceChangeStream::getCmdNsMatchObjForChangeStream(expCtx));
}

TEST_F(ChangeStreamStageTest, CreatingChangeStreamSucceedsWithValidVersions) {
    // Versions "v1", "v2" are supported.
    std::array<boost::optional<StringData>, 3> versions = {boost::none, "v1"_sd, "v2"_sd};

    for (auto version : versions) {
        BSONObj spec;
        if (version.has_value()) {
            spec = BSON("$changeStream" << BSON("version" << *version));
        } else {
            spec = BSON("$changeStream" << BSONObj());
        }

        auto pipeline = DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());
        ASSERT_FALSE(pipeline.empty());

        bool found = false;
        for (auto& stage : pipeline) {
            if (stage->getSourceName() == DocumentSourceChangeStreamTransform::kStageName) {
                // Serialize the stage to BSON and read back the "version" field.
                std::vector<Value> serialization;
                stage->serializeToArray(serialization);

                ASSERT_EQ(serialization.size(), 1UL);
                ASSERT_EQ(serialization[0].getType(), BSONType::object);

                if (version.has_value()) {
                    ASSERT_EQ(version,
                              serialization[0]
                                  .getDocument()
                                  .getField(DocumentSourceChangeStreamTransform::kStageName)
                                  .getDocument()
                                  .getField("version"_sd)
                                  .getStringData());
                } else {
                    ASSERT_TRUE(serialization[0]
                                    .getDocument()
                                    .getField(DocumentSourceChangeStreamTransform::kStageName)
                                    .getDocument()
                                    .getField("version"_sd)
                                    .missing());
                }
                found = true;
            }
        }
        ASSERT_TRUE(found);
    }
}

TEST_F(ChangeStreamStageTest, CreatingChangeStreamSucceedsWithoutAnyVersion) {
    // Do not specify "version" at all.
    auto spec = BSON("$changeStream" << BSONObj());

    auto pipeline = DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_FALSE(pipeline.empty());
}

TEST_F(ChangeStreamStageTest, CreatingChangestreamFailsWithInvalidVersions) {
    // Test a bunch of unsupported versions.
    for (auto version : {"v3", "v0", "V1", "", "1", "2"}) {
        auto spec = BSON("$changeStream" << BSON("version" << version));

        ASSERT_THROWS_CODE(DSChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                           AssertionException,
                           ErrorCodes::BadValue);
    }
}

// Test different values for 'ignoreRemovedShards' when creating a change stream.
TEST_F(ChangeStreamStageTest, SetIgnoreRemovedShards) {
    std::array<boost::optional<bool>, 3> values = {boost::none, true, false};

    for (auto ignoreRemovedShards : values) {
        BSONObj spec;
        if (ignoreRemovedShards.has_value()) {
            spec = BSON("$changeStream" << BSON("ignoreRemovedShards" << *ignoreRemovedShards));
        } else {
            spec = BSON("$changeStream" << BSONObj());
        }

        auto pipeline = DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());
        ASSERT_FALSE(pipeline.empty());

        bool found = false;
        for (auto& stage : pipeline) {
            if (stage->getSourceName() == DocumentSourceChangeStreamTransform::kStageName) {
                // Serialize the stage to BSON and read back the "version" field.
                std::vector<Value> serialization;
                stage->serializeToArray(serialization);

                ASSERT_EQ(serialization.size(), 1UL);
                ASSERT_EQ(serialization[0].getType(), BSONType::object);

                if (ignoreRemovedShards.has_value()) {
                    ASSERT_EQ(*ignoreRemovedShards,
                              serialization[0]
                                  .getDocument()
                                  .getField(DocumentSourceChangeStreamTransform::kStageName)
                                  .getDocument()
                                  .getField("ignoreRemovedShards"_sd)
                                  .getBool());
                } else {
                    ASSERT_TRUE(serialization[0]
                                    .getDocument()
                                    .getField(DocumentSourceChangeStreamTransform::kStageName)
                                    .getDocument()
                                    .getField("ignoreRemovedShards"_sd)
                                    .missing());
                }
                found = true;

                // Also test conversion from optional<bool> to 'ChangeStreamReadMode' enum value.
                ChangeStreamReadMode readMode =
                    fromIgnoreRemovedShardsParameter(ignoreRemovedShards);
                ASSERT_EQ(ignoreRemovedShards.value_or(false)
                              ? ChangeStreamReadMode::kIgnoreRemovedShards
                              : ChangeStreamReadMode::kStrict,
                          readMode);
            }
        }
        ASSERT_TRUE(found);
    }
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyXAndId) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,            // op type
                                 nss,                            // namespace
                                 BSON("_id" << 1 << "x" << 2),   // o
                                 testUuid(),                     // uuid
                                 boost::none,                    // fromMigrate
                                 BSON("x" << 2 << "_id" << 1));  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1), DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert);
    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto insert2 = makeOplogEntry(insert.getOpType(),    // op type
                                  insert.getNss(),       // namespace
                                  insert.getObject(),    // o
                                  insert.getUuid(),      // uuid
                                  fromMigrate,           // fromMigrate
                                  insert.getObject2());  // o2
    checkTransformation(insert2, expectedInsert);
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyIdAndX) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,            // op type
                                 nss,                            // namespace
                                 BSON("x" << 2 << "_id" << 1),   // o
                                 testUuid(),                     // uuid
                                 boost::none,                    // fromMigrate
                                 BSON("_id" << 1 << "x" << 2));  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), BSON("_id" << 1 << "x" << 2), DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"x", 2}, {"_id", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},  // _id first
    };
    checkTransformation(insert, expectedInsert);
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyJustId) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("_id" << 1 << "x" << 2),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 BSON("_id" << 1));             // o2

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1), DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(insert, expectedInsert);
}

TEST_F(ChangeStreamStageTest, TransformInsertFromMigrate) {
    bool fromMigrate = true;
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("_id" << 1 << "x" << 1),  // o
                                 boost::none,                   // uuid
                                 fromMigrate,                   // fromMigrate
                                 boost::none);                  // o2

    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformInsertFromMigrateShowMigrations) {
    bool fromMigrate = true;
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,            // op type
                                 nss,                            // namespace
                                 BSON("x" << 2 << "_id" << 1),   // o
                                 testUuid(),                     // uuid
                                 fromMigrate,                    // fromMigrate
                                 BSON("_id" << 1 << "x" << 2));  // o2

    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");
    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), BSON("_id" << 1 << "x" << 2), DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"x", 2}, {"_id", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},  // _id first
    };
    checkTransformation(insert, expectedInsert, spec);
}

TEST_F(ChangeStreamStageTest, TransformUpdateFields) {
    BSONObj diff = BSON("u" << BSON("y" << 1));
    BSONObj o = BSON("diff" << diff << "$v" << 2);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    const auto expectedUpdateField =
        makeExpectedUpdateEvent(kDefaultTs,
                                nss,
                                o2,
                                D{{"updatedFields", D{{"y", 1}}},
                                  {"removedFields", std::vector<V>()},
                                  {"truncatedArrays", std::vector<V>()}});

    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageTest, TransformUpdateFieldsShowExpandedEvents) {
    BSONObj diff = BSON("u" << BSON("y" << 1));
    BSONObj o = BSON("diff" << diff << "$v" << 2);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    const auto expectedUpdateField =
        makeExpectedUpdateEvent(kDefaultTs,
                                nss,
                                o2,
                                D{{"updatedFields", D{{"y", 1}}},
                                  {"removedFields", std::vector<V>()},
                                  {"truncatedArrays", std::vector<V>()},
                                  {"disambiguatedPaths", D{}}},
                                true);
    checkTransformation(updateField, expectedUpdateField, kShowExpandedEventsSpec);
}

TEST_F(ChangeStreamStageTest, TransformSimpleDeltaOplogUpdatedFields) {
    BSONObj diff = BSON("u" << BSON("a" << 1 << "b"
                                        << "updated"));

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"a", 1}, {"b", "updated"_sd}}},
                           {"removedFields", std::vector<V>{}},
                           {"truncatedArrays", std::vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformSimpleDeltaOplogInsertFields) {
    BSONObj diff = BSON("i" << BSON("a" << 1 << "b"
                                        << "updated"));

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"a", 1}, {"b", "updated"_sd}}},
                           {"removedFields", std::vector<V>{}},
                           {"truncatedArrays", std::vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformSimpleDeltaOplogRemovedFields) {
    BSONObj diff = BSON("d" << BSON("a" << false << "b" << false));

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{}},
                           {"removedFields", std::vector<V>{V("a"_sd), V("b"_sd)}},
                           {"truncatedArrays", std::vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformComplexDeltaOplog) {
    BSONObj diff = fromjson(
        "{"
        "   d: { a: false, b: false },"
        "   u: { c: 1, d: \"updated\" },"
        "   i: { e: 2, f: 3 }"
        "}");

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"c", 1}, {"d", "updated"_sd}, {"e", 2}, {"f", 3}}},
                           {"removedFields", std::vector<V>{V("a"_sd), V("b"_sd)}},
                           {"truncatedArrays", std::vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogSubObjectDiff) {
    BSONObj diff = fromjson(
        "{"
        "   u: { c: 1, d: \"updated\" },"
        "   ssubObj: {"
        "           d: { a: false, b: false },"
        "           u: { c: 1, d: \"updated\" }"
        "   }"
        "}");

    runUpdateV2OplogTest(
        diff,
        D{{"updatedFields",
           D{{"c", 1}, {"d", "updated"_sd}, {"subObj.c", 1}, {"subObj.d", "updated"_sd}}},
          {"removedFields", std::vector<V>{V("subObj.a"_sd), V("subObj.b"_sd)}},
          {"truncatedArrays", std::vector<V>{}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogSubArrayDiff) {
    BSONObj diff = fromjson(
        "{"
        "   sarrField: {a: true, l: 10,"
        "           u0: 1,"
        "           u1: {a: 1}},"
        "   sarrField2: {a: true, l: 20}"
        "}");

    runUpdateV2OplogTest(diff,
                         D{{"updatedFields", D{{"arrField.0", 1}, {"arrField.1", D{{"a", 1}}}}},
                           {"removedFields", std::vector<V>{}},
                           {"truncatedArrays",
                            std::vector<V>{V{D{{"field", "arrField"_sd}, {"newSize", 10}}},
                                           V{D{{"field", "arrField2"_sd}, {"newSize", 20}}}}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogSubArrayDiffWithEmptyStringField) {
    BSONObj diff = fromjson(
        "{"
        "   s: {a: true, l: 10,"
        "           u0: 1,"
        "           u1: {a: 1}}"
        "}");

    runUpdateV2OplogTest(
        diff,
        D{{"updatedFields", D{{".0", 1}, {".1", D{{"a", 1}}}}},
          {"removedFields", std::vector<V>{}},
          {"truncatedArrays", std::vector<V>{V{D{{"field", ""_sd}, {"newSize", 10}}}}}});
}

TEST_F(ChangeStreamStageTest, TransformDeltaOplogNestedComplexSubDiffs) {
    BSONObj diff = fromjson(
        "{"
        "   u: { a: 1, b: 2},"
        "   sarrField: {a: true, l: 10,"
        "           u0: 1,"
        "           u1: {a: 1},"
        "           s2: { u: {a: 1}},"  // "arrField.2.a" should be updated.
        "           u4: 1,"             // Test updating non-contiguous fields.
        "           u6: 2},"
        "   ssubObj: {"
        "           d: {b: false},"  // "subObj.b" should be removed.
        "           u: {a: 1}}"      // "subObj.a" should be updated.
        "}");

    runUpdateV2OplogTest(
        diff,
        D{{"updatedFields",
           D{
               {"a", 1},
               {"b", 2},
               {"arrField.0", 1},
               {"arrField.1", D{{"a", 1}}},
               {"arrField.2.a", 1},
               {"arrField.4", 1},
               {"arrField.6", 2},
               {"subObj.a", 1},
           }},
          {"removedFields", std::vector<V>{V("subObj.b"_sd)}},
          {"truncatedArrays", std::vector<V>{V{D{{"field", "arrField"_sd}, {"newSize", 10}}}}}});
}

// Legacy documents might not have an _id field; then the document key is the full (post-update)
// document.
TEST_F(ChangeStreamStageTest, TransformUpdateFieldsLegacyNoId) {
    BSONObj diff = BSON("u" << BSON("y" << 1));
    BSONObj o = BSON("diff" << diff << "$v" << 2);
    BSONObj o2 = BSON("x" << 1 << "y" << 1);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    const auto expectedUpdateField =
        makeExpectedUpdateEvent(kDefaultTs,
                                nss,
                                o2,
                                D{{"updatedFields", D{{"y", 1}}},
                                  {"removedFields", std::vector<V>()},
                                  {"truncatedArrays", std::vector<V>()}});
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageTest, TransformRemoveFields) {
    BSONObj diff = BSON("d" << BSON("y" << false));
    BSONObj o = BSON("diff" << diff << "$v" << 2);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto removeField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    const auto expectedUpdateField =
        makeExpectedUpdateEvent(kDefaultTs,
                                nss,
                                o2,
                                D{{"updatedFields", D{}},
                                  {"removedFields", {"y"_sd}},
                                  {"truncatedArrays", std::vector<V>()}});
    checkTransformation(removeField, expectedUpdateField);
}  // namespace

TEST_F(ChangeStreamStageTest, TransformReplace) {
    BSONObj o = BSON("_id" << 1 << "x" << 2 << "y" << 1);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto replace = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                  nss,                  // namespace
                                  o,                    // o
                                  testUuid(),           // uuid
                                  boost::none,          // fromMigrate
                                  o2);                  // o2

    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), o2, DSChangeStream::kReplaceOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeStreamStageTest, TransformReplaceShowExpandedEvents) {
    BSONObj o = BSON("_id" << 1 << "x" << 2 << "y" << 1);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto replace = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                  nss,                  // namespace
                                  o,                    // o
                                  testUuid(),           // uuid
                                  boost::none,          // fromMigrate
                                  o2);                  // o2

    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), o2, DSChangeStream::kReplaceOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, testUuid()},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(replace, expectedReplace, kShowExpandedEventsSpec);
}

TEST_F(ChangeStreamStageTest, TransformDelete) {
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), o, DSChangeStream::kDeleteOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),    // op type
                                       deleteEntry.getNss(),       // namespace
                                       deleteEntry.getObject(),    // o
                                       deleteEntry.getUuid(),      // uuid
                                       fromMigrate,                // fromMigrate
                                       deleteEntry.getObject2());  // o2

    checkTransformation(deleteEntry2, expectedDelete);
}

TEST_F(ChangeStreamStageTest, TransformDeleteShowExpandedEvents) {
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), o, DSChangeStream::kDeleteOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, testUuid()},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete, kShowExpandedEventsSpec);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),    // op type
                                       deleteEntry.getNss(),       // namespace
                                       deleteEntry.getObject(),    // o
                                       deleteEntry.getUuid(),      // uuid
                                       fromMigrate,                // fromMigrate
                                       deleteEntry.getObject2());  // o2

    checkTransformation(deleteEntry2, expectedDelete, kShowExpandedEventsSpec);
}

TEST_F(ChangeStreamStageTest, TransformDeleteFromMigrate) {
    bool fromMigrate = true;
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      BSON("_id" << 1),     // o
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    checkTransformation(deleteEntry, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformDeleteFromMigrateShowMigrations) {
    bool fromMigrate = true;
    BSONObj o = BSON("_id" << 1);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      fromMigrate,          // fromMigrate
                                      BSON("_id" << 1));    // o2

    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");
    Document expectedDelete{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), o, DSChangeStream::kDeleteOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };

    checkTransformation(deleteEntry, expectedDelete, spec);
}

TEST_F(ChangeStreamStageTest, TransformDrop) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());

    Document expectedDrop{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         testUuid(),
                         Value(),
                         DSChangeStream::kDropCollectionOpType,
                         ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
    };

    checkTransformation(dropColl, expectedDrop, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformDropShowExpandedEvents) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());

    Document expectedDrop{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, testUuid()},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };

    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         testUuid(),
                         Value(),
                         DSChangeStream::kDropCollectionOpType,
                         ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
    };

    checkTransformation(dropColl, expectedDrop, kShowExpandedEventsSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformCreate) {
    OplogEntry create = createCommand(BSON("create" << nss.coll() << "idIndex"
                                                    << BSON("v" << 2 << "key" << BSON("id" << 1))
                                                    << "name" << IndexConstants::kIdIndexName),
                                      testUuid());

    const auto expectedOpDescription = fromjson("{idIndex: {v: 2, key: {id: 1}}, name: '_id_'}");
    Document expectedCreate{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), expectedOpDescription, DSChangeStream::kCreateOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kCreateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, testUuid()},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kOperationDescriptionField, Value(expectedOpDescription)},
        {DSChangeStream::kNsTypeField, "collection"_sd}};

    checkTransformation(create, expectedCreate, kShowExpandedEventsSpec);
}

TEST_F(ChangeStreamStageTest, TransformRename) {
    NamespaceString otherColl = NamespaceString::createNamespaceString_forTest("test.bar");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << nss.ns_forTest() << "to" << otherColl.ns_forTest()), testUuid());

    const auto opDesc =
        Value(D{{"to", D{{"db", otherColl.db_forTest()}, {"coll", otherColl.coll()}}}});
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db_forTest()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), opDesc, DSChangeStream::kRenameCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         testUuid(),
                         opDesc,
                         DSChangeStream::kRenameCollectionOpType,
                         ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
    };

    checkTransformation(rename, expectedRename, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformRenameShowExpandedEvents) {
    NamespaceString otherColl = NamespaceString::createNamespaceString_forTest("test.bar");
    auto dropTarget = UUID::gen();
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns_forTest() << "to" << otherColl.ns_forTest()
                                              << "dropTarget" << dropTarget),
                      testUuid());

    const auto opDesc = V{
        D{{"to", D{{"db", otherColl.db_forTest()}, {"coll", otherColl.coll()}}},
          {"dropTarget", dropTarget}},
    };
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db_forTest()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), opDesc, DSChangeStream::kRenameCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, testUuid()},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kOperationDescriptionField, opDesc},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         testUuid(),
                         opDesc,
                         DSChangeStream::kRenameCollectionOpType,
                         ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
    };

    checkTransformation(rename, expectedRename, kShowExpandedEventsSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformInvalidateFromMigrate) {
    NamespaceString otherColl = NamespaceString::createNamespaceString_forTest("test.bar");

    bool dropCollFromMigrate = true;
    OplogEntry dropColl =
        createCommand(BSON("drop" << nss.coll()), testUuid(), dropCollFromMigrate);
    bool dropDBFromMigrate = true;
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, dropDBFromMigrate);
    bool renameFromMigrate = true;
    OplogEntry rename = createCommand(
        BSON("renameCollection" << nss.ns_forTest() << "to" << otherColl.ns_forTest()),
        boost::none,
        renameFromMigrate);

    for (auto& entry : {dropColl, dropDB, rename}) {
        checkTransformation(entry, boost::none);
    }
}

TEST_F(ChangeStreamStageTest, TransformRenameTarget) {
    NamespaceString otherColl = NamespaceString::createNamespaceString_forTest("test.bar");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << otherColl.ns_forTest() << "to" << nss.ns_forTest()), testUuid());

    const auto opDesc = Value(D{{"to", D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}}});
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), opDesc, DSChangeStream::kRenameCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField,
         D{{"db", otherColl.db_forTest()}, {"coll", otherColl.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         testUuid(),
                         opDesc,
                         DSChangeStream::kRenameCollectionOpType,
                         ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
    };

    checkTransformation(rename, expectedRename, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, MatchFiltersDropDatabaseCommand) {
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, false);
    checkTransformation(dropDB, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformNewShardDetected) {
    auto o2Field = D{{"migrateChunkToNewShard", nss.toString_forTest()},
                     {"fromShardId", "fromShard"_sd},
                     {"toShardId", "toShard"_sd}};
    auto newShardDetected = makeOplogEntry(OpTypeEnum::kNoop,
                                           nss,
                                           BSONObj(),
                                           testUuid(),
                                           boost::none,  // fromMigrate
                                           o2Field.toBson());

    const auto opDesc = Value(D{{"fromShardId", "fromShard"_sd}, {"toShardId", "toShard"_sd}});
    Document expectedNewShardDetected{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), opDesc, DSChangeStream::kNewShardDetectedOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kNewShardDetectedOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, testUuid()},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kOperationDescriptionField, opDesc},
    };

    getExpCtx()->setNeedsMerge(true);

    checkTransformation(newShardDetected, expectedNewShardDetected, kShowExpandedEventsSpec);
}

TEST_F(ChangeStreamStageTest, TransformShardingEvents) {
    auto uuid = UUID::gen();

    for (auto eventType : {DSChangeStream::kShardCollectionOpType,
                           DSChangeStream::kMigrateLastChunkFromShardOpType,
                           DSChangeStream::kRefineCollectionShardKeyOpType,
                           DSChangeStream::kReshardCollectionOpType,
                           DSChangeStream::kNewShardDetectedOpType,
                           DSChangeStream::kReshardBeginOpType,
                           DSChangeStream::kReshardBlockingWritesOpType,
                           DSChangeStream::kReshardDoneCatchUpOpType}) {

        const bool hasReshardingUuid = eventType == DSChangeStream::kReshardBeginOpType ||
            eventType == DSChangeStream::kReshardBlockingWritesOpType ||
            eventType == DSChangeStream::kReshardDoneCatchUpOpType;

        BSONObjBuilder bob;
        bob.appendBool(eventType, 1);
        if (hasReshardingUuid) {
            bob.append(DSChangeStream::kReshardingUuidField, uuid.toBSON());
        }

        auto entry = makeOplogEntry(OpTypeEnum::kNoop,
                                    nss,
                                    BSONObj(),
                                    uuid,
                                    false,  // fromMigrate
                                    bob.obj());

        Value opDesc = V{D{}};
        if (hasReshardingUuid) {
            opDesc = V{D{{DSChangeStream::kReshardingUuidField, D{{"uuid"_sd, uuid}}}}};
        }

        Document expectedDoc{
            {DSChangeStream::kReshardingUuidField,
             hasReshardingUuid ? V{D{{"uuid"_sd, uuid}}} : V{}},
            {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, uuid, opDesc, eventType)},
            {DSChangeStream::kOperationTypeField, eventType},
            {DSChangeStream::kClusterTimeField, kDefaultTs},
            {DSChangeStream::kCollectionUuidField, uuid},
            {DSChangeStream::kWallTimeField, Date_t()},
            {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
            {DSChangeStream::kOperationDescriptionField, opDesc},
        };

        if (eventType == DSChangeStream::kNewShardDetectedOpType) {
            // Need to set this because the event is only emitted on the router.
            getExpCtx()->setNeedsMerge(true);
        }

        // Using 'show...Events' here in order to see all relevant events.
        checkTransformation(
            entry,
            expectedDoc,
            BSON("$changeStream" << BSON("showSystemEvents" << true << "showExpandedEvents" << true
                                                            << "showMigrationEvents" << true)));
    }
}

TEST_F(ChangeStreamStageTest, TransformReshardBegin) {
    auto uuid = UUID::gen();
    auto reshardingUuid = UUID::gen();

    ReshardBeginChangeEventO2Field o2Field{nss, reshardingUuid};
    auto reshardingBegin = makeOplogEntry(OpTypeEnum::kNoop,
                                          nss,
                                          BSONObj(),
                                          uuid,
                                          true,  // fromMigrate
                                          o2Field.toBSON());

    auto spec = fromjson("{$changeStream: {showMigrationEvents: true, showExpandedEvents: true}}");

    const auto opDesc = V{D{{"reshardingUUID", reshardingUuid}}};
    Document expectedReshardingBegin{
        {DSChangeStream::kReshardingUuidField, reshardingUuid},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, uuid, opDesc, DSChangeStream::kReshardBeginOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReshardBeginOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, uuid},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kOperationDescriptionField, opDesc},
    };
    checkTransformation(reshardingBegin, expectedReshardingBegin, spec);
}

TEST_F(ChangeStreamStageTest, TransformReshardBlockingWrites) {
    auto uuid = UUID::gen();
    auto reshardingUuid = UUID::gen();

    ReshardBlockingWritesChangeEventO2Field o2Field{
        nss, reshardingUuid, std::string{resharding::kReshardFinalOpLogType}};
    auto reshardingBlockingWrites = makeOplogEntry(OpTypeEnum::kNoop,
                                                   nss,
                                                   BSONObj(),
                                                   uuid,
                                                   false,  // fromMigrate
                                                   o2Field.toBSON());

    auto spec = fromjson("{$changeStream: {showSystemEvents: true, showExpandedEvents: true}}");

    const auto opDesc =
        D{{"reshardingUUID", reshardingUuid}, {"type", resharding::kReshardFinalOpLogType}};

    Document expectedReshardingBlockingWrites{
        {DSChangeStream::kReshardingUuidField, reshardingUuid},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, uuid, opDesc, DSChangeStream::kReshardBlockingWritesOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReshardBlockingWritesOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, uuid},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kOperationDescriptionField, opDesc},
    };
    checkTransformation(reshardingBlockingWrites, expectedReshardingBlockingWrites, spec);
}

TEST_F(ChangeStreamStageTest, TransformReshardDoneCatchUp) {
    auto existingUuid = UUID::gen();
    auto reshardingUuid = UUID::gen();
    auto temporaryNs = resharding::constructTemporaryReshardingNss(nss, existingUuid);

    ReshardDoneCatchUpChangeEventO2Field o2Field{temporaryNs, reshardingUuid};
    auto reshardDoneCatchUp = makeOplogEntry(OpTypeEnum::kNoop,
                                             temporaryNs,
                                             BSONObj(),
                                             reshardingUuid,
                                             true,  // fromMigrate
                                             o2Field.toBSON());

    auto spec = fromjson(
        "{$changeStream: {showMigrationEvents: true, allowToRunOnSystemNS: true, "
        "showExpandedEvents: true}}");
    auto expCtx = getExpCtx();
    expCtx->setNamespaceString(temporaryNs);

    const auto opDesc = V{D{{"reshardingUUID", reshardingUuid}}};
    Document expectedReshardingDoneCatchUp{
        {DSChangeStream::kReshardingUuidField, reshardingUuid},
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, reshardingUuid, opDesc, DSChangeStream::kReshardDoneCatchUpOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReshardDoneCatchUpOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, reshardingUuid},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField,
         D{{"db", temporaryNs.db_forTest()}, {"coll", temporaryNs.coll()}}},
        {DSChangeStream::kOperationDescriptionField, opDesc},
    };

    checkTransformation(reshardDoneCatchUp, expectedReshardingDoneCatchUp, spec);
}

TEST_F(ChangeStreamStageTest, TransformEmptyApplyOps) {
    Document applyOpsDoc{{"applyOps", Value{std::vector<Document>{}}}};

    LogicalSessionFromClient lsid = testLsid();
    std::vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // Should not return anything.
    ASSERT_EQ(results.size(), 0u);
}

DEATH_TEST_F(ChangeStreamStageTest, ShouldCrashWithNoopInsideApplyOps, "Unexpected noop") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", "n"_sd},
                               {"ns", nss.ns_forTest()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

DEATH_TEST_F(ChangeStreamStageTest,
             ShouldCrashWithEntryWithoutOpFieldInsideApplyOps,
             "Unexpected format for entry") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"ns", nss.ns_forTest()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

DEATH_TEST_F(ChangeStreamStageTest,
             ShouldCrashWithEntryWithNonStringOpFieldInsideApplyOps,
             "Unexpected format for entry") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", 2},
                               {"ns", nss.ns_forTest()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

TEST_F(ChangeStreamStageTest, TransformNonTxnNumberApplyOps) {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", "i"_sd},
                               {"ns", nss.ns_forTest()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};

    LogicalSessionFromClient lsid = testLsid();
    std::vector<Document> results =
        getApplyOpsResults(applyOpsDoc, lsid, kDefaultSpec, false /* hasTxnNumber */);

    ASSERT_EQ(results.size(), 1u);

    const auto nextDoc = results[0];
    ASSERT(nextDoc.getField("txnNumber").missing());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["x"].getString(), "hallo");
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);
}

TEST_F(ChangeStreamStageTest, TransformNonTxnNumberBatchedDeleteApplyOps) {

    Document applyOpsDoc{
        {"applyOps",
         Value{std::vector<Document>{
             Document{{"op", "d"_sd},
                      {"ns", nss.ns_forTest()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"_id", 10}}}}},
             Document{{"op", "d"_sd},
                      {"ns", nss.ns_forTest()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"_id", 11}}}}},
             Document{{"op", "d"_sd},
                      {"ns", nss.ns_forTest()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"_id", 12}}}}},
         }}},
    };
    LogicalSessionFromClient lsid = testLsid();
    std::vector<Document> results =
        getApplyOpsResults(applyOpsDoc, lsid, kDefaultSpec, false /* hasTxnNumber */);

    ASSERT_EQ(results.size(), 3u);

    int i = 0;
    for (const auto& nextDoc : results) {
        ASSERT(nextDoc.getField("txnNumber").missing());
        ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
                  DSChangeStream::kDeleteOpType);
        ASSERT_EQ(nextDoc[DSChangeStream::kDocumentKeyField]["_id"].getInt(), 10 + i++);
        ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);
    }
}

TEST_F(ChangeStreamStageTest, TransformApplyOpsWithEntriesOnDifferentNs) {
    // Doesn't use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.

    auto otherUUID = UUID::gen();
    Document applyOpsDoc{
        {"applyOps",
         Value{std::vector<Document>{
             Document{{"op", "i"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", otherUUID},
                      {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}},
             Document{{"op", "u"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", otherUUID},
                      {"o", Value{Document{{"$set", Value{Document{{"x", "hallo 2"_sd}}}}}}},
                      {"o2", Value{Document{{"_id", 123}}}}},
         }}},
    };
    LogicalSessionFromClient lsid = testLsid();
    std::vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // All documents should be skipped.
    ASSERT_EQ(results.size(), 0u);
}

TEST_F(ChangeStreamStageTest, PreparedTransactionApplyOpsEntriesAreIgnored) {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", "i"_sd},
                               {"ns", nss.ns_forTest()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}},
                 {"prepare", true}};
    LogicalSessionFromClient lsid = testLsid();
    std::vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // applyOps entries that are part of a prepared transaction are ignored. These entries will be
    // fetched for changeStreams delivery as part of transaction commit.
    ASSERT_EQ(results.size(), 0u);
}

TEST_F(ChangeStreamStageTest, CommitCommandReturnsOperationsFromPreparedTransaction) {
    // Create an oplog entry representing a prepared transaction.
    Document preparedApplyOps{
        {"applyOps",
         Value{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{D{{"_id", 123}}}},
               {"o2", V{D{}}}},
         }}},
        {"prepare", true},
    };

    repl::OpTime applyOpsOpTime(Timestamp(99, 1), 1);
    auto preparedTransaction = makeOplogEntry(OpTypeEnum::kCommand,
                                              nss.getCommandNS(),
                                              preparedApplyOps.toBson(),
                                              testUuid(),
                                              boost::none,  // fromMigrate
                                              boost::none,  // o2 field
                                              applyOpsOpTime);

    // Create an oplog entry representing the commit for the prepared transaction. The commit has a
    // 'prevWriteOpTimeInTransaction' value that matches the 'preparedApplyOps' entry, which the
    // MockMongoInterface will pretend is in the oplog.
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    auto oplogEntry = repl::DurableOplogEntry(
        kDefaultOpTime,                   // optime
        OpTypeEnum::kCommand,             // opType
        nss.getCommandNS(),               // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        boost::none,                      // checkExistenceForDiffInsert
        boost::none,                      // versionContext
        repl::OplogEntry::kOplogVersion,  // version
        BSON("commitTransaction" << 1 << "commitTimestamp" << kDefaultCommitTs),  // o
        boost::none,                                                              // o2
        sessionInfo,                                                              // sessionInfo
        boost::none,                                                              // upsert
        Date_t(),                                                                 // wall clock time
        {},                                                                       // statement ids
        applyOpsOpTime,  // optime of previous write within same transaction
        boost::none,     // pre-image optime
        boost::none,     // post-image optime
        boost::none,     // ShardId of resharding recipient
        boost::none,     // _id
        boost::none);    // needsRetryImage

    {
        // No expanded events.

        // When the DocumentSourceChangeStreamTransform sees the "commitTransaction" oplog entry, we
        // expect it to return the insert op within our 'preparedApplyOps' oplog entry.
        Document expectedResult{
            {DSChangeStream::kTxnNumberField, static_cast<int>(*sessionInfo.getTxnNumber())},
            {DSChangeStream::kLsidField, Document{{sessionInfo.getSessionId()->toBSON()}}},
            {DSChangeStream::kIdField,
             makeResumeToken(kDefaultTs, testUuid(), BSONObj(), DSChangeStream::kInsertOpType)},
            {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
            {DSChangeStream::kClusterTimeField, kDefaultTs},
            {DSChangeStream::kWallTimeField, Date_t()},
            {DSChangeStream::kFullDocumentField, D{{"_id", 123}}},
            {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
            {DSChangeStream::kDocumentKeyField, D{}},
        };

        checkTransformation(oplogEntry, expectedResult, kDefaultSpec, {}, {preparedTransaction});
    }

    {
        // Expanded events: this will additionally emit the 'commitTimestamp' and 'collectionUUID'
        // fields.

        // When the DocumentSourceChangeStreamTransform sees the "commitTransaction" oplog entry, we
        // expect it to return the insert op within our 'preparedApplyOps' oplog entry.
        Document expectedResult{
            {DSChangeStream::kTxnNumberField, static_cast<int>(*sessionInfo.getTxnNumber())},
            {DSChangeStream::kLsidField, Document{{sessionInfo.getSessionId()->toBSON()}}},
            {DSChangeStream::kIdField,
             makeResumeToken(kDefaultTs, testUuid(), BSONObj(), DSChangeStream::kInsertOpType)},
            {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
            {DSChangeStream::kClusterTimeField, kDefaultTs},
            {DSChangeStream::kCommitTimestampField, kDefaultCommitTs},
            {DSChangeStream::kWallTimeField, Date_t()},
            {DSChangeStream::kFullDocumentField, D{{"_id", 123}}},
            {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
            {DSChangeStream::kDocumentKeyField, D{}},
        };

        checkTransformation(
            oplogEntry, expectedResult, kShowCommitTimestampSpec, {}, {preparedTransaction});
    }
}

TEST_F(ChangeStreamStageTest, TransactionWithMultipleOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create two applyOps entries that together represent a whole transaction.
    repl::OpTime applyOpsOpTime1(Timestamp(100, 1), 1);
    Document applyOps1{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{Document{{"_id", 123}}}},
               {"o2", V{Document{{"_id", 123}}}}},
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{Document{{"_id", 456}}}},
               {"o2", V{Document{{"_id", 456}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(100, 2), 1);
    Document applyOps2{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{D{{"_id", 789}}}},
               {"o2", V{D{{"_id", 789}}}}},
         }}},
        /* The absence of the "partialTxn" and "prepare" fields indicates that this command commits
           the transaction. */
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // We do not use the checkTransformation() pattern that other tests use since we expect
    // multiple documents to be returned from one applyOps.
    auto execPipeline = makeExecPipeline(transactionEntry2, kDefaultSpec);
    auto transform = execPipeline->getStages()[3].get();
    invariant(dynamic_cast<exec::agg::ChangeStreamTransformStage*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(
        std::vector<repl::OplogEntry>{transactionEntry2, transactionEntry1}));

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    // Note that we never expect to see a 'commitTimestamp' event field because the events are
    // not from a prepared transaction.
    assertCommitTimestamp(false /* showCommitTimestamp */, nextDoc);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime2.getTimestamp(),
                                       testUuid(),
                                       V{D{{"_id", 123}}},
                                       DSChangeStream::kInsertOpType,
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    // Note that we never expect to see a 'commitTimestamp' event field because the events are
    // not from a prepared transaction.
    assertCommitTimestamp(false /* showCommitTimestamp */, nextDoc);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime2.getTimestamp(),
                                       testUuid(),
                                       V{D{{"_id", 456}}},
                                       DSChangeStream::kInsertOpType,
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       1));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 789);
    // Note that we never expect to see a 'commitTimestamp' event field because the events are
    // not from a prepared transaction.
    assertCommitTimestamp(false /* showCommitTimestamp */, nextDoc);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime2.getTimestamp(),
                                       testUuid(),
                                       V{D{{"_id", 789}}},
                                       DSChangeStream::kInsertOpType,
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       2));
}

TEST_F(ChangeStreamStageTest, TransactionWithEmptyOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create a transaction that is chained across 5 applyOps oplog entries. The first, third, and
    // final oplog entries in the transaction chain contain empty applyOps arrays. The test verifies
    // that change streams (1) correctly detect the transaction chain despite the fact that the
    // final applyOps, which implicitly commits the transaction, is empty; and (2) behaves correctly
    // upon encountering empty applyOps at other stages of the transaction chain.
    repl::OpTime applyOpsOpTime1(Timestamp(100, 1), 1);
    Document applyOps1{
        {"applyOps", V{std::vector<Document>{}}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(100, 2), 1);
    Document applyOps2{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{Document{{"_id", 123}}}},
               {"o2", V{Document{{"_id", 123}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    repl::OpTime applyOpsOpTime3(Timestamp(100, 3), 1);
    Document applyOps3{
        {"applyOps", V{std::vector<Document>{}}},
        {"partialTxn", true},
    };

    auto transactionEntry3 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps3.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime3,
                                            sessionInfo,
                                            applyOpsOpTime2);

    repl::OpTime applyOpsOpTime4(Timestamp(100, 4), 1);
    Document applyOps4{
        {"applyOps",
         V{std::vector<Document>{D{{"op", "i"_sd},
                                   {"ns", nss.ns_forTest()},
                                   {"ui", testUuid()},
                                   {"o", V{Document{{"_id", 456}}}},
                                   {"o2", V{Document{{"_id", 456}}}}}}}},
        {"partialTxn", true},
    };

    auto transactionEntry4 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps4.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime4,
                                            sessionInfo,
                                            applyOpsOpTime3);

    repl::OpTime applyOpsOpTime5(Timestamp(100, 5), 1);
    Document applyOps5{
        {"applyOps", V{std::vector<Document>{}}},
        /* The absence of the "partialTxn" and "prepare" fields indicates that this command commits
           the transaction. */
    };

    auto transactionEntry5 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps5.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime5,
                                            sessionInfo,
                                            applyOpsOpTime4);

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto execPipeline = makeExecPipeline(transactionEntry5);
    auto transform = execPipeline->getStages()[3].get();
    invariant(dynamic_cast<exec::agg::ChangeStreamTransformStage*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::vector<repl::OplogEntry>{transactionEntry5,
                                                                           transactionEntry4,
                                                                           transactionEntry3,
                                                                           transactionEntry2,
                                                                           transactionEntry1}));

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime5.getTimestamp(),
                                       testUuid(),
                                       V{D{{"_id", 123}}},
                                       DSChangeStream::kInsertOpType,
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(resumeToken,
                       makeResumeToken(applyOpsOpTime5.getTimestamp(),
                                       testUuid(),
                                       V{D{{"_id", 456}}},
                                       DSChangeStream::kInsertOpType,
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                                       1));
}

TEST_F(ChangeStreamStageTest, TransactionWithOnlyEmptyOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create a transaction that is chained across 2 applyOps oplog entries. This test verifies that
    // a change stream correctly reads an empty transaction and does not observe any events from it.
    repl::OpTime applyOpsOpTime1(Timestamp(100, 1), 1);
    Document applyOps1{
        {"applyOps", V{std::vector<Document>{}}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(100, 2), 1);
    Document applyOps2{
        {"applyOps", V{std::vector<Document>{}}},
        /* The absence of the "partialTxn" and "prepare" fields indicates that this command commits
           the transaction. */
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto execPipeline = makeExecPipeline(transactionEntry2);
    auto transform = execPipeline->getStages()[3].get();
    invariant(dynamic_cast<exec::agg::ChangeStreamTransformStage*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(
        std::vector<repl::OplogEntry>{transactionEntry2, transactionEntry1}));

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(!next.isAdvanced());
}

TEST_F(ChangeStreamStageTest, PreparedTransactionWithMultipleOplogEntries) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create two applyOps entries that together represent a whole transaction.
    repl::OpTime applyOpsOpTime1(Timestamp(99, 1), 1);
    Document applyOps1{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{D{{"_id", 123}}}},
               {"o2", V{D{{"_id", 123}}}}},
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{D{{"_id", 456}}}},
               {"o2", V{D{{"_id", 456}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(99, 2), 1);
    Document applyOps2{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{D{{"_id", 789}}}},
               {"o2", V{D{{"_id", 789}}}}},
         }}},
        {"prepare", true},
    };

    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // Create an oplog entry representing the commit for the prepared transaction.
    auto commitEntry = repl::DurableOplogEntry(
        kDefaultOpTime,                   // optime
        OpTypeEnum::kCommand,             // opType
        nss.getCommandNS(),               // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        boost::none,                      // checkExistenceForDiffInsert
        boost::none,                      // versionContext
        repl::OplogEntry::kOplogVersion,  // version
        BSON("commitTransaction" << 1 << "commitTimestamp" << kDefaultCommitTs),  // o
        boost::none,                                                              // o2
        sessionInfo,                                                              // sessionInfo
        boost::none,                                                              // upsert
        Date_t(),                                                                 // wall clock time
        {},                                                                       // statement ids
        applyOpsOpTime2,  // optime of previous write within same transaction
        boost::none,      // pre-image optime
        boost::none,      // post-image optime
        boost::none,      // ShardId of resharding recipient
        boost::none,      // _id
        boost::none);     // needsRetryImage

    // We do not use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    auto execPipeline = makeExecPipeline(commitEntry);
    auto transform = execPipeline->getStages()[3].get();
    invariant(dynamic_cast<exec::agg::ChangeStreamTransformStage*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(
        std::vector<repl::OplogEntry>{commitEntry, transactionEntry2, transactionEntry1}));

    // We should get three documents from the change stream, based on the documents in the two
    // applyOps entries.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{{"_id", 123}}},
                        DSChangeStream::kInsertOpType,
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{{"_id", 456}}},
                        DSChangeStream::kInsertOpType,
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        1));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 789);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{{"_id", 789}}},
                        DSChangeStream::kInsertOpType,
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        2));

    next = transform->getNext();
    ASSERT(!next.isAdvanced());
}

TEST_F(ChangeStreamStageTest, PreparedTransactionEndingWithEmptyApplyOps) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(1);
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    // Create two applyOps entries that together represent a whole transaction.
    repl::OpTime applyOpsOpTime1(Timestamp(99, 1), 1);
    Document applyOps1{
        {"applyOps",
         V{std::vector<Document>{
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{D{{"_id", 123}}}},
               {"o2", V{D{{"_id", 123}}}}},
             D{{"op", "i"_sd},
               {"ns", nss.ns_forTest()},
               {"ui", testUuid()},
               {"o", V{D{{"_id", 456}}}},
               {"o2", V{D{{"_id", 456}}}}},
         }}},
        {"partialTxn", true},
    };

    auto transactionEntry1 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps1.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime1,
                                            sessionInfo,
                                            repl::OpTime());

    repl::OpTime applyOpsOpTime2(Timestamp(99, 2), 1);
    Document applyOps2{
        {"applyOps", V{std::vector<Document>{}}},
        {"prepare", true},
    };

    // The second applyOps is empty.
    auto transactionEntry2 = makeOplogEntry(OpTypeEnum::kCommand,
                                            nss.getCommandNS(),
                                            applyOps2.toBson(),
                                            testUuid(),
                                            boost::none,  // fromMigrate
                                            boost::none,  // o2 field
                                            applyOpsOpTime2,
                                            sessionInfo,
                                            applyOpsOpTime1);

    // Create an oplog entry representing the commit for the prepared transaction.
    auto commitEntry = repl::DurableOplogEntry(
        kDefaultOpTime,                   // optime
        OpTypeEnum::kCommand,             // opType
        nss.getCommandNS(),               // namespace
        boost::none,                      // uuid
        boost::none,                      // fromMigrate
        boost::none,                      // checkExistenceForDiffInsert
        boost::none,                      // versionContext
        repl::OplogEntry::kOplogVersion,  // version
        BSON("commitTransaction" << 1 << "commitTimestamp" << kDefaultCommitTs),  // o
        boost::none,                                                              // o2
        sessionInfo,                                                              // sessionInfo
        boost::none,                                                              // upsert
        Date_t(),                                                                 // wall clock time
        {},                                                                       // statement ids
        applyOpsOpTime2,  // optime of previous write within same transaction
        boost::none,      // pre-image optime
        boost::none,      // post-image optime
        boost::none,      // ShardId of resharding recipient
        boost::none,      // _id
        boost::none);     // needsRetryImage

    // We do not use the checkTransformation() pattern that other tests use since we expect
    // multiple documents to be returned from one applyOps.
    auto execPipeline = makeExecPipeline(commitEntry, kDefaultSpec);
    auto transform = execPipeline->getStages()[3].get();
    invariant(dynamic_cast<exec::agg::ChangeStreamTransformStage*>(transform) != nullptr);

    // Populate the MockTransactionHistoryEditor in reverse chronological order.
    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(
        std::vector<repl::OplogEntry>{commitEntry, transactionEntry2, transactionEntry1}));

    // We should get two documents from the change stream, based on the documents in the
    // non-empty applyOps entry.
    auto next = transform->getNext();
    ASSERT(next.isAdvanced());
    auto nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    assertCommitTimestamp(false /* showCommitTimestamp */, nextDoc);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    auto resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{{"_id", 123}}},
                        DSChangeStream::kInsertOpType,
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        0));

    next = transform->getNext();
    ASSERT(next.isAdvanced());
    nextDoc = next.releaseDocument();
    ASSERT_EQ(nextDoc[DSChangeStream::kTxnNumberField].getLong(), *sessionInfo.getTxnNumber());
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 456);
    assertCommitTimestamp(false /* showCommitTimestamp */, nextDoc);
    ASSERT_EQ(
        nextDoc["lsid"].getDocument().toBson().woCompare(sessionInfo.getSessionId()->toBSON()), 0);
    resumeToken = ResumeToken::parse(nextDoc["_id"].getDocument()).toDocument();
    ASSERT_DOCUMENT_EQ(
        resumeToken,
        makeResumeToken(kDefaultOpTime.getTimestamp(),  // Timestamp of the commitCommand.
                        testUuid(),
                        V{D{{"_id", 456}}},
                        DSChangeStream::kInsertOpType,
                        ResumeTokenData::FromInvalidate::kNotFromInvalidate,
                        1));

    next = transform->getNext();
    ASSERT(!next.isAdvanced());
}

TEST_F(ChangeStreamStageTest, TransformApplyOps) {
    // Doesn't use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    Document applyOpsDoc{
        {"applyOps",
         Value{std::vector<Document>{
             Document{{"op", "i"_sd},
                      {"ns", nss.ns_forTest()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}},
             Document{{"op", "u"_sd},
                      {"ns", nss.ns_forTest()},
                      {"ui", testUuid()},
                      {"o",
                       Value{Document{
                           {"diff", Value{Document{{"u", Value{Document{{"x", "hallo 2"_sd}}}}}}},
                           {"$v", 2}}}},
                      {"o2", Value{Document{{"_id", 123}}}}},
             // Operation on another namespace which should be skipped.
             Document{{"op", "i"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", UUID::gen()},
                      {"o", Value{Document{{"_id", 0}, {"x", "Should not read this!"_sd}}}}},
         }}},
    };
    LogicalSessionFromClient lsid = testLsid();
    std::vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // The third document should be skipped.
    ASSERT_EQ(results.size(), 2u);

    // Check that the first document is correct.
    auto nextDoc = results[0];
    ASSERT_EQ(nextDoc["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["x"].getString(), "hallo");
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);

    // Check the second document.
    nextDoc = results[1];
    ASSERT_EQ(nextDoc["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kUpdateOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kDocumentKeyField]["_id"].getInt(), 123);
    ASSERT_EQ(nextDoc[DSChangeStream::kUpdateDescriptionField]["updatedFields"]["x"].getString(),
              "hallo 2");
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);

    // The third document is skipped.
}

TEST_F(ChangeStreamStageTest, TransformApplyOpsWithCreateOperation) {
    // Enable the endOfTransaction feature flag so this test produces an EOT change event.
    RAIIServerParameterControllerForTest controller("featureFlagEndOfTransactionChangeEvent", true);
    // Doesn't use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.
    Document idIndexDef = Document{{"v", 2}, {"key", D{{"_id", 1}}}};
    Document applyOpsDoc{
        {"applyOps",
         Value{std::vector<Document>{
             Document{{"op", "c"_sd},
                      {"ns", nss.db_forTest() + ".$cmd"},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"create", nss.coll()}, {"idIndex", idIndexDef}}}},
                      {"ts", Timestamp(0, 1)}},
             Document{{"op", "i"_sd},
                      {"ns", nss.ns_forTest()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}},
             Document{
                 {"op", "c"_sd},
                 {"ns", nss.db_forTest() + ".$cmd"},
                 {"ui", UUID::gen()},
                 // Operation on another collection which should be skipped.
                 {"o", Value{Document{{"create", "otherCollection"_sd}, {"idIndex", idIndexDef}}}}},
         }}},
    };
    LogicalSessionFromClient lsid = testLsid();
    std::vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid, kShowExpandedEventsSpec);

    size_t expectedSize = 3;

    // The create operation should be skipped.
    ASSERT_EQ(results.size(), expectedSize);

    // Check that the first document is correct.
    auto nextDoc = results[0];
    ASSERT_EQ(nextDoc["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kCreateOpType);
    ASSERT_VALUE_EQ(nextDoc[DSChangeStream::kOperationDescriptionField],
                    Value(Document{{"idIndex", idIndexDef}}));
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);
    ASSERT_EQ(nextDoc[DSChangeStream::kNsTypeField].getString(), "collection"_sd);
    ASSERT_EQ(ResumeToken::parse(nextDoc["_id"].getDocument()).getData().txnOpIndex, 0);

    // Check the second document.
    nextDoc = results[1];
    ASSERT_EQ(nextDoc["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kInsertOpType);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["_id"].getInt(), 123);
    ASSERT_EQ(nextDoc[DSChangeStream::kFullDocumentField]["x"].getString(), "hallo");
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);
    ASSERT_EQ(ResumeToken::parse(nextDoc["_id"].getDocument()).getData().txnOpIndex, 1);

    // The third document in applyOps is skipped, so the third document of the result is
    // endOfTransaction.
    nextDoc = results[2];
    ASSERT_EQ(nextDoc[DSChangeStream::kOperationTypeField].getString(),
              DSChangeStream::kEndOfTransactionOpType);
    ASSERT_EQ(nextDoc["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(nextDoc["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);
    auto operationDescription = nextDoc[DSChangeStream::kOperationDescriptionField];
    ASSERT_EQ(operationDescription["txnNumber"].getLong(), 0LL);
    ASSERT_EQ(operationDescription["lsid"].getDocument().toBson().woCompare(lsid.toBSON()), 0);
    // Third document (with txnOpIndex == 2) is skipped, so EOT should have txnOpIndex == 3.
    ASSERT_EQ(ResumeToken::parse(nextDoc["_id"].getDocument()).getData().txnOpIndex, 3);
}

TEST_F(ChangeStreamStageTest, ClusterTimeMatchesOplogEntry) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);

    // Test the 'clusterTime' field is copied from the oplog entry for an update.
    BSONObj diff = BSON("u" << BSON("y" << 1));
    BSONObj o = BSON("diff" << diff << "$v" << 2);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2,                   // o2
                                      opTime);              // opTime

    const auto expectedUpdateField =
        makeExpectedUpdateEvent(ts,
                                nss,
                                o2,
                                D{{"updatedFields", D{{"y", 1}}},
                                  {"removedFields", std::vector<V>()},
                                  {"truncatedArrays", std::vector<V>()}});
    checkTransformation(updateField, expectedUpdateField);

    // Test the 'clusterTime' field is copied from the oplog entry for a collection drop.
    OplogEntry dropColl =
        createCommand(BSON("drop" << nss.coll()), testUuid(), boost::none, opTime);

    Document expectedDrop{
        {DSChangeStream::kIdField,
         makeResumeToken(ts, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };
    checkTransformation(dropColl, expectedDrop);

    // Test the 'clusterTime' field is copied from the oplog entry for a collection rename.
    NamespaceString otherColl = NamespaceString::createNamespaceString_forTest("test.bar");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << nss.ns_forTest() << "to" << otherColl.ns_forTest()),
        testUuid(),
        boost::none,
        opTime);

    const auto opDesc =
        Value(D{{"to", D{{"db", otherColl.db_forTest()}, {"coll", otherColl.coll()}}}});
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db_forTest()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField,
         makeResumeToken(ts, testUuid(), opDesc, DSChangeStream::kRenameCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateCollectionWhenShowExpandedEventsOff) {
    auto collSpec =
        D{{"create", "foo"_sd},
          {"idIndex", D{{"v", 2}, {"key", D{{"_id", 1}}}, {"name", IndexConstants::kIdIndexName}}}};
    OplogEntry createColl = createCommand(collSpec.toBson(), testUuid());
    checkTransformation(createColl, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersNoOp) {
    auto noOp = makeOplogEntry(OpTypeEnum::kNoop,  // op type
                               {},                 // namespace
                               BSON(repl::kNewPrimaryMsgField << repl::kNewPrimaryMsg));  // o

    checkTransformation(noOp, boost::none);
}


TEST_F(ChangeStreamStageTest, DocumentSourceChangeStreamTransformParseValidSupportedEvents) {
    auto expCtx = getExpCtx();

    for (const auto& supportedEvents :
         {BSONArray(),
          BSON_ARRAY("singleEvent"),
          BSON_ARRAY("CASE" << "case"
                            << "Case"
                            << "insensitive"),  //< Test case sensitivity.
          BSON_ARRAY("someEvent" << "someOtherEvent"
                                 << "yetAnotherEvent")}) {
        BSONObj spec =
            BSON(DocumentSourceChangeStreamTransform::kStageName << BSON(
                     "resumeAfter"
                     << makeResumeToken(kDefaultTs, Value(), Value(), DSChangeStream::kInsertOpType)
                     << "supportedEvents" << supportedEvents));

        auto expected = Value(supportedEvents).getArray();

        auto stage =
            DocumentSourceChangeStreamTransform::createFromBson(spec.firstElement(), expCtx);
        std::vector<Value> serialization;
        stage->serializeToArray(serialization);
        ASSERT_EQ(serialization.size(), 1UL);
        ASSERT_EQ(serialization[0].getType(), BSONType::object);

        auto actualSupportedEvents = serialization[0]
                                         .getDocument()
                                         .getField(DocumentSourceChangeStreamTransform::kStageName)
                                         .getDocument()
                                         .getField("supportedEvents"_sd);
        ASSERT_TRUE(actualSupportedEvents.isArray());
        ASSERT_VALUE_EQ(Value(expected), Value(actualSupportedEvents.getArray()));
    }
}

TEST_F(ChangeStreamStageTest, DocumentSourceChangeStreamTransformParseInvalidSupportedEvents) {
    auto expCtx = getExpCtx();

    for (const auto& supportedEvents : {
             BSON_ARRAY("singleEvent" << "singleEvent"),
             BSON_ARRAY("a" << "b"
                            << "c"
                            << "d"
                            << "a"),
             BSON_ARRAY(""),  //< Test invalid name.
             BSON_ARRAY("a" << "b"
                            << ""),
         }) {
        BSONObj spec =
            BSON(DocumentSourceChangeStreamTransform::kStageName << BSON(
                     "resumeAfter"
                     << makeResumeToken(kDefaultTs, Value(), Value(), DSChangeStream::kInsertOpType)
                     << "supportedEvents" << supportedEvents));

        ASSERT_THROWS_CODE(
            DocumentSourceChangeStreamTransform::createFromBson(spec.firstElement(), getExpCtx()),
            AssertionException,
            10498500);
    }
}

TEST_F(ChangeStreamStageTest, DocumentSourceChangeStreamTransformCannotSetSupportedEventsOnRouter) {
    auto expCtx = getExpCtx();
    expCtx->setInRouter(true);

    BSONObj spec =
        BSON(DocumentSourceChangeStreamTransform::kStageName
             << BSON("resumeAfter"
                     << makeResumeToken(kDefaultTs, Value(), Value(), DSChangeStream::kInsertOpType)
                     << "supportedEvents" << BSON_ARRAY("eventType1")));

    ASSERT_THROWS_CODE(
        DocumentSourceChangeStreamTransform::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        10498501);
}

TEST_F(ChangeStreamStageTest, DocumentSourceChangeStreamTransformTransformSingleSupportedEvent) {
    BSONObj spec =
        BSON(DocumentSourceChangeStreamTransform::kStageName
             << BSON("resumeAfter" << makeResumeToken(kDefaultTs, Value(), Value(), "eventType1"_sd)
                                   << "supportedEvents" << BSON_ARRAY("eventType1")));

    BSONObj operationDescription = BSON("foo" << "bar"
                                              << "baz"
                                              << "qux"
                                              << "sub" << BSON("sub1" << true << "sub2" << false));

    auto entry =
        makeOplogEntry(OpTypeEnum::kNoop,
                       nss,
                       BSONObj(),
                       testUuid(),
                       false,
                       BSON("eventType1" << "willBeRemoved").addFields(operationDescription));

    Document expectedDoc{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), operationDescription, "eventType1"_sd)},
        {DSChangeStream::kOperationTypeField, "eventType1"_sd},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}}};

    auto stage =
        exec::agg::MockStage::createForTest({Document{entry.getEntry().toBSON()}}, getExpCtx());
    auto transformDS =
        DocumentSourceChangeStreamTransform::createFromBson(spec.firstElement(), getExpCtx());
    auto transformStage = exec::agg::buildStage(transformDS);
    transformStage->setSource(stage.get());

    auto next = transformStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDoc);

    next = transformStage->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(ChangeStreamStageTest, DocumentSourceChangeStreamTransformTransformMultipleSupportedEvents) {
    BSONObj spec = BSON(DocumentSourceChangeStreamTransform::kStageName
                        << BSON("resumeAfter"
                                << makeResumeToken(kDefaultTs, Value(), Value(), "eventType1"_sd)
                                << "supportedEvents" << BSON_ARRAY("eventType1" << "eventType2")));

    BSONObj operationDescriptionEvent1 =
        BSON("foo" << "bar"
                   << "baz"
                   << "qux"
                   << "sub" << BSON("sub1" << true << "sub2" << false));
    BSONObj operationDescriptionEvent2 = BSON("some" << BSON("that" << "will"
                                                                    << "end"
                                                                    << "up"
                                                                    << "in"
                                                                    << "result"));

    auto entry1 = makeOplogEntry(OpTypeEnum::kNoop,
                                 nss,
                                 BSONObj(),
                                 testUuid(),
                                 false,
                                 BSON("eventType1" << BSON("will" << "be"
                                                                  << "removed"
                                                                  << "too"))
                                     .addFields(operationDescriptionEvent1));

    auto entry2 = makeOplogEntry(OpTypeEnum::kNoop,
                                 nss,
                                 BSONObj(),
                                 testUuid(),
                                 false,
                                 BSON("eventType2" << true).addFields(operationDescriptionEvent2));

    Document expectedDoc1{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), operationDescriptionEvent1, "eventType1"_sd)},
        {DSChangeStream::kOperationTypeField, "eventType1"_sd},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}}};

    Document expectedDoc2{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), operationDescriptionEvent2, "eventType2"_sd)},
        {DSChangeStream::kOperationTypeField, "eventType2"_sd},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}}};

    std::deque<exec::agg::GetNextResult> docs;
    docs.push_back(Document{entry1.getEntry().toBSON()});
    docs.push_back(Document{entry2.getEntry().toBSON()});
    docs.push_back(Document{entry1.getEntry().toBSON()});
    auto stage = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto transformDS =
        DocumentSourceChangeStreamTransform::createFromBson(spec.firstElement(), getExpCtx());
    auto transformStage = exec::agg::buildStage(transformDS);

    transformStage->setSource(stage.get());

    auto next = transformStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDoc1);

    next = transformStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDoc2);

    next = transformStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDoc1);

    next = transformStage->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(ChangeStreamStageTest,
       DocumentSourceChangeStreamTransformTransformMultipleSupportedEventsExpandedEvents) {
    BSONObj spec =
        BSON(DocumentSourceChangeStreamTransform::kStageName
             << BSON("resumeAfter" << makeResumeToken(kDefaultTs, Value(), Value(), "eventType1"_sd)
                                   << "showExpandedEvents" << true << "supportedEvents"
                                   << BSON_ARRAY("eventType1" << "eventType2")));

    BSONObj operationDescriptionEvent1 =
        BSON("foo" << "bar"
                   << "baz"
                   << "qux"
                   << "sub" << BSON("sub1" << true << "sub2" << false));
    BSONObj operationDescriptionEvent2 = BSON("some" << BSON("that" << "will"
                                                                    << "end"
                                                                    << "up"
                                                                    << "in"
                                                                    << "result"));

    auto entry1 = makeOplogEntry(OpTypeEnum::kNoop,
                                 nss,
                                 BSONObj(),
                                 testUuid(),
                                 false,
                                 BSON("eventType1" << BSON("will" << "be"
                                                                  << "removed"
                                                                  << "too"))
                                     .addFields(operationDescriptionEvent1));

    auto entry2 = makeOplogEntry(OpTypeEnum::kNoop,
                                 nss,
                                 BSONObj(),
                                 testUuid(),
                                 false,
                                 BSON("eventType2" << true).addFields(operationDescriptionEvent2));

    Document expectedDoc1{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), operationDescriptionEvent1, "eventType1"_sd)},
        {DSChangeStream::kOperationTypeField, "eventType1"_sd},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, testUuid()},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kOperationDescriptionField, operationDescriptionEvent1}};

    Document expectedDoc2{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), operationDescriptionEvent2, "eventType2"_sd)},
        {DSChangeStream::kOperationTypeField, "eventType2"_sd},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, testUuid()},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kOperationDescriptionField, operationDescriptionEvent2}};

    std::deque<exec::agg::GetNextResult> docs;
    docs.push_back(Document{entry1.getEntry().toBSON()});
    docs.push_back(Document{entry2.getEntry().toBSON()});
    docs.push_back(Document{entry1.getEntry().toBSON()});
    auto stage = exec::agg::MockStage::createForTest(std::move(docs), getExpCtx());
    auto transformDS =
        DocumentSourceChangeStreamTransform::createFromBson(spec.firstElement(), getExpCtx());
    auto transformStage = exec::agg::buildStage(transformDS);

    transformStage->setSource(stage.get());

    auto next = transformStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDoc1);

    next = transformStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDoc2);

    next = transformStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDoc1);

    next = transformStage->getNext();
    ASSERT_TRUE(next.isEOF());
}

DEATH_TEST_REGEX_F(ChangeStreamStageTest,
                   DocumentSourceChangeStreamTransformTransformUnknownSupportedEvent,
                   "Tripwire assertion.*5052201") {
    BSONObj spec = BSON(DocumentSourceChangeStreamTransform::kStageName
                        << BSON("resumeAfter"
                                << makeResumeToken(kDefaultTs, Value(), Value(), "eventType1"_sd)
                                << "supportedEvents" << BSON_ARRAY("eventType1" << "eventType2")));

    auto entry = makeOplogEntry(OpTypeEnum::kNoop,
                                nss,
                                BSONObj(),
                                testUuid(),
                                false,
                                BSON("unsupportedEventType" << BSONObj()));

    auto stage =
        exec::agg::MockStage::createForTest({Document{entry.getEntry().toBSON()}}, getExpCtx());
    auto transformDS =
        DocumentSourceChangeStreamTransform::createFromBson(spec.firstElement(), getExpCtx());
    auto transformStage = exec::agg::buildStage(transformDS);
    transformStage->setSource(stage.get());

    ASSERT_THROWS_CODE(transformStage->getNext(), AssertionException, 5052201);
}

TEST_F(ChangeStreamStageTest, TransformationShouldBeAbleToReParseSerializedStage) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamSpec spec;
    spec.setStartAtOperationTime(kDefaultTs);
    auto originalSpec = BSON("" << spec.toBSON());

    auto result = DSChangeStream::createFromBson(originalSpec.firstElement(), expCtx);

    std::vector<boost::intrusive_ptr<DocumentSource>> allStages(std::begin(result),
                                                                std::end(result));

    ASSERT_EQ(allStages.size(), 6);

    auto stage = allStages[2];
    ASSERT(dynamic_cast<DocumentSourceChangeStreamTransform*>(stage.get()));

    //
    // Serialize the stage and confirm contents.
    //
    std::vector<Value> serialization;
    stage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_BSONOBJ_EQ(
        serializedDoc[DocumentSourceChangeStreamTransform::kStageName].getDocument().toBson(),
        originalSpec[""].Obj());

    //
    // Create a new stage from the serialization. Serialize the new stage and confirm that it is
    // equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = Pipeline::create(
        DSChangeStream::createFromBson(serializedBson.firstElement(), expCtx), expCtx);
    auto newSerialization = roundTripped->serialize();

    ASSERT_EQ(newSerialization.size(), 6UL);

    // DSCSTransform stage should be the third stage after DSCSOplogMatch and
    // DSCSUnwindTransactions stages.
    ASSERT_VALUE_EQ(newSerialization[2], serialization[0]);
}

TEST_F(ChangeStreamStageTest, DSCSTransformStageEmptySpecSerializeResumeAfter) {
    auto expCtx = getExpCtx();
    auto originalSpec = BSON(DSChangeStream::kStageName << BSONObj());

    // Verify that the 'initialPostBatchResumeToken' is populated while parsing.
    ASSERT(expCtx->getInitialPostBatchResumeToken().isEmpty());
    ON_BLOCK_EXIT([&expCtx] {
        // Reset for the next run.
        expCtx->setInitialPostBatchResumeToken(BSONObj());
    });

    auto result = DSChangeStream::createFromBson(originalSpec.firstElement(), expCtx);
    ASSERT(!expCtx->getInitialPostBatchResumeToken().isEmpty());

    std::vector<boost::intrusive_ptr<DocumentSource>> allStages(std::begin(result),
                                                                std::end(result));
    ASSERT_EQ(allStages.size(), 6);
    auto transformDS = allStages[2];
    ASSERT(dynamic_cast<DocumentSourceChangeStreamTransform*>(transformDS.get()));


    // Verify that an additional start point field is populated while serializing.
    std::vector<Value> serialization;
    transformDS->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);
    ASSERT(!serialization[0]
                .getDocument()[DocumentSourceChangeStreamTransform::kStageName]
                .getDocument()[DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName]
                .missing());
}

TEST_F(ChangeStreamStageTest, DSCSTransformStageWithResumeTokenSerialize) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(ResumeToken::parse(
        makeResumeToken(kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)));
    auto originalSpec = BSON("" << spec.toBSON());

    // Verify that the 'initialPostBatchResumeToken' is populated while parsing.
    ASSERT(expCtx->getInitialPostBatchResumeToken().isEmpty());
    ON_BLOCK_EXIT([&expCtx] {
        // Reset for the next run.
        expCtx->setInitialPostBatchResumeToken(BSONObj());
    });

    auto stage =
        DocumentSourceChangeStreamTransform::createFromBson(originalSpec.firstElement(), expCtx);
    ASSERT(!expCtx->getInitialPostBatchResumeToken().isEmpty());

    std::vector<Value> serialization;
    stage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);
    ASSERT_BSONOBJ_EQ(serialization[0]
                          .getDocument()[DocumentSourceChangeStreamTransform::kStageName]
                          .getDocument()
                          .toBson(),
                      originalSpec[""].Obj());
}

template <typename Stage, typename StageSpec>
void validateDocumentSourceStageSerialization(
    StageSpec spec, BSONObj specAsBSON, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto stage = Stage::createFromBson(specAsBSON.firstElement(), expCtx);
    std::vector<Value> serialization;
    stage->serializeToArray(serialization);

    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);
    ASSERT_BSONOBJ_EQ(serialization[0].getDocument().toBson(),
                      BSON(Stage::kStageName << spec.toBSON()));
}

std::pair<DocumentSourceChangeStreamInjectControlEventsSpec, BSONObj>
buildControlEventsSpecFromBSON(const BSONObj& actions) {
    DocumentSourceChangeStreamInjectControlEventsSpec spec;
    spec.setActions(actions);
    return std::make_pair(spec, BSON("" << spec.toBSON()));
}

TEST_F(ChangeStreamStageTest, DSCSInjectControlEventsStageSerialization) {
    // No actions specified.
    {
        auto [spec, stageSpecAsBSON] = buildControlEventsSpecFromBSON(BSONObj());
        validateDocumentSourceStageSerialization<DocumentSourceChangeStreamInjectControlEvents>(
            std::move(spec), stageSpecAsBSON, getExpCtx());
    }

    // Test some valid actions.
    {
        auto [spec, stageSpecAsBSON] =
            buildControlEventsSpecFromBSON(BSON("event1" << "transformToControlEvent"
                                                         << "event2"
                                                         << "injectControlEvent"));
        validateDocumentSourceStageSerialization<DocumentSourceChangeStreamInjectControlEvents>(
            std::move(spec), stageSpecAsBSON, getExpCtx());
    }

    // Test serializing for explain.
    {
        auto actions = BSON("event1" << "transformToControlEvent"
                                     << "event2"
                                     << "injectControlEvent");
        auto [spec, stageSpecAsBSON] = buildControlEventsSpecFromBSON(actions);
        auto stage = DocumentSourceChangeStreamInjectControlEvents::createFromBson(
            stageSpecAsBSON.firstElement(), getExpCtx());
        std::vector<Value> serialization;
        SerializationOptions options;
        options.verbosity = ExplainOptions::Verbosity::kQueryPlanner;
        stage->serializeToArray(serialization, options);

        ASSERT_EQ(serialization.size(), 1UL);
        ASSERT_EQ(serialization[0].getType(), BSONType::object);

        ASSERT_BSONOBJ_EQ(serialization[0].getDocument().toBson(),
                          BSON("$changeStream"
                               << BSON("stage"
                                       << DocumentSourceChangeStreamInjectControlEvents::kStageName
                                       << "actions" << actions)));
    }
}

DEATH_TEST_REGEX_F(ChangeStreamStageTest,
                   DSCSInjectControlEventsStageSerializationInvalidInputType,
                   "Tripwire assertion.*10384001") {
    // Test invalid top-level BSON type.
    auto [spec, stageSpecAsBSON] = buildControlEventsSpecFromBSON(BSON_ARRAY(1));
    ASSERT_THROWS_CODE(
        validateDocumentSourceStageSerialization<DocumentSourceChangeStreamInjectControlEvents>(
            std::move(spec), stageSpecAsBSON, getExpCtx()),
        AssertionException,
        10384001);
}

DEATH_TEST_REGEX_F(ChangeStreamStageTest,
                   DSCSInjectControlEventsStageSerializationInvalidActionInputs,
                   "Tripwire assertion.*10384001") {
    // Test invalid actions types.
    {
        for (const BSONObj& value : {BSON("" << 1234), BSON("" << true), BSONObj()}) {
            auto [spec, stageSpecAsBSON] =
                buildControlEventsSpecFromBSON(BSON("event" << value.firstElement()));
            ASSERT_THROWS_CODE(validateDocumentSourceStageSerialization<
                                   DocumentSourceChangeStreamInjectControlEvents>(
                                   std::move(spec), stageSpecAsBSON, getExpCtx()),
                               AssertionException,
                               10384001);
        }
    }

    // Test invalid actions values.
    {
        for (StringData value : {"", " ", "foo", "dum dee dum", "INJECTCONTROLEVENT"}) {
            auto [spec, stageSpecAsBSON] = buildControlEventsSpecFromBSON(BSON("event" << value));
            ASSERT_THROWS_CODE(validateDocumentSourceStageSerialization<
                                   DocumentSourceChangeStreamInjectControlEvents>(
                                   std::move(spec), stageSpecAsBSON, getExpCtx()),
                               AssertionException,
                               10384001);
        }
    }
}

DEATH_TEST_REGEX_F(ChangeStreamStageTest,
                   DSCSInjectControlEventsStageSerializationDuplicateEvents,
                   "Tripwire assertion.*10384002") {
    // Test duplicate events in spec.
    auto [spec, stageSpecAsBSON] =
        buildControlEventsSpecFromBSON(BSON("event1" << "injectControlEvent"
                                                     << "event1"
                                                     << "transformToControlEvent"));
    ASSERT_THROWS_CODE(
        validateDocumentSourceStageSerialization<DocumentSourceChangeStreamInjectControlEvents>(
            std::move(spec), stageSpecAsBSON, getExpCtx()),
        AssertionException,
        10384002);
}

TEST_F(ChangeStreamStageTest, InjectControlEventsHandlesNonMatchingInputsCorrectly) {
    auto expCtx = getExpCtx();

    const BSONObj doc1 = BSON("operationType" << "test1"
                                              << "foo"
                                              << "bar");
    const BSONObj doc2 = BSON("operationType" << "test2"
                                              << "test"
                                              << "value");

    // Test the control events stage with different configurations.
    for (const BSONObj& config : {
             BSONObj(),
             BSON("eventType1" << "injectControlEvent"
                               << "eventType2"
                               << "transformToControlEvent"),
         }) {
        auto [_, stageSpecAsBSON] = buildControlEventsSpecFromBSON(config);

        auto injectControlEvents = DocumentSourceChangeStreamInjectControlEvents::createFromBson(
            stageSpecAsBSON.firstElement(), expCtx);

        std::deque<DocumentSource::GetNextResult> inputDocs = {
            DocumentSource::GetNextResult::makePauseExecution(),
            Document::fromBsonWithMetaData(doc1),
            DocumentSource::GetNextResult::makePauseExecution(),
            Document::fromBsonWithMetaData(doc2),
            DocumentSource::GetNextResult::makeEOF(),
        };

        auto stage = exec::agg::MockStage::createForTest(inputDocs, expCtx);
        auto injectControlEventsStage = exec::agg::buildStage(injectControlEvents);
        injectControlEventsStage->setSource(stage.get());

        auto next = injectControlEventsStage->getNext();
        ASSERT_TRUE(next.isPaused());

        next = injectControlEventsStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(doc1), next.getDocument());

        next = injectControlEventsStage->getNext();
        ASSERT_TRUE(next.isPaused());

        next = injectControlEventsStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(doc2), next.getDocument());

        next = injectControlEventsStage->getNext();
        ASSERT_TRUE(next.isEOF());
    }
}

TEST_F(ChangeStreamStageTest, InjectControlEventsHandlesMatchingInputsCorrectly) {
    auto expCtx = getExpCtx();

    auto [_, stageSpecAsBSON] =
        buildControlEventsSpecFromBSON(BSON("eventType1" << "injectControlEvent"
                                                         << "eventType2"
                                                         << "transformToControlEvent"));

    auto injectControlEvents = DocumentSourceChangeStreamInjectControlEvents::createFromBson(
        stageSpecAsBSON.firstElement(), expCtx);

    BSONObj doc1 = BSON("operationType" << "test1"
                                        << "foo"
                                        << "bar");
    BSONObj doc2 = BSON("operationType" << "test2"
                                        << "test"
                                        << "value");
    BSONObj doc3 = BSON("operationType" << "test3"
                                        << "baz"
                                        << "qux");
    BSONObj ctrl1 = BSON("operationType" << "eventType1"
                                         << "value" << 1234);
    BSONObj ctrl2 = BSON("operationType" << "eventType2"
                                         << "value"
                                         << "test");
    BSONObj ctrl3 = BSON("operationType" << "eventType1"
                                         << "value" << BSONObj());

    std::deque<DocumentSource::GetNextResult> inputDocs = {
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc1),
        Document::fromBsonWithMetaData(ctrl1),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document::fromBsonWithMetaData(doc2),
        Document::fromBsonWithMetaData(ctrl2),
        Document::fromBsonWithMetaData(doc3),
        Document::fromBsonWithMetaData(ctrl3),
        DocumentSource::GetNextResult::makeEOF(),
    };

    auto stage = exec::agg::MockStage::createForTest(inputDocs, expCtx);
    auto injectControlEventsStage = exec::agg::buildStage(injectControlEvents);
    injectControlEventsStage->setSource(stage.get());

    auto next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isPaused());

    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(doc1), next.getDocument());

    // This document leads to injecting a follow-up control event.
    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(ctrl1), next.getDocument());

    // The injected control event.
    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isAdvancedControlDocument());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(ctrl1), next.getDocument());

    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isPaused());

    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(doc2), next.getDocument());

    // This document gets transformed into a control event.
    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isAdvancedControlDocument());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(ctrl2), next.getDocument());

    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(doc3), next.getDocument());

    // This document leads to injecting a follow-up control event.
    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(ctrl3), next.getDocument());

    // The injected control event.
    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isAdvancedControlDocument());
    ASSERT_DOCUMENT_EQ(Document::fromBsonWithMetaData(ctrl3), next.getDocument());

    next = injectControlEventsStage->getNext();
    ASSERT_TRUE(next.isEOF());
}

TEST_F(ChangeStreamStageTest, DSCSOplogMatchStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamOplogMatchSpec spec;
    auto dummyFilter = BSON("a" << 1);
    spec.setFilter(dummyFilter);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamOplogMatch>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSUnwindTransactionStageSerialization) {
    auto expCtx = getExpCtx();

    auto filter = BSON("ns" << BSON("$regex" << "^db\\.coll$"));
    DocumentSourceChangeStreamUnwindTransactionSpec spec{std::move(filter)};
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamUnwindTransaction>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSCheckInvalidateStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamCheckInvalidateSpec spec;
    spec.setStartAfterInvalidate(
        ResumeToken::parse(makeResumeToken(kDefaultTs,
                                           testUuid(),
                                           Value(),
                                           DSChangeStream::kDropCollectionOpType,
                                           ResumeTokenData::FromInvalidate::kFromInvalidate)));
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamCheckInvalidate>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSResumabilityStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamCheckResumabilitySpec spec;
    spec.setResumeToken(ResumeToken::parse(
        makeResumeToken(kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)));
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamCheckResumability>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSLookupChangePreImageStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamAddPreImageSpec spec(FullDocumentBeforeChangeModeEnum::kRequired);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamAddPreImage>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, DSCSLookupChangePostImageStageSerialization) {
    auto expCtx = getExpCtx();

    DocumentSourceChangeStreamAddPostImageSpec spec(FullDocumentModeEnum::kUpdateLookup);
    auto stageSpecAsBSON = BSON("" << spec.toBSON());

    validateDocumentSourceStageSerialization<DocumentSourceChangeStreamAddPostImage>(
        std::move(spec), stageSpecAsBSON, expCtx);
}

TEST_F(ChangeStreamStageTest, CloseCursorOnInvalidateEntries) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    auto execPipeline = makeExecPipeline(dropColl);
    auto lastStage = execPipeline->getStages().back();

    Document expectedDrop{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         testUuid(),
                         Value(),
                         DSChangeStream::kDropCollectionOpType,
                         ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
    };

    auto next = lastStage->getNext();
    // Transform into drop entry.
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDrop);
    next = lastStage->getNext();
    // Transform into invalidate entry.
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedInvalidate);

    // Then throw an exception on the next call of getNext().
    ASSERT_THROWS(lastStage->getNext(), ExceptionFor<ErrorCodes::ChangeStreamInvalidated>);
}

TEST_F(ChangeStreamStageTest, CloseCursorEvenIfInvalidateEntriesGetFilteredOut) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    auto execPipeline = makeExecPipeline(dropColl);
    auto lastStage = execPipeline->getStages().back();
    // Add a match stage after change stream to filter out the invalidate entries.
    auto match = DocumentSourceMatch::create(fromjson("{operationType: 'insert'}"), getExpCtx());

    auto matchStage = exec::agg::buildStage(match);
    matchStage->setSource(lastStage.get());

    // Throw an exception on the call of getNext().
    ASSERT_THROWS(matchStage->getNext(), ExceptionFor<ErrorCodes::ChangeStreamInvalidated>);
}

TEST_F(ChangeStreamStageTest, DocumentKeyShouldNotIncludeShardKeyWhenNoO2FieldInOplog) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    {
        Lock::GlobalWrite lk(getExpCtx()->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(uuid, nss);
        CollectionCatalog::write(
            getExpCtx()->getOperationContext(), [&](CollectionCatalog& catalog) {
                catalog.registerCollection(
                    getExpCtx()->getOperationContext(), std::move(collection), /*ts=*/boost::none);
            });
    }


    BSONObj docKey = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, docKey, DSChangeStream::kInsertOpType);

    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    // The documentKey should have just an _id in this case.
    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(ts, uuid, D{{"_id", 2}}, DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry, expectedInsert, BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));

    // Verify the same behavior with resuming using 'startAfter'.
    checkTransformation(
        insertEntry, expectedInsert, BSON("$changeStream" << BSON("startAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageTest, DocumentKeyShouldUseO2FieldInOplog) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    {
        Lock::GlobalWrite lk(getExpCtx()->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(uuid, nss);
        CollectionCatalog::write(
            getExpCtx()->getOperationContext(), [&](CollectionCatalog& catalog) {
                catalog.registerCollection(
                    getExpCtx()->getOperationContext(), std::move(collection), /*ts=*/boost::none);
            });
    }


    BSONObj docKey = BSON("_id" << 1);
    auto resumeToken = makeResumeToken(ts, uuid, docKey, DSChangeStream::kInsertOpType);

    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    BSONObj o2 = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      o2,                   // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(ts, uuid, insertDoc, DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}, {"shardKey", 3}}},
    };
    // When o2 is present in the oplog entry, we should use its value for the document key, even if
    // the resume token doesn't contain shard key.
    checkTransformation(
        insertEntry, expectedInsert, BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));

    // Verify the same behavior with resuming using 'startAfter'.
    checkTransformation(
        insertEntry, expectedInsert, BSON("$changeStream" << BSON("startAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageTest, ResumeAfterFailsIfResumeTokenDoesNotContainUUID) {
    const Timestamp ts(3, 45);

    {
        Lock::GlobalWrite lk(getExpCtx()->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
        CollectionCatalog::write(
            getExpCtx()->getOperationContext(), [&](CollectionCatalog& catalog) {
                catalog.registerCollection(
                    getExpCtx()->getOperationContext(), std::move(collection), /*ts=*/boost::none);
            });
    }

    // Create a resume token from only the timestamp.
    auto resumeToken = makeResumeToken(ts, Value(), Value(), DSChangeStream::kDropCollectionOpType);

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << resumeToken)).firstElement(),
            getExpCtx()),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageTest, RenameFromSystemToUserCollectionShouldIncludeNotification) {
    // Renaming to a non-system collection will include a notification in the stream.
    NamespaceString systemColl =
        NamespaceString::createNamespaceString_forTest(nss.dbName(), "system.users");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << systemColl.ns_forTest() << "to" << nss.ns_forTest()),
        testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    const auto opDesc = Value(D{{"to", D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}}});
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), opDesc, DSChangeStream::kRenameCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField,
         D{{"db", systemColl.db_forTest()}, {"coll", systemColl.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageTest, RenameFromUserToSystemCollectionShouldIncludeNotification) {
    // Renaming to a system collection will include a notification in the stream.
    NamespaceString systemColl =
        NamespaceString::createNamespaceString_forTest(nss.dbName(), "system.users");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << nss.ns_forTest() << "to" << systemColl.ns_forTest()),
        testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    const auto opDesc =
        Value(D{{"to", D{{"db", systemColl.db_forTest()}, {"coll", systemColl.coll()}}}});
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", systemColl.db_forTest()}, {"coll", systemColl.coll()}}},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), opDesc, DSChangeStream::kRenameCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageTest, ResumeAfterWithTokenFromInvalidateShouldFail) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the collection catalog so the resume token is valid.
    {
        Lock::GlobalWrite lk(getExpCtx()->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
        CollectionCatalog::write(expCtx->getOperationContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(
                getExpCtx()->getOperationContext(), std::move(collection), /*ts=*/boost::none);
        });
    }

    const auto resumeTokenInvalidate =
        makeResumeToken(kDefaultTs,
                        testUuid(),
                        BSON("x" << 2 << "_id" << 1),
                        DSChangeStream::kInsertOpType,
                        ResumeTokenData::FromInvalidate::kFromInvalidate);

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << resumeTokenInvalidate))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageTest, UsesResumeTokenAsSortKeyIfNeedsMergeIsFalse) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,            // op type
                                 nss,                            // namespace
                                 BSON("x" << 2 << "_id" << 1),   // o
                                 testUuid(),                     // uuid
                                 boost::none,                    // fromMigrate
                                 BSON("x" << 2 << "_id" << 1));  // o2

    auto execPipeline = makeExecPipeline(insert.getEntry().toBSON(), kDefaultSpec);

    getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>());

    getExpCtx()->setNeedsMerge(false);

    auto next = execPipeline->getStages().back()->getNext();

    auto expectedSortKey = makeResumeToken(
        kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1), DSChangeStream::kInsertOpType);

    ASSERT_TRUE(next.isAdvanced());
    ASSERT_VALUE_EQ(next.releaseDocument().metadata().getSortKey(), Value(expectedSortKey));
}

//
// Test class for change stream of a single database.
//
class ChangeStreamStageDBTest : public ChangeStreamStageTest {
public:
    ChangeStreamStageDBTest()
        : ChangeStreamStageTest(NamespaceString::makeCollectionlessAggregateNSS(nss.dbName())) {}
};

TEST_F(ChangeStreamStageDBTest, TransformInsert) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,
                                 nss,
                                 BSON("_id" << 1 << "x" << 2),
                                 testUuid(),
                                 boost::none,
                                 BSON("x" << 2 << "_id" << 1));

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1), DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert);
}

TEST_F(ChangeStreamStageDBTest, TransformInsertShowExpandedEvents) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,
                                 nss,
                                 BSON("_id" << 1 << "x" << 2),
                                 testUuid(),
                                 boost::none,
                                 BSON("x" << 2 << "_id" << 1));

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1), DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kCollectionUuidField, testUuid()},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert, kShowExpandedEventsSpec);
}

TEST_F(ChangeStreamStageDBTest, InsertOnOtherCollections) {
    NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest("unittests.other_collection.");
    auto insertOtherColl = makeOplogEntry(OpTypeEnum::kInsert,
                                          otherNss,
                                          BSON("_id" << 1 << "x" << 2),
                                          testUuid(),
                                          boost::none,
                                          BSON("x" << 2 << "_id" << 1));

    // Insert on another collection in the same database.
    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(
             kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1), DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField,
         D{{"db", otherNss.db_forTest()}, {"coll", otherNss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insertOtherColl, expectedInsert);
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersChangesOnOtherDatabases) {
    std::set<NamespaceString> unmatchedNamespaces = {
        // Namespace starts with the db name, but is longer.
        NamespaceString::createNamespaceString_forTest("unittests2.coll"),
        // Namespace contains the db name, but not at the front.
        NamespaceString::createNamespaceString_forTest("test.unittests"),
        // Namespace contains the db name + dot.
        NamespaceString::createNamespaceString_forTest("test.unittests.coll"),
        // Namespace contains the db name + dot but is followed by $.
        NamespaceString::createNamespaceString_forTest("unittests.$cmd"),
    };

    // Insert into another database.
    for (auto& ns : unmatchedNamespaces) {
        auto insert = makeOplogEntry(OpTypeEnum::kInsert, ns, BSON("_id" << 1));
        checkTransformation(insert, boost::none);
    }
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersAllSystemDotCollections) {
    auto nss = NamespaceString::createNamespaceString_forTest("unittests.system.coll");
    auto insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString::createNamespaceString_forTest("unittests.system.users");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString::createNamespaceString_forTest("unittests.system.roles");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString::createNamespaceString_forTest("unittests.system.keys");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageDBTest, TransformsEntriesForLegalClientCollectionsWithSystem) {
    std::set<NamespaceString> allowedNamespaces = {
        NamespaceString::createNamespaceString_forTest("unittests.coll.system"),
        NamespaceString::createNamespaceString_forTest("unittests.coll.system.views"),
        NamespaceString::createNamespaceString_forTest("unittests.systemx"),
    };

    for (auto& ns : allowedNamespaces) {
        auto insert = makeOplogEntry(
            OpTypeEnum::kInsert, ns, BSON("_id" << 1), testUuid(), boost::none, BSON("_id" << 1));
        Document expectedInsert{
            {DSChangeStream::kIdField,
             makeResumeToken(
                 kDefaultTs, testUuid(), BSON("_id" << 1), DSChangeStream::kInsertOpType)},
            {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
            {DSChangeStream::kClusterTimeField, kDefaultTs},
            {DSChangeStream::kWallTimeField, Date_t()},
            {DSChangeStream::kFullDocumentField, D{{"_id", 1}}},
            {DSChangeStream::kNamespaceField, D{{"db", ns.db_forTest()}, {"coll", ns.coll()}}},
            {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
        };
        checkTransformation(insert, expectedInsert);
    }
}

TEST_F(ChangeStreamStageDBTest, TransformUpdateFieldsVMissingNotSupported) {
    // A missing $v field in the update oplog entry implies $v:1, which is no longer supported.
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate, nss, o, testUuid(), boost::none, o2);
    checkTransformation(updateField, boost::none, kDefaultSpec, {}, {}, {}, 6741200);
}

TEST_F(ChangeStreamStageDBTest, TransformUpdateFieldsNonV2NotSupported) {
    BSONObj diff = BSON("u" << BSON("y" << 1));
    BSONObj o = BSON("diff" << diff << "$v" << 3);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate, nss, o, testUuid(), boost::none, o2);
    checkTransformation(updateField, boost::none, kDefaultSpec, {}, {}, {}, 6741200);
}

TEST_F(ChangeStreamStageDBTest, TransformUpdateFields) {
    BSONObj diff = BSON("u" << BSON("y" << 1));
    BSONObj o = BSON("diff" << diff << "$v" << 2);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate, nss, o, testUuid(), boost::none, o2);

    const auto expectedUpdateField =
        makeExpectedUpdateEvent(kDefaultTs,
                                nss,
                                o2,
                                D{{"updatedFields", D{{"y", 1}}},
                                  {"removedFields", std::vector<V>()},
                                  {"truncatedArrays", std::vector<V>()}});
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageDBTest, TransformRemoveFields) {
    BSONObj diff = BSON("d" << BSON("y" << false));
    BSONObj o = BSON("diff" << diff << "$v" << 2);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto removeField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    const auto expectedRemoveField =
        makeExpectedUpdateEvent(kDefaultTs,
                                nss,
                                o2,
                                D{{"updatedFields", D{}},
                                  {"removedFields", {"y"_sd}},
                                  {"truncatedArrays", std::vector<V>()}});
    checkTransformation(removeField, expectedRemoveField);
}

TEST_F(ChangeStreamStageDBTest, TransformReplace) {
    BSONObj o = BSON("_id" << 1 << "x" << 2 << "y" << 1);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto replace = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                  nss,                  // namespace
                                  o,                    // o
                                  testUuid(),           // uuid
                                  boost::none,          // fromMigrate
                                  o2);                  // o2

    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), o2, DSChangeStream::kReplaceOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeStreamStageDBTest, TransformDelete) {
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), o, DSChangeStream::kDeleteOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),    // op type
                                       deleteEntry.getNss(),       // namespace
                                       deleteEntry.getObject(),    // o
                                       deleteEntry.getUuid(),      // uuid
                                       fromMigrate,                // fromMigrate
                                       deleteEntry.getObject2());  // o2

    checkTransformation(deleteEntry2, expectedDelete);
}

TEST_F(ChangeStreamStageDBTest, TransformDeleteFromMigrate) {
    bool fromMigrate = true;
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      BSON("_id" << 1),     // o
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    checkTransformation(deleteEntry, boost::none);
}

TEST_F(ChangeStreamStageDBTest, TransformDeleteFromMigrateShowMigrations) {
    bool fromMigrate = true;
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    auto spec = fromjson("{$changeStream: {showMigrationEvents: true}}");
    Document expectedDelete{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), o, DSChangeStream::kDeleteOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };

    checkTransformation(deleteEntry, expectedDelete, spec);
}

TEST_F(ChangeStreamStageDBTest, TransformDrop) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    Document expectedDrop{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };
    checkTransformation(dropColl, expectedDrop);
}

TEST_F(ChangeStreamStageDBTest, TransformRename) {
    NamespaceString otherColl = NamespaceString::createNamespaceString_forTest("test.bar");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << nss.ns_forTest() << "to" << otherColl.ns_forTest()), testUuid());

    const auto opDesc =
        Value(D{{"to", D{{"db", otherColl.db_forTest()}, {"coll", otherColl.coll()}}}});
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", otherColl.db_forTest()}, {"coll", otherColl.coll()}}},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), opDesc, DSChangeStream::kRenameCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageDBTest, TransformDropDatabase) {
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, false);

    // Drop database entry doesn't have a UUID.
    Document expectedDropDatabase{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, Value(), Value(), DSChangeStream::kDropDatabaseOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropDatabaseOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         Value(),
                         Value(),
                         DSChangeStream::kDropDatabaseOpType,
                         ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
    };

    checkTransformation(dropDB, expectedDropDatabase, kDefaultSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageDBTest, TransformDropDatabaseShowExpandedEvents) {
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, false);

    // Drop database entry doesn't have a UUID.
    Document expectedDropDatabase{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, Value(), Value(), DSChangeStream::kDropDatabaseOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDropDatabaseOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}}},
    };
    Document expectedInvalidate{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs,
                         Value(),
                         Value(),
                         DSChangeStream::kDropDatabaseOpType,
                         ResumeTokenData::FromInvalidate::kFromInvalidate)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
    };

    checkTransformation(dropDB, expectedDropDatabase, kShowExpandedEventsSpec, expectedInvalidate);
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersOperationsOnSystemCollections) {
    NamespaceString systemColl =
        NamespaceString::createNamespaceString_forTest(nss.dbName(), "system.users");
    OplogEntry insert = makeOplogEntry(OpTypeEnum::kInsert, systemColl, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    OplogEntry dropColl = createCommand(BSON("drop" << systemColl.coll()), testUuid());
    checkTransformation(dropColl, boost::none);

    // Rename from a 'system' collection to another 'system' collection should not include a
    // notification.
    NamespaceString renamedSystemColl(NamespaceString::makeSystemDotViewsNamespace(nss.dbName()));
    OplogEntry rename = createCommand(BSON("renameCollection" << systemColl.ns_forTest() << "to"
                                                              << renamedSystemColl.ns_forTest()),
                                      testUuid());
    checkTransformation(rename, boost::none);
}

TEST_F(ChangeStreamStageDBTest, RenameFromSystemToUserCollectionShouldIncludeNotification) {
    // Renaming to a non-system collection will include a notification in the stream.
    NamespaceString systemColl =
        NamespaceString::createNamespaceString_forTest(nss.dbName(), "system.users");
    NamespaceString renamedColl =
        NamespaceString::createNamespaceString_forTest(nss.dbName(), "non_system_coll");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << systemColl.ns_forTest() << "to" << renamedColl.ns_forTest()),
        testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    const auto opDesc =
        Value(D{{"to", D{{"db", renamedColl.db_forTest()}, {"coll", renamedColl.coll()}}}});
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", renamedColl.db_forTest()}, {"coll", renamedColl.coll()}}},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), opDesc, DSChangeStream::kRenameCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField,
         D{{"db", systemColl.db_forTest()}, {"coll", systemColl.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageDBTest, RenameFromUserToSystemCollectionShouldIncludeNotification) {
    // Renaming to a system collection will include a notification in the stream.
    NamespaceString systemColl =
        NamespaceString::createNamespaceString_forTest(nss.dbName(), "system.users");
    OplogEntry rename = createCommand(
        BSON("renameCollection" << nss.ns_forTest() << "to" << systemColl.ns_forTest()),
        testUuid());

    // Note that the collection rename does *not* have the queued invalidated field.
    const auto opDesc =
        Value(D{{"to", D{{"db", systemColl.db_forTest()}, {"coll", systemColl.coll()}}}});
    Document expectedRename{
        {DSChangeStream::kRenameTargetNssField,
         D{{"db", systemColl.db_forTest()}, {"coll", systemColl.coll()}}},
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), opDesc, DSChangeStream::kRenameCollectionOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kRenameCollectionOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
    };
    checkTransformation(rename, expectedRename);
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersNoOp) {
    OplogEntry noOp = makeOplogEntry(OpTypeEnum::kNoop,
                                     NamespaceString::kEmpty,
                                     BSON(repl::kNewPrimaryMsgField << repl::kNewPrimaryMsg));
    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageDBTest, DocumentKeyShouldNotIncludeShardKeyWhenNoO2FieldInOplog) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    {
        Lock::GlobalWrite lk(getExpCtx()->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(uuid, nss);
        CollectionCatalog::write(
            getExpCtx()->getOperationContext(), [&](CollectionCatalog& catalog) {
                catalog.registerCollection(
                    getExpCtx()->getOperationContext(), std::move(collection), /*ts=*/boost::none);
            });
    }

    BSONObj docKey = BSON("_id" << 1 << "shardKey" << 2);
    auto resumeToken = makeResumeToken(ts, uuid, docKey, DSChangeStream::kInsertOpType);

    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      boost::none,          // o2
                                      opTime);              // opTime

    // The documentKey should just have an _id in this case.
    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(ts, uuid, D{{"_id", 2}}, DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry, expectedInsert, BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageDBTest, DocumentKeyShouldUseO2FieldInOplog) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);
    const auto uuid = testUuid();

    {
        Lock::GlobalWrite lk(getExpCtx()->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(uuid, nss);
        CollectionCatalog::write(
            getExpCtx()->getOperationContext(), [&](CollectionCatalog& catalog) {
                catalog.registerCollection(
                    getExpCtx()->getOperationContext(), std::move(collection), /*ts=*/boost::none);
            });
    }

    BSONObj docKey = BSON("_id" << 1);
    auto resumeToken = makeResumeToken(ts, uuid, docKey, DSChangeStream::kInsertOpType);

    BSONObj insertDoc = BSON("_id" << 2 << "shardKey" << 3);
    BSONObj o2 = BSON("_id" << 2 << "shardKey" << 3);
    auto insertEntry = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      nss,                  // namespace
                                      insertDoc,            // o
                                      uuid,                 // uuid
                                      boost::none,          // fromMigrate
                                      o2,                   // o2
                                      opTime);              // opTime

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(ts, uuid, insertDoc, DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}, {"shardKey", 3}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}, {"shardKey", 3}}},
    };
    // When o2 is present in the oplog entry, we should use its value for the document key, even if
    // the resume token doesn't contain shard key.
    checkTransformation(
        insertEntry, expectedInsert, BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}

TEST_F(ChangeStreamStageDBTest, ResumeAfterWithTokenFromInvalidateShouldFail) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the collection catalog so the resume token is valid.
    {
        Lock::GlobalWrite lk(getExpCtx()->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
        CollectionCatalog::write(expCtx->getOperationContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(
                getExpCtx()->getOperationContext(), std::move(collection), /*ts=*/boost::none);
        });
    }

    const auto resumeTokenInvalidate =
        makeResumeToken(kDefaultTs,
                        testUuid(),
                        BSON("x" << 2 << "_id" << 1),
                        DSChangeStream::kInsertOpType,
                        ResumeTokenData::FromInvalidate::kFromInvalidate);

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("resumeAfter" << resumeTokenInvalidate))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::InvalidResumeToken);
}

TEST_F(ChangeStreamStageDBTest, ResumeAfterWithTokenFromDropDatabase) {
    const auto uuid = testUuid();

    {
        Lock::GlobalWrite lk(getExpCtx()->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(uuid, nss);
        CollectionCatalog::write(
            getExpCtx()->getOperationContext(), [&](CollectionCatalog& catalog) {
                catalog.registerCollection(
                    getExpCtx()->getOperationContext(), std::move(collection), /*ts=*/boost::none);
            });
    }

    // Create a resume token from only the timestamp, similar to a 'dropDatabase' entry.
    auto resumeToken = makeResumeToken(kDefaultTs,
                                       Value(),
                                       Value(),
                                       DSChangeStream::kInsertOpType,
                                       ResumeTokenData::FromInvalidate::kNotFromInvalidate);

    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry =
        makeOplogEntry(OpTypeEnum::kInsert, nss, insertDoc, testUuid(), boost::none, insertDoc);

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), insertDoc, DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry, expectedInsert, BSON("$changeStream" << BSON("resumeAfter" << resumeToken)));
}


TEST_F(ChangeStreamStageDBTest, StartAfterSucceedsEvenIfResumeTokenDoesNotContainUUID) {
    const auto uuid = testUuid();

    {
        Lock::GlobalWrite lk(getExpCtx()->getOperationContext());
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(uuid, nss);
        CollectionCatalog::write(
            getExpCtx()->getOperationContext(), [&](CollectionCatalog& catalog) {
                catalog.registerCollection(
                    getExpCtx()->getOperationContext(), std::move(collection), /*ts=*/boost::none);
            });
    }

    // Create a resume token from only the timestamp, similar to a 'dropDatabase' entry.
    auto resumeToken =
        makeResumeToken(kDefaultTs, Value(), Value(), DSChangeStream::kDropDatabaseOpType);

    BSONObj insertDoc = BSON("_id" << 2);
    auto insertEntry =
        makeOplogEntry(OpTypeEnum::kInsert, nss, insertDoc, uuid, boost::none, insertDoc);

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, uuid, insertDoc, DSChangeStream::kInsertOpType)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kWallTimeField, Date_t()},
        {DSChangeStream::kFullDocumentField, D{{"_id", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db_forTest()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 2}}},
    };
    checkTransformation(
        insertEntry, expectedInsert, BSON("$changeStream" << BSON("startAfter" << resumeToken)));
}

//
// Tests that the single '$match' gets promoted before the
// '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleMatch) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        kShowExpandedEventsSpec,
        fromjson("{$match: {operationType: 'insert'}}"),
    };

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that multiple '$match' gets merged and promoted before the
// '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleMatch) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$match: {operationType: 'insert'}}"),
                                              fromjson("{$match: {operationType: 'delete'}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that multiple '$match' gets merged and promoted before the
// '$_internalChangeStreamCheckTopologyChange' when resume token is present.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleMatchAndResumeToken) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream"
             << BSON("resumeAfter"
                     << makeResumeToken(
                            kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)
                     << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true)),
        BSON("$match" << BSON("operationType" << "insert")),
        BSON("$match" << BSON("operationType" << "insert"))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

//
// Tests that the single '$project' gets promoted before the
// '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleProject) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$project: {operationType: 1}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that multiple '$project' gets promoted before the
// '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleProject) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$project: {operationType: 1}}"),
                                              fromjson("{$project: {fullDocument: 1}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that multiple '$project' gets promoted before the
// '$_internalChangeStreamHandleTopologyChange' if resume token is present.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleProjectAndResumeToken) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream"
             << BSON("resumeAfter"
                     << makeResumeToken(
                            kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)
                     << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true)),
        BSON("$project" << BSON("operationType" << 1)),
        BSON("$project" << BSON("fullDocument" << 1))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

//
// Tests that a '$project' followed by a '$match' gets optimized and they get promoted before
// the '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithProjectMatchAndResumeToken) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream"
             << BSON("resumeAfter"
                     << makeResumeToken(
                            kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)
                     << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true)),
        BSON("$project" << BSON("operationType" << 1)),
        BSON("$match" << BSON("operationType" << "insert"))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

//
// Tests that the single '$unset' gets promoted before the
// '$_internalChangeStreamCheckTopologyChange' as
// '$project'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleUnset) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$unset: 'operationType'}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that multiple '$unset' gets promoted before the '$_internalChangeStreamCheckTopologyChange'
// as '$project'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleUnset) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$unset: 'operationType'}"),
                                              fromjson("{$unset: 'fullDocument'}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           // The two '$unset' stages are coalesced.
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that the '$unset' gets promoted before the '$_internalChangeStreamCheckTopologyChange' as
// '$project' even if resume token is present.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithUnsetAndResumeToken) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream"
             << BSON("resumeAfter"
                     << makeResumeToken(
                            kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)
                     << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true)),
        BSON("$unset" << "operationType")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

//
// Tests that the single'$addFields' gets promoted before the
// '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleAddFields) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$addFields: {stockPrice: 100}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$addFields",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that multiple '$addFields' gets promoted before the
// '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleAddFields) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$addFields: {stockPrice: 100}}"),
                                              fromjson("{$addFields: {quarter: 'Q1'}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$addFields",
                           "$addFields",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that the '$addFields' gets promoted before the '$_internalChangeStreamCheckTopologyChange'
// if resume token is present.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithAddFieldsAndResumeToken) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream"
             << BSON("resumeAfter"
                     << makeResumeToken(
                            kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)
                     << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true)),
        BSON("$addFields" << BSON("stockPrice" << 100))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$addFields",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

//
// Tests that the single '$set' gets promoted before the
// '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleSet) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$set: {stockPrice: 100}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$set",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that multiple '$set' gets promoted before the '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithMultipleSet) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$set: {stockPrice: 100}}"),
                                              fromjson("{$set: {quarter: 'Q1'}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$set",
                           "$set",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that the '$set' gets promoted before the '$_internalChangeStreamCheckTopologyChange' if
// resume token is present.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithSetAndResumeToken) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream"
             << BSON("resumeAfter"
                     << makeResumeToken(
                            kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)
                     << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true)),
        BSON("$set" << BSON("stockPrice" << 100))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$set",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

//
// Tests that the single '$replaceRoot' gets promoted before the
// '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleReplaceRoot) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        kShowExpandedEventsSpec, fromjson("{$replaceRoot: {newRoot: '$fullDocument'}}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that the '$replaceRoot' gets promoted before the
// '$_internalChangeStreamCheckTopologyChange' if resume token is present.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithReplaceRootAndResumeToken) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream"
             << BSON("resumeAfter"
                     << makeResumeToken(
                            kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)
                     << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true)),
        BSON("$replaceRoot" << BSON("newRoot" << "$fullDocument"))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

//
// Tests that the single '$replaceWith' gets promoted before the
// '$_internalChangeStreamCheckTopologyChange' as
// '$replaceRoot'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithSingleReplaceWith) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec,
                                              fromjson("{$replaceWith: '$fullDocument'}")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that the '$replaceWith' gets promoted before the
// '$_internalChangeStreamCheckTopologyChange' if resume token is present as '$replaceRoot'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithReplaceWithAndResumeToken) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream"
             << BSON("resumeAfter"
                     << makeResumeToken(
                            kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)
                     << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true)),
        BSON("$replaceWith" << "$fullDocument")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

//
//  Tests that when 'showExpandedEvents' is true, we do not inject any additional stages.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithShowExpandedEventsTrueDoesNotInjectMatchStage) {
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that when 'showExpandedEvents' is unset, we inject an additional $match stage and promote
// it before the '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithShowExpandedEventsFalseInjectsMatchStage) {
    const std::vector<BSONObj> rawPipeline = {kDefaultSpec};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that when 'showExpandedEvents' is false, the injected match stage gets merged with the user
// match stage and gets promoted before the '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithShowExpandedEventsFalseAndUserMatch) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$changeStream: {showExpandedEvents: false}}"),
        BSON("$match" << BSON("operationType" << "insert"))};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that when 'showExpandedEvents' is false, the injected match stage can be merged with the
// user match stage and can be promoted before the user '$project' and
// '$_internalChangeStreamHandleTopologyChange'.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithShowExpandedEventsFalseAndUserProjectMatch) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$changeStream: {showExpandedEvents: false}}"),
        BSON("$project" << BSON("operationType" << 1)),
        BSON("$match" << BSON("operationType" << "insert")),
    };

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$project",
                           "$_internalChangeStreamHandleTopologyChange"});
}

//
// Tests that when all allowed stages are included along with the resume token, the final
// pipeline gets optimized.
//
TEST_F(ChangeStreamStageTest, ChangeStreamWithAllStagesAndResumeToken) {
    // We enable the 'showExpandedEvents' flag to avoid injecting an additional $match stage which
    // filters out newly added events.
    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream"
             << BSON("resumeAfter"
                     << makeResumeToken(
                            kDefaultTs, testUuid(), Value(), DSChangeStream::kDropCollectionOpType)
                     << DocumentSourceChangeStreamSpec::kShowExpandedEventsFieldName << true)),
        BSON("$project" << BSON("operationType" << 1)),
        BSON("$unset" << "_id"),
        BSON("$addFields" << BSON("stockPrice" << 100)),
        BSON("$set" << BSON("fullDocument.stockPrice" << 100)),
        BSON("$match" << BSON("operationType" << "insert")),
        BSON("$replaceRoot" << BSON("newRoot" << "$fullDocument")),
        BSON("$replaceWith" << "fullDocument.stockPrice")};

    auto pipeline = buildTestPipeline(rawPipeline);

    assertStagesNameOrder(std::move(pipeline),
                          {"$_internalChangeStreamOplogMatch",
                           "$_internalChangeStreamUnwindTransaction",
                           "$_internalChangeStreamTransform",
                           "$_internalChangeStreamCheckInvalidate",
                           "$_internalChangeStreamCheckResumability",
                           "$_internalChangeStreamCheckTopologyChange",
                           "$match",
                           "$project",
                           "$project",
                           "$addFields",
                           "$set",
                           "$replaceRoot",
                           "$replaceRoot",
                           "$_internalChangeStreamHandleTopologyChange",
                           "$_internalChangeStreamEnsureResumeTokenPresent"});
}

BSONObj makeAnOplogEntry(const Timestamp& ts, Document docKey) {
    const auto uuid = testUuid();

    auto updateField = change_stream_test_helper::makeOplogEntry(
        repl::OpTypeEnum::kUpdate,                                 // op type
        change_stream_test_helper::nss,                            // namespace
        BSON("$v" << 2 << "diff" << BSON("u" << BSON("y" << 2))),  // o
        uuid,                                                      // uuid
        boost::none,                                               // fromMigrate
        docKey.toBson(),                                           // o2
        repl::OpTime(ts, 1));                                      // opTime
    return updateField.getEntry().toBSON();
}

using MultiTokenFormatVersionTest = ChangeStreamStageTest;

TEST_F(MultiTokenFormatVersionTest, CanResumeFromV2Token) {
    const auto beforeResumeTs = Timestamp(100, 1);
    const auto resumeTs = Timestamp(100, 2);
    const auto afterResumeTs = Timestamp(100, 3);
    const auto uuid = testUuid();

    const auto lowerDocumentKey = Document{{"x", 1}, {"y", 0}};
    const auto midDocumentKey = Document{{"x", 1}, {"y", 1}};
    const auto higherDocumentKey = Document{{"x", 1}, {"y", 2}};

    auto oplogBeforeResumeTime = makeAnOplogEntry(beforeResumeTs, midDocumentKey);
    auto oplogAtResumeTimeLowerDocKey = makeAnOplogEntry(resumeTs, lowerDocumentKey);
    auto oplogResumeTime = makeAnOplogEntry(resumeTs, midDocumentKey);
    auto oplogAtResumeTimeHigherDocKey = makeAnOplogEntry(resumeTs, higherDocumentKey);
    auto oplogAfterResumeTime = makeAnOplogEntry(afterResumeTs, midDocumentKey);

    // Create a resume token matching the 'oplogResumeTime' above.
    ResumeTokenData resumeToken{
        resumeTs, 2 /* version */, 0, uuid, "update"_sd, Value(midDocumentKey), Value()};

    // Create a change stream spec that resumes after 'resumeToken'.
    const auto spec =
        BSON("$changeStream" << BSON("resumeAfter" << ResumeToken(resumeToken).toBSON()));

    // Make a pipeline from this spec and seed it with the oplog entries in order.
    auto execPipeline = makeExecPipeline({oplogBeforeResumeTime,
                                          oplogAtResumeTimeLowerDocKey,
                                          oplogResumeTime,
                                          oplogAtResumeTimeHigherDocKey,
                                          oplogAfterResumeTime},
                                         spec);
    auto lastStage = execPipeline->getStages().back();

    // The stream will swallow everything up to and including the resume token. The first event we
    // get back has the same clusterTime as the resume token, and should therefore use the client
    // token's version, which is 2. Similarly, the eventIdentifier should use the v2 token format.
    auto next = lastStage->getNext();
    ASSERT(next.isAdvanced());
    const auto sameTsResumeToken =
        ResumeToken::parse(next.releaseDocument()["_id"].getDocument()).getData();
    ASSERT_EQ(sameTsResumeToken.clusterTime, resumeTs);
    ASSERT_EQ(sameTsResumeToken.version, 2);
    ASSERT_VALUE_EQ(
        sameTsResumeToken.eventIdentifier,
        Value(Document{{"operationType", "update"_sd}, {"documentKey", higherDocumentKey}}));

    // The next event has a clusterTime later than the resume point, and should therefore start
    // using the default token version, which is 2.
    next = lastStage->getNext();
    ASSERT(next.isAdvanced());
    const auto afterResumeTsResumeToken =
        ResumeToken::parse(next.releaseDocument()["_id"].getDocument()).getData();
    ASSERT_EQ(afterResumeTsResumeToken.clusterTime, afterResumeTs);
    ASSERT_EQ(afterResumeTsResumeToken.version, ResumeTokenData::kDefaultTokenVersion);
    ASSERT_VALUE_EQ(
        afterResumeTsResumeToken.eventIdentifier,
        Value(Document{{"operationType", "update"_sd}, {"documentKey", midDocumentKey}}));

    // Verify that no other events are returned.
    next = lastStage->getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(MultiTokenFormatVersionTest, CanResumeFromV1Token) {
    const auto beforeResumeTs = Timestamp(100, 1);
    const auto resumeTs = Timestamp(100, 2);
    const auto afterResumeTs = Timestamp(100, 3);
    const auto uuid = testUuid();

    const auto lowerDocumentKey = Document{{"x", 1}, {"y", 0}};
    const auto midDocumentKey = Document{{"x", 1}, {"y", 1}};
    const auto higherDocumentKey = Document{{"x", 1}, {"y", 2}};

    auto oplogBeforeResumeTime = makeAnOplogEntry(beforeResumeTs, midDocumentKey);
    auto oplogAtResumeTimeLowerDocKey = makeAnOplogEntry(resumeTs, lowerDocumentKey);
    auto oplogResumeTime = makeAnOplogEntry(resumeTs, midDocumentKey);
    auto oplogAtResumeTimeHigherDocKey = makeAnOplogEntry(resumeTs, higherDocumentKey);
    auto oplogAfterResumeTime = makeAnOplogEntry(afterResumeTs, midDocumentKey);

    // Create a resume token matching the 'oplogResumeTime' above.
    ResumeTokenData resumeToken{
        resumeTs, 1 /* version */, 0, uuid, "update"_sd, Value(midDocumentKey), Value()};

    // Create a change stream spec that resumes after 'resumeToken'.
    const auto spec =
        BSON("$changeStream" << BSON("resumeAfter" << ResumeToken(resumeToken).toBSON()));

    // Make a pipeline from this spec and seed it with the oplog entries in order.
    auto execPipeline = makeExecPipeline({oplogBeforeResumeTime,
                                          oplogAtResumeTimeLowerDocKey,
                                          oplogResumeTime,
                                          oplogAtResumeTimeHigherDocKey,
                                          oplogAfterResumeTime},
                                         spec);
    auto lastStage = execPipeline->getStages().back();

    // The stream will swallow everything up to and including the resume token. The first event we
    // get back has the same clusterTime as the resume token, and should therefore use the client
    // token's version, which is 1. Similarly, the eventIdentifier should use the v1 token format.
    auto next = lastStage->getNext();
    ASSERT(next.isAdvanced());
    const auto sameTsResumeToken =
        ResumeToken::parse(next.releaseDocument()["_id"].getDocument()).getData();
    ASSERT_EQ(sameTsResumeToken.clusterTime, resumeTs);
    ASSERT_EQ(sameTsResumeToken.version, 1);
    ASSERT_VALUE_EQ(sameTsResumeToken.eventIdentifier, Value(higherDocumentKey));

    // The next event has a clusterTime later than the resume point, and should therefore start
    // using the default token version.
    next = lastStage->getNext();
    ASSERT(next.isAdvanced());
    const auto afterResumeTsResumeToken =
        ResumeToken::parse(next.releaseDocument()["_id"].getDocument()).getData();
    ASSERT_EQ(afterResumeTsResumeToken.clusterTime, afterResumeTs);
    ASSERT_EQ(afterResumeTsResumeToken.version, ResumeTokenData::kDefaultTokenVersion);
    ASSERT_VALUE_EQ(
        afterResumeTsResumeToken.eventIdentifier,
        Value(Document{{"operationType", "update"_sd}, {"documentKey", midDocumentKey}}));

    // Verify that no other events are returned.
    next = lastStage->getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(MultiTokenFormatVersionTest, CanResumeFromV1HighWaterMark) {
    const auto beforeResumeTs = Timestamp(100, 1);
    const auto resumeTs = Timestamp(100, 2);
    const auto afterResumeTs = Timestamp(100, 3);

    const auto documentKey = Document{{"x", 1}, {"y", 1}};
    const auto higherDocumentKey = Document{{"x", 1}, {"y", 2}};

    auto oplogBeforeResumeTime = makeAnOplogEntry(beforeResumeTs, documentKey);
    auto firstOplogAtResumeTime = makeAnOplogEntry(resumeTs, documentKey);
    auto secondOplogAtResumeTime = makeAnOplogEntry(resumeTs, higherDocumentKey);
    auto oplogAfterResumeTime = makeAnOplogEntry(afterResumeTs, documentKey);

    // Create a v2 high water mark token which sorts immediately before 'firstOplogAtResumeTime'.
    ResumeTokenData resumeToken = ResumeToken::makeHighWaterMarkToken(resumeTs, 2).getData();
    resumeToken.version = 1;
    auto expCtx = getExpCtxRaw();
    expCtx->setNamespaceString(NamespaceString::makeCollectionlessAggregateNSS(
        DatabaseName::createDatabaseName_forTest(boost::none, "unittests")));

    // Create a change stream spec that resumes after 'resumeToken'.
    const auto spec =
        BSON("$changeStream" << BSON("resumeAfter" << ResumeToken(resumeToken).toBSON()));

    // Make a pipeline from this spec and seed it with the oplog entries in order.
    auto execPipeline = makeExecPipeline({oplogBeforeResumeTime,
                                          firstOplogAtResumeTime,
                                          secondOplogAtResumeTime,
                                          oplogAfterResumeTime},
                                         spec);

    // The high water mark token should be order ahead of every other entry with the same
    // clusterTime. So we should see both entries that match the resumeToken's clusterTime.
    // Even though the high watermark token has version 1, the resulting events should have
    // the default resume token version.
    auto lastStage = execPipeline->getStages().back();
    auto next = lastStage->getNext();
    ASSERT(next.isAdvanced());
    const auto sameTsResumeToken1 =
        ResumeToken::parse(next.releaseDocument()["_id"].getDocument()).getData();
    ASSERT_EQ(sameTsResumeToken1.clusterTime, resumeTs);
    ASSERT_EQ(sameTsResumeToken1.version, 2);
    ASSERT_VALUE_EQ(sameTsResumeToken1.eventIdentifier,
                    Value(Document{{"operationType", "update"_sd}, {"documentKey", documentKey}}));

    next = lastStage->getNext();
    ASSERT(next.isAdvanced());
    const auto sameTsResumeToken2 =
        ResumeToken::parse(next.releaseDocument()["_id"].getDocument()).getData();
    ASSERT_EQ(sameTsResumeToken2.clusterTime, resumeTs);
    ASSERT_EQ(sameTsResumeToken2.version, 2);
    ASSERT_VALUE_EQ(
        sameTsResumeToken2.eventIdentifier,
        Value(Document{{"operationType", "update"_sd}, {"documentKey", higherDocumentKey}}));

    // The resumeToken after the current clusterTime should start using the default version, and
    // corresponding 'eventIdentifier' format.
    next = lastStage->getNext();
    ASSERT(next.isAdvanced());
    const auto afterResumeTsResumeToken =
        ResumeToken::parse(next.releaseDocument()["_id"].getDocument()).getData();
    ASSERT_EQ(afterResumeTsResumeToken.clusterTime, afterResumeTs);
    ASSERT_EQ(afterResumeTsResumeToken.version, ResumeTokenData::kDefaultTokenVersion);
    ASSERT_VALUE_EQ(afterResumeTsResumeToken.eventIdentifier,
                    Value(Document{{"operationType", "update"_sd}, {"documentKey", documentKey}}));

    // Verify that no other events are returned.
    next = lastStage->getNext();
    ASSERT_FALSE(next.isAdvanced());
}

TEST_F(ChangeStreamStageTestNoSetup, DocumentSourceChangeStreamAddPostImageEmptyForQueryStats) {
    auto spec = DocumentSourceChangeStreamSpec();
    spec.setFullDocument(FullDocumentModeEnum::kUpdateLookup);

    auto docSource = DocumentSourceChangeStreamAddPostImage::create(getExpCtx(), spec);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalChangeStreamAddPostImage":{"fullDocument":"updateLookup"}})",
        docSource->serialize().getDocument().toBson());

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

TEST_F(ChangeStreamStageTestNoSetup, DocumentSourceChangeStreamAddPreImageEmptyForQueryStats) {
    auto docSource = DocumentSourceChangeStreamAddPreImage{
        getExpCtx(), FullDocumentBeforeChangeModeEnum::kWhenAvailable};

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalChangeStreamAddPreImage": {
                "fullDocumentBeforeChange": "whenAvailable"
            }
        })",
        docSource.serialize().getDocument().toBson());


    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource.serialize(opts).missing());
}

TEST_F(ChangeStreamStageTestNoSetup, DocumentSourceChangeStreamCheckInvalidateEmptyForQueryStats) {
    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(
        ResumeToken::parse(makeResumeToken(Timestamp(),
                                           testConstUuid,
                                           BSON("_id" << 1 << "x" << 2),
                                           DocumentSourceChangeStream::kInsertOpType,
                                           ResumeTokenData::kFromInvalidate)));

    auto docSource = DocumentSourceChangeStreamCheckInvalidate::create(getExpCtx(), spec);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalChangeStreamCheckInvalidate": {
                "startAfterInvalidate": {
                    "_data": "8200000000000000002B042C0100296F5A10046948DF8014BD4E0488427668D9C001F5463C6F7065726174696F6E54797065003C696E736572740046646F63756D656E744B657900461E5F6964002B021E78002B04000004"
                }
            }
        })",
        docSource->serialize().getDocument().toBson());

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

TEST_F(ChangeStreamStageTestNoSetup,
       DocumentSourceChangeStreamCheckResumabilityEmptyForQueryStats) {
    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(
        ResumeToken::parse(makeResumeToken(Timestamp(),
                                           testConstUuid,
                                           BSON("_id" << 1 << "x" << 2),
                                           DocumentSourceChangeStream::kInsertOpType)));

    auto docSource = DocumentSourceChangeStreamCheckResumability::create(getExpCtx(), spec);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalChangeStreamCheckResumability": {
                "resumeToken": {
                    "_data": "8200000000000000002B042C0100296E5A10046948DF8014BD4E0488427668D9C001F5463C6F7065726174696F6E54797065003C696E736572740046646F63756D656E744B657900461E5F6964002B021E78002B04000004"
                }
            }
        })",
        docSource->serialize().getDocument().toBson());

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

TEST_F(ChangeStreamStageTestNoSetup,
       DocumentSourceChangeStreamCheckTopologyChangeEmptyForQueryStats) {
    auto docSource = DocumentSourceChangeStreamCheckTopologyChange::create(getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalChangeStreamCheckTopologyChange":{}})",
        docSource->serialize().getDocument().toBson());

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

TEST_F(ChangeStreamStageTestNoSetup,
       DocumentSourceChangeStreamEnsureResumeTokenPresentEmptyForQueryStats) {
    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(
        ResumeToken::parse(makeResumeToken(Timestamp(),
                                           testConstUuid,
                                           BSON("_id" << 1 << "x" << 2),
                                           DocumentSourceChangeStream::kInsertOpType)));

    auto docSource = DocumentSourceChangeStreamEnsureResumeTokenPresent::create(getExpCtx(), spec);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalChangeStreamEnsureResumeTokenPresent": {
                "resumeToken": {
                    "_data": "8200000000000000002B042C0100296E5A10046948DF8014BD4E0488427668D9C001F5463C6F7065726174696F6E54797065003C696E736572740046646F63756D656E744B657900461E5F6964002B021E78002B04000004"
                }
            }
        })",
        docSource->serialize().getDocument().toBson());

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

TEST_F(ChangeStreamStageTestNoSetup,
       DocumentSourceChangeStreamHandleTopologyChangeEmptyForQueryStats) {
    auto docSource = DocumentSourceChangeStreamHandleTopologyChange::create(getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalChangeStreamHandleTopologyChange":{}})",
        docSource->serialize().getDocument().toBson());

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

TEST_F(ChangeStreamStageTestNoSetup, RedactDocumentSourceChangeStreamSplitLargeEvent) {
    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(
        ResumeToken::parse(makeResumeToken(Timestamp(),
                                           testConstUuid,
                                           BSON("_id" << 1 << "x" << 2),
                                           DocumentSourceChangeStream::kInsertOpType)));

    auto docSource = DocumentSourceChangeStreamSplitLargeEvent::create(getExpCtx(), spec);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$changeStreamSplitLargeEvent":{}})",
        docSource->serialize().getDocument().toBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$changeStreamSplitLargeEvent":{}})",
        redact(*docSource));
}

TEST_F(ChangeStreamStageTestNoSetup, RedactDocumentSourceChangeStreamTransform) {
    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(
        ResumeToken::parse(makeResumeToken(Timestamp(),
                                           testConstUuid,
                                           BSON("_id" << 1 << "x" << 2),
                                           DocumentSourceChangeStream::kInsertOpType)));

    auto docSource = DocumentSourceChangeStreamTransform::create(getExpCtx(), spec);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalChangeStreamTransform": {
                "resumeAfter": {
                    "_data": "8200000000000000002B042C0100296E5A10046948DF8014BD4E0488427668D9C001F5463C6F7065726174696F6E54797065003C696E736572740046646F63756D656E744B657900461E5F6964002B021E78002B04000004"
                },
                "fullDocument": "default",
                "fullDocumentBeforeChange": "off"
            }
        })",
        docSource->serialize().getDocument().toBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$changeStream": {
                "resumeAfter": {
                    "_data": "?string"
                },
                "fullDocument": "default",
                "fullDocumentBeforeChange": "off"
            }
        })",
        redact(*docSource));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$changeStream": {
                "resumeAfter": {
                    "_data": "8200000000000000002B0229296E04"
                },
                "fullDocument": "default",
                "fullDocumentBeforeChange": "off"
            }
        })",
        docSource
            ->serialize(SerializationOptions{
                .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue})
            .getDocument()
            .toBson());
}


TEST_F(ChangeStreamStageTestNoSetup, RedactDocumentSourceChangeStreamTransformMoreFields) {
    DocumentSourceChangeStreamSpec spec;
    spec.setStartAfter(
        ResumeToken::parse(makeResumeToken(Timestamp(),
                                           testConstUuid,
                                           BSON("_id" << 1 << "x" << 2),
                                           DocumentSourceChangeStream::kInsertOpType)));
    spec.setFullDocument(FullDocumentModeEnum::kRequired);
    spec.setFullDocumentBeforeChange(FullDocumentBeforeChangeModeEnum::kWhenAvailable);
    spec.setShowExpandedEvents(true);

    auto docSource = DocumentSourceChangeStreamTransform::create(getExpCtx(), spec);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalChangeStreamTransform": {
                "startAfter": {
                    "_data": "8200000000000000002B042C0100296E5A10046948DF8014BD4E0488427668D9C001F5463C6F7065726174696F6E54797065003C696E736572740046646F63756D656E744B657900461E5F6964002B021E78002B04000004"
                },
                "fullDocument": "required",
                "fullDocumentBeforeChange": "whenAvailable",
                "showExpandedEvents": true
            }
        })",
        docSource->serialize().getDocument().toBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
                "$changeStream": {
                    "startAfter": {
                        "_data": "?string"
                    },
                    "fullDocument": "required",
                    "fullDocumentBeforeChange": "whenAvailable",
                    "showExpandedEvents": true
                }
            })",
        redact(*docSource));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
                "$changeStream": {
                    "startAfter": {
                        "_data": "8200000000000000002B0229296E04"
                    },
                    "fullDocument": "required",
                    "fullDocumentBeforeChange": "whenAvailable",
                    "showExpandedEvents": true
                }
            })",
        docSource
            ->serialize(SerializationOptions{
                .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue})
            .getDocument()
            .toBson());
}

// For DocumentSource types which contain an arbitrarily internal
// MatchExpression, we don't want match the entire structure. This
// assertion allows us to check some basic structure.
void assertRedactedMatchExpressionContainsOperatorsAndRedactedFieldPaths(BSONElement el) {
    // Walk the redacted BSON and assert that we have some ops and
    // redacted field paths.
    auto opCount = 0;
    auto redactedFieldPaths = 0;
    while (true) {
        if (el.type() == BSONType::array) {
            auto array = el.Array();
            if (array.empty()) {
                break;
            }
            el = array[0];
        } else if (el.type() == BSONType::object) {
            auto obj = el.Obj();
            if (obj.begin() == obj.end()) {
                break;
            }
            el = obj.firstElement();

            // Field name should be an operator or a redacted field path.
            if (el.fieldName()[0] == '$') {
                opCount++;
            } else if (!strcmp(el.fieldName(), "$regularExpression")) {
                opCount++;
                // Skip $regularExpression.
                continue;
            } else {
                if (strstr(el.fieldName(), "HASH<") != el.fieldName()) {
                    FAIL(std::string("Expected redacted field path: ") + el.fieldName());
                }
                redactedFieldPaths++;
            }
        } else {
            break;
        }
    }

    ASSERT(opCount > 0);
    ASSERT(redactedFieldPaths > 0);
}

TEST_F(ChangeStreamStageTestNoSetup,
       DocumentSourceChangeStreamUnwindTransactionEmptyForQueryStats) {
    auto docSource = DocumentSourceChangeStreamUnwindTransaction::create(getExpCtx());

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

TEST_F(ChangeStreamStageTestNoSetup, DocumentSourceChangeStreamOplogMatchEmptyForQueryStats) {
    DocumentSourceChangeStreamSpec spec;
    spec.setResumeAfter(
        ResumeToken::parse(makeResumeToken(Timestamp(),
                                           testConstUuid,
                                           BSON("_id" << 1 << "x" << 2),
                                           DocumentSourceChangeStream::kInsertOpType)));

    auto docSource = DocumentSourceChangeStreamOplogMatch::create(getExpCtx(), spec);

    auto opts = SerializationOptions{
        .literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

}  // namespace
}  // namespace mongo
