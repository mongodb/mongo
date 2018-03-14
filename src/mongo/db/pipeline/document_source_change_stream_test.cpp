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
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/unittest/ensure_fcv.h"
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

using unittest::EnsureFCV;

static const Timestamp ts(100, 1);
static const repl::OpTime optime(ts, 1);
static const NamespaceString nss("unittests.change_stream");

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTypeEnum opType,
                                NamespaceString nss,
                                boost::optional<UUID> uuid,
                                boost::optional<bool> fromMigrate,
                                BSONObj object,
                                boost::optional<BSONObj> object2) {
    long long hash = 1LL;
    return repl::OplogEntry(optime,                           // optime
                            hash,                             // hash
                            opType,                           // opType
                            nss,                              // namespace
                            uuid,                             // uuid
                            fromMigrate,                      // fromMigrate
                            repl::OplogEntry::kOplogVersion,  // version
                            object,                           // o
                            object2,                          // o2
                            {},                               // sessionInfo
                            boost::none,                      // upsert
                            boost::none,                      // wall clock time
                            boost::none,                      // statement id
                            boost::none,   // optime of previous write within same transaction
                            boost::none,   // pre-image optime
                            boost::none);  // post-image optime
}

class ChangeStreamStageTestNoSetup : public AggregationContextFixture {
public:
    ChangeStreamStageTestNoSetup() : ChangeStreamStageTestNoSetup(nss) {}
    ChangeStreamStageTestNoSetup(NamespaceString nsString)
        : AggregationContextFixture(nsString), _ensureFCV(EnsureFCV::Version::kFullyUpgradedTo36) {}

private:
    EnsureFCV _ensureFCV;
};

// This is needed only for the "insert" tests.
struct MockMongoProcessInterface final : public StubMongoProcessInterface {

    MockMongoProcessInterface(std::vector<FieldPath> fields) : _fields(std::move(fields)) {}

    std::vector<FieldPath> collectDocumentKeyFields(UUID) const final {
        return _fields;
    }

    std::vector<FieldPath> _fields;
};

class ChangeStreamStageTest : public ChangeStreamStageTestNoSetup {
public:
    ChangeStreamStageTest() : ChangeStreamStageTestNoSetup() {
        repl::ReplicationCoordinator::set(getExpCtx()->opCtx->getServiceContext(),
                                          stdx::make_unique<repl::ReplicationCoordinatorMock>(
                                              getExpCtx()->opCtx->getServiceContext()));
    }

    void checkTransformation(const OplogEntry& entry,
                             const boost::optional<Document> expectedDoc,
                             std::vector<FieldPath> docKeyFields = {}) {
        vector<intrusive_ptr<DocumentSource>> stages = makeStages(entry);
        auto transform = stages[2].get();

        auto mongoProcess = std::make_shared<MockMongoProcessInterface>(docKeyFields);
        using NeedyDS = DocumentSourceNeedsMongoProcessInterface;
        dynamic_cast<NeedyDS&>(*transform).injectMongoProcessInterface(std::move(mongoProcess));

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

        // This match stage is a DocumentSourceOplogMatch, which we explicitly disallow from
        // executing as a safety mechanism, since it needs to use the collection-default collation,
        // even if the rest of the pipeline is using some other collation. To avoid ever executing
        // that stage here, we'll up-convert it from the non-executable DocumentSourceOplogMatch to
        // a fully-executable DocumentSourceMatch. This is safe because all of the unit tests will
        // use the 'simple' collation.
        auto match = dynamic_cast<DocumentSourceMatch*>(stages[0].get());
        ASSERT(match);
        auto executableMatch = DocumentSourceMatch::create(match->getQuery(), getExpCtx());

        auto mock = DocumentSourceMock::create(D(entry.toBSON()));
        executableMatch->setSource(mock.get());

        // Check the oplog entry is transformed correctly.
        auto transform = stages[1].get();
        ASSERT(transform);
        ASSERT_EQ(string(transform->getSourceName()), DSChangeStream::kStageName);
        transform->setSource(executableMatch.get());

        auto closeCursor = stages.back().get();
        ASSERT(closeCursor);
        closeCursor->setSource(transform);

        return {mock, executableMatch, transform, closeCursor};
    }

    OplogEntry createCommand(const BSONObj& oField,
                             const boost::optional<UUID> uuid = boost::none,
                             const boost::optional<bool> fromMigrate = boost::none) {
        return makeOplogEntry(OpTypeEnum::kCommand,  // op type
                              nss.getCommandNS(),    // namespace
                              uuid,                  // uuid
                              fromMigrate,           // fromMigrate
                              oField,                // o
                              boost::none);          // o2
    }

    Document makeResumeToken(Timestamp ts,
                             ImplicitValue uuid = Value(),
                             ImplicitValue docKey = Value()) {
        ResumeTokenData tokenData;
        tokenData.clusterTime = ts;
        tokenData.documentKey = docKey;
        if (!uuid.missing())
            tokenData.uuid = uuid.getUuid();
        return ResumeToken(tokenData).toDocument();
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

TEST_F(ChangeStreamStageTest, ShouldRejectBothResumeAfterClusterTimeAndResumeAfterOptions) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the UUID catalog so the resume token is valid.
    Collection collection(stdx::make_unique<CollectionMock>(nss));
    UUIDCatalog::get(expCtx->opCtx).onCreateCollection(expCtx->opCtx, &collection, testUuid());

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON(
                     "resumeAfter" << makeResumeToken(ts, testUuid(), BSON("x" << 2 << "_id" << 1))
                                   << "$_resumeAfterClusterTime"
                                   << BSON("ts" << ts)))
                .firstElement(),
            expCtx),
        AssertionException,
        40674);
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

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyXAndId) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 BSON("_id" << 1 << "x" << 2),  // o
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert, {{"x"}, {"_id"}});
    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto insert2 = makeOplogEntry(insert.getOpType(),     // op type
                                  insert.getNamespace(),  // namespace
                                  insert.getUuid(),       // uuid
                                  fromMigrate,            // fromMigrate
                                  insert.getObject(),     // o
                                  insert.getObject2());   // o2
    checkTransformation(insert2, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyIdAndX) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 BSON("x" << 2 << "_id" << 1),  // o
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), BSON("_id" << 1 << "x" << 2))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kFullDocumentField, D{{"x", 2}, {"_id", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},  // _id first
    };
    checkTransformation(insert, expectedInsert, {{"_id"}, {"x"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyJustId) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 BSON("_id" << 1 << "x" << 2),  // o
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), BSON("_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(insert, expectedInsert, {{"_id"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertFromMigrate) {
    bool fromMigrate = true;
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 boost::none,                   // uuid
                                 fromMigrate,                   // fromMigrate
                                 BSON("_id" << 1 << "x" << 1),  // o
                                 boost::none);                  // o2

    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformUpdateFields) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o,                    // o
                                      o2);                  // o2

    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription", D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);
}

// Legacy documents might not have an _id field; then the document key is the full (post-update)
// document.
TEST_F(ChangeStreamStageTest, TransformUpdateFieldsLegacyNoId) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("x" << 1 << "y" << 1);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o,                    // o
                                      o2);                  // o2

    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 1}, {"y", 1}}},
        {
            "updateDescription", D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageTest, TransformRemoveFields) {
    BSONObj o = BSON("$unset" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto removeField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o,                    // o
                                      o2);                  // o2

    // Remove fields
    Document expectedRemoveField{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{{"_id", 1}, {"x", 2}}}},
        {
            "updateDescription", D{{"updatedFields", D{}}, {"removedFields", vector<V>{V("y"_sd)}}},
        }};
    checkTransformation(removeField, expectedRemoveField);
}

TEST_F(ChangeStreamStageTest, TransformReplace) {
    BSONObj o = BSON("_id" << 1 << "x" << 2 << "y" << 1);
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto replace = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                  nss,                  // namespace
                                  testUuid(),           // uuid
                                  boost::none,          // fromMigrate
                                  o,                    // o
                                  o2);                  // o2

    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeStreamStageTest, TransformDelete) {
    BSONObj o = BSON("_id" << 1 << "x" << 2);
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o,                    // o
                                      boost::none);         // o2

    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),     // op type
                                       deleteEntry.getNamespace(),  // namespace
                                       deleteEntry.getUuid(),       // uuid
                                       fromMigrate,                 // fromMigrate
                                       deleteEntry.getObject(),     // o
                                       deleteEntry.getObject2());   // o2

    checkTransformation(deleteEntry2, expectedDelete);
}

TEST_F(ChangeStreamStageTest, TransformDeleteFromMigrate) {
    bool fromMigrate = true;
    auto deleteEntry = makeOplogEntry(OpTypeEnum::kDelete,  // op type
                                      nss,                  // namespace
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      BSON("_id" << 1),     // o
                                      boost::none);         // o2

    checkTransformation(deleteEntry, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformInvalidate) {
    NamespaceString otherColl("test.bar");

    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    bool dropDBFromMigrate = false;  // verify this doesn't get it filtered
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, dropDBFromMigrate);
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

    bool dropCollFromMigrate = true;
    OplogEntry dropColl =
        createCommand(BSON("drop" << nss.coll()), testUuid(), dropCollFromMigrate);
    bool dropDBFromMigrate = true;
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, dropDBFromMigrate);
    bool renameFromMigrate = true;
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()),
                      boost::none,
                      renameFromMigrate);

    for (auto& entry : {dropColl, dropDB, rename}) {
        checkTransformation(entry, boost::none);
    }
}

TEST_F(ChangeStreamStageTest, TransformInvalidateRenameDropTarget) {
    NamespaceString otherColl("test.bar");
    auto rename =
        makeOplogEntry(OpTypeEnum::kCommand,      // op type
                       otherColl.getCommandNS(),  // namespace
                       testUuid(),                // uuid
                       boost::none,               // fromMigrate
                       BSON("renameCollection" << otherColl.ns() << "to" << nss.ns()),  // o
                       boost::none);                                                    // o2

    Document expectedInvalidate{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
    };
    checkTransformation(rename, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformNewShardDetected) {
    auto o2Field = D{{"type", "migrateChunkToNewShard"_sd}};
    auto newShardDetected = makeOplogEntry(OpTypeEnum::kNoop,
                                           nss,
                                           testUuid(),
                                           boost::none,  // fromMigrate
                                           BSONObj(),
                                           o2Field.toBson());

    Document expectedNewShardDetected{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), BSON("_id" << o2Field))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kNewShardDetectedOpType},
    };
    checkTransformation(newShardDetected, expectedNewShardDetected);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateCollection) {
    auto collSpec =
        D{{"create", "foo"_sd},
          {"idIndex", D{{"v", 2}, {"key", D{{"_id", 1}}}, {"name", "_id_"_sd}, {"ns", nss.ns()}}}};
    OplogEntry createColl = createCommand(collSpec.toBson(), testUuid());
    checkTransformation(createColl, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersNoOp) {
    auto noOp = makeOplogEntry(OpTypeEnum::kNoop,  // op type
                               {},                 // namespace
                               boost::none,        // uuid
                               boost::none,        // fromMigrate
                               BSON("msg"
                                    << "new primary"),  // o
                               boost::none);            // o2

    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateIndex) {
    auto indexSpec = D{{"v", 2}, {"key", D{{"a", 1}}}, {"name", "a_1"_sd}, {"ns", nss.ns()}};
    NamespaceString indexNs(nss.getSystemIndexesCollection());
    bool fromMigrate = false;  // At the moment this makes no difference.
    auto createIndex = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      indexNs,              // namespace
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      indexSpec.toBson(),   // o
                                      boost::none);         // o2

    checkTransformation(createIndex, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateIndexFromMigrate) {
    auto indexSpec = D{{"v", 2}, {"key", D{{"a", 1}}}, {"name", "a_1"_sd}, {"ns", nss.ns()}};
    NamespaceString indexNs(nss.getSystemIndexesCollection());
    bool fromMigrate = true;
    auto createIndex = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      indexNs,              // namespace
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      indexSpec.toBson(),   // o
                                      boost::none);         // o2

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
    ASSERT_THROWS(closeCursor->getNext(), ExceptionFor<ErrorCodes::CloseChangeStream>);
}

TEST_F(ChangeStreamStageTest, CloseCursorEvenIfInvalidateEntriesGetFilteredOut) {
    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    auto stages = makeStages(dropColl);
    auto closeCursor = stages.back();
    // Add a match stage after change stream to filter out the invalidate entries.
    auto match = DocumentSourceMatch::create(fromjson("{operationType: 'insert'}"), getExpCtx());
    match->setSource(closeCursor.get());

    // Throw an exception on the call of getNext().
    ASSERT_THROWS(match->getNext(), ExceptionFor<ErrorCodes::CloseChangeStream>);
}

}  // namespace
}  // namespace mongo
