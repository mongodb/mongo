/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/close_change_stream_exception.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;
using repl::OpTypeEnum;
using repl::OplogEntry;
using std::list;
using std::string;
using std::vector;

using D = Document;
using V = Value;

using DSChangeStream = DocumentSourceChangeStream;

static const Timestamp ts(100, 1);
static const repl::OpTime optime(ts, 1);
static const NamespaceString nss("unittests.change_stream");

using ChangeStreamStageTestNoSetup = AggregationContextFixture;

class ChangeStreamStageTest : public AggregationContextFixture {
public:
    ChangeStreamStageTest() : AggregationContextFixture(nss) {
        repl::ReplicationCoordinator::set(getExpCtx()->opCtx->getServiceContext(),
                                          stdx::make_unique<repl::ReplicationCoordinatorMock>(
                                              getExpCtx()->opCtx->getServiceContext()));
    }

    void checkTransformation(const OplogEntry& entry, const boost::optional<Document> expectedDoc) {
        vector<intrusive_ptr<DocumentSource>> stages = makeStages(entry);
        auto transform = stages[2].get();

        auto next = transform->getNext();
        // Match stage should pass the doc down if expectedDoc is given.
        ASSERT_EQ(next.isAdvanced(), static_cast<bool>(expectedDoc));
        if (expectedDoc) {
            ASSERT_DOCUMENT_EQ(next.releaseDocument(), *expectedDoc);
        }
    }

    /**
     * Returns a list of stages expanded from a $changStream specification, starting with a
     * DocumentSourceMock which contains a single document representing 'entry'.
     */
    vector<intrusive_ptr<DocumentSource>> makeStages(const OplogEntry& entry) {
        const auto spec = fromjson("{$changeStream: {}}");
        list<intrusive_ptr<DocumentSource>> result =
            DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());
        vector<intrusive_ptr<DocumentSource>> stages(std::begin(result), std::end(result));

        auto match = dynamic_cast<DocumentSourceMatch*>(stages[0].get());
        ASSERT(match);
        auto mock = DocumentSourceMock::create(D(entry.toBSON()));
        match->setSource(mock.get());

        // Check the oplog entry is transformed correctly.
        auto transform = stages[1].get();
        ASSERT(transform);
        ASSERT_EQ(string(transform->getSourceName()), DSChangeStream::kStageName);
        transform->setSource(match);

        auto closeCursor = stages.back().get();
        ASSERT(closeCursor);
        closeCursor->setSource(transform);

        // Include the mock stage in the "stages" so it won't get destroyed outside the function
        // scope.
        stages.insert(stages.begin(), mock);
        return stages;
    }

    OplogEntry createCommand(const BSONObj& oField,
                             const boost::optional<UUID> uuid = boost::none) {
        auto entry = OplogEntry(optime, 1, OpTypeEnum::kCommand, nss.getCommandNS(), oField);
        if (uuid)
            entry.setUuid(uuid.get());
        return entry;
    }

    Document makeResumeToken(Timestamp ts,
                             ImplicitValue uuid = Value(),
                             ImplicitValue id = Value()) {
        if (id.missing()) {
            return {{"clusterTime", D{{"ts", ts}}}, {"uuid", uuid}};
        }
        return {{"clusterTime", D{{"ts", ts}}}, {"uuid", uuid}, {"documentKey", D{{"_id", id}}}};
    }

    /**
     * This method is required to avoid a static initialization fiasco resulting from calling
     * UUID::gen() in file static scope.
     */
    static const UUID& testUuid() {
        static const UUID* uuid_gen = new UUID(UUID::gen());
        return *uuid_gen;
    }
};

TEST_F(ChangeStreamStageTest, ShouldRejectUnrecognizedOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("unexpected" << 4)).firstElement(), expCtx),
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

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(BSON(DSChangeStream::kStageName << BSON("fullDocument"
                                                                               << "unrecognized"))
                                           .firstElement(),
                                       expCtx),
        AssertionException,
        40575);
}

TEST_F(ChangeStreamStageTestNoSetup, FailsWithNoReplicationCoordinator) {
    const auto spec = fromjson("{$changeStream: {}}");

    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       40573);
}

TEST_F(ChangeStreamStageTest, StagesGeneratedCorrectly) {
    const auto spec = fromjson("{$changeStream: {}}");

    list<intrusive_ptr<DocumentSource>> result =
        DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());
    vector<intrusive_ptr<DocumentSource>> stages(std::begin(result), std::end(result));
    ASSERT_EQUALS(stages.size(), 3UL);
    ASSERT_TRUE(dynamic_cast<DocumentSourceMatch*>(stages.front().get()));
    ASSERT_EQUALS(string(stages[0]->getSourceName()), DSChangeStream::kStageName);
    ASSERT_EQUALS(string(stages[1]->getSourceName()), DSChangeStream::kStageName);
    ASSERT_EQUALS(string(stages[2]->getSourceName()), DSChangeStream::kStageName);

    // TODO: Check explain result.
}

TEST_F(ChangeStreamStageTest, TransformInsert) {
    OplogEntry insert(optime, 1, OpTypeEnum::kInsert, nss, BSON("_id" << 1 << "x" << 1));
    insert.setUuid(testUuid());
    // Insert
    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), 1)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(insert, expectedInsert);
    insert.setFromMigrate(false);  // also check actual "fromMigrate: false" not filtered
    checkTransformation(insert, expectedInsert);
}

TEST_F(ChangeStreamStageTest, TransformInsertFromMigrate) {
    OplogEntry insert(optime, 1, OpTypeEnum::kInsert, nss, BSON("_id" << 1 << "x" << 1));
    insert.setFromMigrate(true);
    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformUpdateFields) {
    OplogEntry updateField(
        optime, 1, OpTypeEnum::kUpdate, nss, BSON("$set" << BSON("y" << 1)), BSON("_id" << 1));
    updateField.setUuid(testUuid());
    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), 1)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
        {
            "updateDescription", D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageTest, TransformRemoveFields) {
    OplogEntry removeField(
        optime, 1, OpTypeEnum::kUpdate, nss, BSON("$unset" << BSON("y" << 1)), BSON("_id" << 1));
    removeField.setUuid(testUuid());
    // Remove fields
    Document expectedRemoveField{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), 1)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
        {
            "updateDescription", D{{"updatedFields", D{}}, {"removedFields", vector<V>{V("y"_sd)}}},
        }};
    checkTransformation(removeField, expectedRemoveField);
}

TEST_F(ChangeStreamStageTest, TransformReplace) {
    OplogEntry replace(
        optime, 1, OpTypeEnum::kUpdate, nss, BSON("_id" << 1 << "y" << 1), BSON("_id" << 1));
    replace.setUuid(testUuid());
    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), 1)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeStreamStageTest, TransformDelete) {
    OplogEntry deleteEntry(optime, 1, OpTypeEnum::kDelete, nss, BSON("_id" << 1));
    deleteEntry.setUuid(testUuid());
    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), 1)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(deleteEntry, expectedDelete);
    deleteEntry.setFromMigrate(false);  // also check actual "fromMigrate: false" not filtered
    checkTransformation(deleteEntry, expectedDelete);
}

TEST_F(ChangeStreamStageTest, TransformDeleteFromMigrate) {
    OplogEntry deleteEntry(optime, 1, OpTypeEnum::kDelete, nss, BSON("_id" << 1));
    deleteEntry.setFromMigrate(true);
    checkTransformation(deleteEntry, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformInvalidate) {
    NamespaceString otherColl("test.bar");

    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1));
    dropDB.setFromMigrate(false);  // verify this doesn't get it filtered
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()), testUuid());

    // Invalidate entry doesn't have a document id.
    Document expectedInvalidate{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
    };
    for (auto& entry : {dropColl, rename}) {
        checkTransformation(entry, expectedInvalidate);
    }

    // Drop database invalidate entry doesn't have a UUID.
    Document expectedInvalidateDropDatabase{
        {DSChangeStream::kIdField, makeResumeToken(ts)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
    };
    checkTransformation(dropDB, expectedInvalidateDropDatabase);
}

TEST_F(ChangeStreamStageTest, TransformInvalidateFromMigrate) {
    NamespaceString otherColl("test.bar");

    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    dropColl.setFromMigrate(true);
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1));
    dropDB.setFromMigrate(true);
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()));
    rename.setFromMigrate(true);

    for (auto& entry : {dropColl, dropDB, rename}) {
        checkTransformation(entry, boost::none);
    }
}

TEST_F(ChangeStreamStageTest, TransformInvalidateRenameDropTarget) {
    NamespaceString otherColl("test.bar");
    OplogEntry rename(optime,
                      1,
                      OpTypeEnum::kCommand,
                      otherColl.getCommandNS(),
                      BSON("renameCollection" << otherColl.ns() << "to" << nss.ns()));
    rename.setUuid(testUuid());
    Document expectedInvalidate{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
    };
    checkTransformation(rename, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateCollection) {
    auto collSpec =
        D{{"create", "foo"_sd},
          {"idIndex", D{{"v", 2}, {"key", D{{"_id", 1}}}, {"name", "_id_"_sd}, {"ns", nss.ns()}}}};
    OplogEntry createColl = createCommand(collSpec.toBson(), testUuid());
    checkTransformation(createColl, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersNoOp) {
    OplogEntry noOp(
        optime, 1, OpTypeEnum::kNoop, NamespaceString(), fromjson("{'msg':'new primary'}"));
    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateIndex) {
    auto indexSpec = D{{"v", 2}, {"key", D{{"a", 1}}}, {"name", "a_1"_sd}, {"ns", nss.ns()}};
    NamespaceString indexNs(nss.getSystemIndexesCollection());
    OplogEntry createIndex(optime, 1, OpTypeEnum::kInsert, indexNs, indexSpec.toBson());
    createIndex.setFromMigrate(false);  // At the moment this makes no difference.
    checkTransformation(createIndex, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateIndexFromMigrate) {
    auto indexSpec = D{{"v", 2}, {"key", D{{"a", 1}}}, {"name", "a_1"_sd}, {"ns", nss.ns()}};
    NamespaceString indexNs(nss.getSystemIndexesCollection());
    OplogEntry createIndex(optime, 1, OpTypeEnum::kInsert, indexNs, indexSpec.toBson());
    createIndex.setFromMigrate(true);
    checkTransformation(createIndex, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformationShouldBeAbleToReParseSerializedStage) {
    auto expCtx = getExpCtx();

    auto originalSpec = BSON(DSChangeStream::kStageName << BSONObj());
    auto result = DSChangeStream::createFromBson(originalSpec.firstElement(), expCtx);
    vector<intrusive_ptr<DocumentSource>> allStages(std::begin(result), std::end(result));
    ASSERT_EQ(allStages.size(), 3UL);
    auto stage = allStages[1];
    ASSERT(dynamic_cast<DocumentSourceSingleDocumentTransformation*>(stage.get()));

    //
    // Serialize the stage and confirm contents.
    //
    vector<Value> serialization;
    stage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_BSONOBJ_EQ(serializedDoc.toBson(), originalSpec);

    //
    // Create a new stage from the serialization. Serialize the new stage and confirm that it is
    // equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = uassertStatusOK(Pipeline::create(
        DSChangeStream::createFromBson(serializedBson.firstElement(), expCtx), expCtx));

    auto newSerialization = roundTripped->serialize();

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(ChangeStreamStageTest, CloseCursorOnInvalidateEntries) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    auto stages = makeStages(dropColl);
    auto closeCursor = stages.back();

    Document expectedInvalidate{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
    };

    auto next = closeCursor->getNext();
    // Transform into invalidate entry.
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedInvalidate);
    // Then throw an exception on the next call of getNext().
    ASSERT_THROWS_CODE(
        closeCursor->getNext(), CloseChangeStreamException, ErrorCodes::CloseChangeStream);
}

TEST_F(ChangeStreamStageTest, CloseCursorEvenIfInvalidateEntriesGetFilteredOut) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    auto stages = makeStages(dropColl);
    auto closeCursor = stages.back();
    // Add a match stage after change stream to filter out the invalidate entries.
    auto match = DocumentSourceMatch::create(fromjson("{operationType: 'insert'}"), getExpCtx());
    match->setSource(closeCursor.get());

    // Throw an exception on the call of getNext().
    ASSERT_THROWS_CODE(match->getNext(), CloseChangeStreamException, ErrorCodes::CloseChangeStream);
}

}  // namespace
}  // namespace mongo
