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
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
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
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
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

static const Timestamp kDefaultTs(100, 1);
static const repl::OpTime kDefaultOpTime(kDefaultTs, 1);
static const NamespaceString nss("unittests.change_stream");

class ChangeStreamStageTestNoSetup : public AggregationContextFixture {
public:
    ChangeStreamStageTestNoSetup() : ChangeStreamStageTestNoSetup(nss) {}
    explicit ChangeStreamStageTestNoSetup(NamespaceString nsString)
        : AggregationContextFixture(nsString) {}
};

// This is needed only for the "insert" tests.
struct MockMongoInterface final : public StubMongoProcessInterface {

    MockMongoInterface(std::vector<FieldPath> fields) : _fields(std::move(fields)) {}

    std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFields(OperationContext*,
                                                                     UUID) const final {
        return {_fields, true};
    }

    std::vector<FieldPath> _fields;
};

class ChangeStreamStageTest : public ChangeStreamStageTestNoSetup {
public:
    ChangeStreamStageTest() : ChangeStreamStageTest(nss){};

    explicit ChangeStreamStageTest(NamespaceString nsString)
        : ChangeStreamStageTestNoSetup(nsString) {
        repl::ReplicationCoordinator::set(getExpCtx()->opCtx->getServiceContext(),
                                          stdx::make_unique<repl::ReplicationCoordinatorMock>(
                                              getExpCtx()->opCtx->getServiceContext()));
    }

    void checkTransformation(const OplogEntry& entry,
                             const boost::optional<Document> expectedDoc,
                             std::vector<FieldPath> docKeyFields = {}) {
        vector<intrusive_ptr<DocumentSource>> stages = makeStages(entry);
        auto transform = stages[2].get();

        getExpCtx()->mongoProcessInterface = stdx::make_unique<MockMongoInterface>(docKeyFields);

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
    vector<intrusive_ptr<DocumentSource>> makeStages(const BSONObj& entry) {
        const auto spec = fromjson("{$changeStream: {}}");
        list<intrusive_ptr<DocumentSource>> result =
            DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());
        vector<intrusive_ptr<DocumentSource>> stages(std::begin(result), std::end(result));
        getExpCtx()->mongoProcessInterface =
            stdx::make_unique<MockMongoInterface>(std::vector<FieldPath>{});

        // This match stage is a DocumentSourceOplogMatch, which we explicitly disallow from
        // executing as a safety mechanism, since it needs to use the collection-default collation,
        // even if the rest of the pipeline is using some other collation. To avoid ever executing
        // that stage here, we'll up-convert it from the non-executable DocumentSourceOplogMatch to
        // a fully-executable DocumentSourceMatch. This is safe because all of the unit tests will
        // use the 'simple' collation.
        auto match = dynamic_cast<DocumentSourceMatch*>(stages[0].get());
        ASSERT(match);
        auto executableMatch = DocumentSourceMatch::create(match->getQuery(), getExpCtx());

        auto mock = DocumentSourceMock::create(D(entry));
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

    vector<intrusive_ptr<DocumentSource>> makeStages(const OplogEntry& entry) {
        return makeStages(entry.toBSON());
    }

    OplogEntry createCommand(const BSONObj& oField,
                             const boost::optional<UUID> uuid = boost::none,
                             const boost::optional<bool> fromMigrate = boost::none,
                             boost::optional<repl::OpTime> opTime = boost::none) {
        return makeOplogEntry(OpTypeEnum::kCommand,  // op type
                              nss.getCommandNS(),    // namespace
                              oField,                // o
                              uuid,                  // uuid
                              fromMigrate,           // fromMigrate
                              boost::none,           // o2
                              opTime);               // opTime
    }

    Document makeResumeToken(Timestamp ts,
                             ImplicitValue uuid = Value(),
                             ImplicitValue docKey = Value()) {
        ResumeTokenData tokenData;
        tokenData.clusterTime = ts;
        tokenData.documentKey = docKey;
        if (!uuid.missing())
            tokenData.uuid = uuid.getUuid();
        return ResumeToken(tokenData).toDocument(ResumeToken::SerializationFormat::kHexString);
    }

    /**
     * Helper for running an applyOps through the pipeline, and getting all of the results.
     */
    std::vector<Document> getApplyOpsResults(const Document& applyOpsDoc,
                                             const LogicalSessionFromClient& lsid) {
        BSONObj applyOpsObj = applyOpsDoc.toBson();

        // Create an oplog entry and then glue on an lsid and txnNumber
        auto baseOplogEntry = makeOplogEntry(OpTypeEnum::kCommand,
                                             nss.getCommandNS(),
                                             applyOpsObj,
                                             testUuid(),
                                             boost::none,  // fromMigrate
                                             BSONObj());
        BSONObjBuilder builder(baseOplogEntry.toBSON());
        builder.append("lsid", lsid.toBSON());
        builder.append("txnNumber", 0LL);
        BSONObj oplogEntry = builder.done();

        // Create the stages and check that the documents produced matched those in the applyOps.
        vector<intrusive_ptr<DocumentSource>> stages = makeStages(oplogEntry);
        auto transform = stages[2].get();
        invariant(dynamic_cast<DocumentSourceChangeStreamTransform*>(transform) != nullptr);

        std::vector<Document> res;
        auto next = transform->getNext();
        while (next.isAdvanced()) {
            res.push_back(next.releaseDocument());
            next = transform->getNext();
        }
        return res;
    }


    /**
     * This method is required to avoid a static initialization fiasco resulting from calling
     * UUID::gen() in file static scope.
     */
    static const UUID& testUuid() {
        static const UUID* uuid_gen = new UUID(UUID::gen());
        return *uuid_gen;
    }

    static LogicalSessionFromClient testLsid() {
        // Required to avoid static initialization fiasco.
        static const UUID* uuid = new UUID(UUID::gen());
        LogicalSessionFromClient lsid{};
        lsid.setId(*uuid);
        return lsid;
    }


    /**
     * Creates an OplogEntry with given parameters and preset defaults for this test suite.
     */
    static repl::OplogEntry makeOplogEntry(repl::OpTypeEnum opType,
                                           NamespaceString nss,
                                           BSONObj object,
                                           boost::optional<UUID> uuid = testUuid(),
                                           boost::optional<bool> fromMigrate = boost::none,
                                           boost::optional<BSONObj> object2 = boost::none,
                                           boost::optional<repl::OpTime> opTime = boost::none) {
        long long hash = 1LL;
        return repl::OplogEntry(opTime ? *opTime : kDefaultOpTime,  // optime
                                hash,                               // hash
                                opType,                             // opType
                                nss,                                // namespace
                                uuid,                               // uuid
                                fromMigrate,                        // fromMigrate
                                repl::OplogEntry::kOplogVersion,    // version
                                object,                             // o
                                object2,                            // o2
                                {},                                 // sessionInfo
                                boost::none,                        // upsert
                                boost::none,                        // wall clock time
                                boost::none,                        // statement id
                                boost::none,   // optime of previous write within same transaction
                                boost::none,   // pre-image optime
                                boost::none);  // post-image optime
    }
};

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
            BSON(DSChangeStream::kStageName
                 << BSON("resumeAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))
                         << "$_resumeAfterClusterTime"
                         << BSON("ts" << kDefaultTs)))
                .firstElement(),
            expCtx),
        AssertionException,
        40674);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAtClusterTimeAndResumeAfterOptions) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the UUID catalog so the resume token is valid.
    Collection collection(stdx::make_unique<CollectionMock>(nss));
    UUIDCatalog::get(expCtx->opCtx).onCreateCollection(expCtx->opCtx, &collection, testUuid());

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName
                 << BSON("resumeAfter"
                         << makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))
                         << "startAtClusterTime"
                         << BSON("ts" << kDefaultTs)))
                .firstElement(),
            expCtx),
        AssertionException,
        40674);
}

TEST_F(ChangeStreamStageTest, ShouldRejectBothStartAtAndResumeAfterClusterTimeOptions) {
    auto expCtx = getExpCtx();

    // Need to put the collection in the UUID catalog so the resume token is valid.
    Collection collection(stdx::make_unique<CollectionMock>(nss));
    UUIDCatalog::get(expCtx->opCtx).onCreateCollection(expCtx->opCtx, &collection, testUuid());

    ASSERT_THROWS_CODE(DSChangeStream::createFromBson(
                           BSON(DSChangeStream::kStageName
                                << BSON("$_resumeAfterClusterTime" << BSON("ts" << kDefaultTs)
                                                                   << "startAtClusterTime"
                                                                   << BSON("ts" << kDefaultTs)))
                               .firstElement(),
                           expCtx),
                       AssertionException,
                       50573);
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
                                 BSON("_id" << 1 << "x" << 2),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert, {{"x"}, {"_id"}});
    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto insert2 = makeOplogEntry(insert.getOpType(),     // op type
                                  insert.getNamespace(),  // namespace
                                  insert.getObject(),     // o
                                  insert.getUuid(),       // uuid
                                  fromMigrate,            // fromMigrate
                                  insert.getObject2());   // o2
    checkTransformation(insert2, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyIdAndX) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("x" << 2 << "_id" << 1),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1 << "x" << 2))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"x", 2}, {"_id", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},  // _id first
    };
    checkTransformation(insert, expectedInsert, {{"_id"}, {"x"}});
}

TEST_F(ChangeStreamStageTest, TransformInsertDocKeyJustId) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert,           // op type
                                 nss,                           // namespace
                                 BSON("_id" << 1 << "x" << 2),  // o
                                 testUuid(),                    // uuid
                                 boost::none,                   // fromMigrate
                                 boost::none);                  // o2

    Document expectedInsert{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
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
                                 BSON("_id" << 1 << "x" << 1),  // o
                                 boost::none,                   // uuid
                                 fromMigrate,                   // fromMigrate
                                 boost::none);                  // o2

    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformUpdateFields) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
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
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
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
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Remove fields
    Document expectedRemoveField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
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
                                  o,                    // o
                                  testUuid(),           // uuid
                                  boost::none,          // fromMigrate
                                  o2);                  // o2

    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
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
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      boost::none);         // o2

    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),     // op type
                                       deleteEntry.getNamespace(),  // namespace
                                       deleteEntry.getObject(),     // o
                                       deleteEntry.getUuid(),       // uuid
                                       fromMigrate,                 // fromMigrate
                                       deleteEntry.getObject2());   // o2

    checkTransformation(deleteEntry2, expectedDelete);
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

TEST_F(ChangeStreamStageTest, TransformInvalidate) {
    NamespaceString otherColl("test.bar");

    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    bool dropDBFromMigrate = false;  // verify this doesn't get it filtered
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, dropDBFromMigrate);
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()), testUuid());

    // Invalidate entry doesn't have a document id.
    Document expectedInvalidate{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };
    for (auto& entry : {dropColl, rename}) {
        checkTransformation(entry, expectedInvalidate);
    }

    // Drop database invalidate entry doesn't have a UUID.
    Document expectedInvalidateDropDatabase{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
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
        makeOplogEntry(OpTypeEnum::kCommand,                                            // op type
                       otherColl.getCommandNS(),                                        // namespace
                       BSON("renameCollection" << otherColl.ns() << "to" << nss.ns()),  // o
                       testUuid(),                                                      // uuid
                       boost::none,   // fromMigrate
                       boost::none);  // o2

    Document expectedInvalidate{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };
    checkTransformation(rename, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, TransformNewShardDetected) {
    auto o2Field = D{{"type", "migrateChunkToNewShard"_sd}};
    auto newShardDetected = makeOplogEntry(OpTypeEnum::kNoop,
                                           nss,
                                           BSONObj(),
                                           testUuid(),
                                           boost::none,  // fromMigrate
                                           o2Field.toBson());

    Document expectedNewShardDetected{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << o2Field))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kNewShardDetectedOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };
    checkTransformation(newShardDetected, expectedNewShardDetected);
}

TEST_F(ChangeStreamStageTest, TransformEmptyApplyOps) {
    Document applyOpsDoc{{"applyOps", Value{std::vector<Document>{}}}};

    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // Should not return anything.
    ASSERT_EQ(results.size(), 0u);
}

DEATH_TEST_F(ChangeStreamStageTest, ShouldCrashWithNoopInsideApplyOps, "Unexpected noop") {
    Document applyOpsDoc =
        Document{{"applyOps",
                  Value{std::vector<Document>{
                      Document{{"op", "n"_sd},
                               {"ns", nss.ns()},
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
                      Document{{"ns", nss.ns()},
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
                               {"ns", nss.ns()},
                               {"ui", testUuid()},
                               {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}};
    LogicalSessionFromClient lsid = testLsid();
    getApplyOpsResults(applyOpsDoc, lsid);  // Should crash.
}

TEST_F(ChangeStreamStageTest, TransformNonTransactionApplyOps) {
    BSONObj applyOpsObj = Document{{"applyOps",
                                    Value{std::vector<Document>{Document{
                                        {"op", "i"_sd},
                                        {"ns", nss.ns()},
                                        {"ui", testUuid()},
                                        {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}}}}}}
                              .toBson();

    // Don't append lsid or txnNumber

    auto oplogEntry = makeOplogEntry(OpTypeEnum::kCommand,
                                     nss.getCommandNS(),
                                     applyOpsObj,
                                     testUuid(),
                                     boost::none,  // fromMigrate
                                     BSONObj());


    checkTransformation(oplogEntry, boost::none);
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
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

    // All documents should be skipped.
    ASSERT_EQ(results.size(), 0u);
}


TEST_F(ChangeStreamStageTest, TransformApplyOps) {
    // Doesn't use the checkTransformation() pattern that other tests use since we expect multiple
    // documents to be returned from one applyOps.

    Document applyOpsDoc{
        {"applyOps",
         Value{std::vector<Document>{
             Document{{"op", "i"_sd},
                      {"ns", nss.ns()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"_id", 123}, {"x", "hallo"_sd}}}}},
             Document{{"op", "u"_sd},
                      {"ns", nss.ns()},
                      {"ui", testUuid()},
                      {"o", Value{Document{{"$set", Value{Document{{"x", "hallo 2"_sd}}}}}}},
                      {"o2", Value{Document{{"_id", 123}}}}},
             // Operation on another namespace which should be skipped.
             Document{{"op", "i"_sd},
                      {"ns", "someotherdb.collname"_sd},
                      {"ui", UUID::gen()},
                      {"o", Value{Document{{"_id", 0}, {"x", "Should not read this!"_sd}}}}},
         }}},
    };
    LogicalSessionFromClient lsid = testLsid();
    vector<Document> results = getApplyOpsResults(applyOpsDoc, lsid);

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

TEST_F(ChangeStreamStageTest, ClusterTimeMatchesOplogEntry) {
    const Timestamp ts(3, 45);
    const long long term = 4;
    const auto opTime = repl::OpTime(ts, term);

    // Test the 'clusterTime' field is copied from the oplog entry for an update.
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2,                   // o2
                                      opTime);              // opTime

    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, ts},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {
            "updateDescription", D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);

    // Test the 'clusterTime' field is copied from the oplog entry for an invalidation.
    OplogEntry dropColl =
        createCommand(BSON("drop" << nss.coll()), testUuid(), boost::none, opTime);

    // Invalidate entry doesn't have a document id.
    Document expectedInvalidate{
        {DSChangeStream::kIdField, makeResumeToken(ts, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, ts},
    };
    checkTransformation(dropColl, expectedInvalidate);
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
                               BSON("msg"
                                    << "new primary"));  // o

    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateIndex) {
    auto indexSpec = D{{"v", 2}, {"key", D{{"a", 1}}}, {"name", "a_1"_sd}, {"ns", nss.ns()}};
    NamespaceString indexNs(nss.getSystemIndexesCollection());
    bool fromMigrate = false;  // At the moment this makes no difference.
    auto createIndex = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      indexNs,              // namespace
                                      indexSpec.toBson(),   // o
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
                                      boost::none);         // o2

    checkTransformation(createIndex, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateIndexFromMigrate) {
    auto indexSpec = D{{"v", 2}, {"key", D{{"a", 1}}}, {"name", "a_1"_sd}, {"ns", nss.ns()}};
    NamespaceString indexNs(nss.getSystemIndexesCollection());
    bool fromMigrate = true;
    auto createIndex = makeOplogEntry(OpTypeEnum::kInsert,  // op type
                                      indexNs,              // namespace
                                      indexSpec.toBson(),   // o
                                      boost::none,          // uuid
                                      fromMigrate,          // fromMigrate
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
    ASSERT(dynamic_cast<DocumentSourceChangeStreamTransform*>(stage.get()));

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
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
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

//
// Test class for change stream of a single database.
//
class ChangeStreamStageDBTest : public ChangeStreamStageTest {
public:
    ChangeStreamStageDBTest()
        : ChangeStreamStageTest(NamespaceString::makeCollectionlessAggregateNSS(nss.db())) {}
};

TEST_F(ChangeStreamStageDBTest, TransformInsert) {
    auto insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1 << "x" << 2));

    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insert, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageDBTest, InsertOnOtherCollections) {
    NamespaceString otherNss("unittests.other_collection.");
    auto insertOtherColl =
        makeOplogEntry(OpTypeEnum::kInsert, otherNss, BSON("_id" << 1 << "x" << 2));

    // Insert on another collection in the same database.
    Document expectedInsert{
        {DSChangeStream::kIdField,
         makeResumeToken(kDefaultTs, testUuid(), BSON("x" << 2 << "_id" << 1))},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}}},
        {DSChangeStream::kNamespaceField, D{{"db", otherNss.db()}, {"coll", otherNss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"x", 2}, {"_id", 1}}},  // Note _id <-> x reversal.
    };
    checkTransformation(insertOtherColl, expectedInsert, {{"x"}, {"_id"}});
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersChangesOnOtherDatabases) {
    std::set<NamespaceString> unmatchedNamespaces = {
        // Namespace starts with the db name, but is longer.
        NamespaceString("unittests2.coll"),
        // Namespace contains the db name, but not at the front.
        NamespaceString("test.unittests"),
        // Namespace contains the db name + dot.
        NamespaceString("test.unittests.coll"),
        // Namespace contains the db name + dot but is followed by $.
        NamespaceString("unittests.$cmd"),
    };

    // Insert into another database.
    for (auto& ns : unmatchedNamespaces) {
        auto insert = makeOplogEntry(OpTypeEnum::kInsert, ns, BSON("_id" << 1));
        checkTransformation(insert, boost::none);
    }
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersAllSystemDotCollections) {
    auto nss = NamespaceString("unittests.system.coll");
    auto insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString("unittests.system.users");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString("unittests.system.roles");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);

    nss = NamespaceString("unittests.system.keys");
    insert = makeOplogEntry(OpTypeEnum::kInsert, nss, BSON("_id" << 1));
    checkTransformation(insert, boost::none);
}

TEST_F(ChangeStreamStageDBTest, TransformsEntriesForLegalClientCollectionsWithSystem) {
    std::set<NamespaceString> allowedNamespaces = {
        NamespaceString("unittests.coll.system"),
        NamespaceString("unittests.coll.system.views"),
        NamespaceString("unittests.systemx"),
    };

    for (auto& ns : allowedNamespaces) {
        auto insert = makeOplogEntry(OpTypeEnum::kInsert, ns, BSON("_id" << 1));
        Document expectedInsert{
            {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), BSON("_id" << 1))},
            {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
            {DSChangeStream::kClusterTimeField, kDefaultTs},
            {DSChangeStream::kFullDocumentField, D{{"_id", 1}}},
            {DSChangeStream::kNamespaceField, D{{"db", ns.db()}, {"coll", ns.coll()}}},
            {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
        };
        checkTransformation(insert, expectedInsert, {{"_id"}});
    }
}

TEST_F(ChangeStreamStageDBTest, TransformUpdateFields) {
    BSONObj o = BSON("$set" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto updateField = makeOplogEntry(OpTypeEnum::kUpdate, nss, o, testUuid(), boost::none, o2);

    Document expectedUpdateField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
        {"updateDescription", D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}}},
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageDBTest, TransformRemoveFields) {
    BSONObj o = BSON("$unset" << BSON("y" << 1));
    BSONObj o2 = BSON("_id" << 1 << "x" << 2);
    auto removeField = makeOplogEntry(OpTypeEnum::kUpdate,  // op type
                                      nss,                  // namespace
                                      o,                    // o
                                      testUuid(),           // uuid
                                      boost::none,          // fromMigrate
                                      o2);                  // o2

    // Remove fields
    Document expectedRemoveField{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{{"_id", 1}, {"x", 2}}}},
        {
            "updateDescription", D{{"updatedFields", D{}}, {"removedFields", vector<V>{V("y"_sd)}}},
        }};
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
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o2)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 2}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
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
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid(), o)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}, {"x", 2}}},
    };
    checkTransformation(deleteEntry, expectedDelete);

    bool fromMigrate = false;  // also check actual "fromMigrate: false" not filtered
    auto deleteEntry2 = makeOplogEntry(deleteEntry.getOpType(),     // op type
                                       deleteEntry.getNamespace(),  // namespace
                                       deleteEntry.getObject(),     // o
                                       deleteEntry.getUuid(),       // uuid
                                       fromMigrate,                 // fromMigrate
                                       deleteEntry.getObject2());   // o2

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

TEST_F(ChangeStreamStageDBTest, TransformInvalidate) {
    NamespaceString otherColl("test.bar");

    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()), testUuid());
    bool dropDBFromMigrate = false;  // verify this doesn't get it filtered
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1), boost::none, dropDBFromMigrate);
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()), testUuid());

    // Invalidate entry doesn't have a document id.
    Document expectedInvalidate{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };
    for (auto& entry : {dropColl, rename}) {
        checkTransformation(entry, expectedInvalidate);
    }

    // Drop database invalidate entry doesn't have a UUID.
    Document expectedInvalidateDropDatabase{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs)},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };
    checkTransformation(dropDB, expectedInvalidateDropDatabase);
}

TEST_F(ChangeStreamStageDBTest, SystemCollectionsDropOrRenameShouldInvalidate) {
    NamespaceString systemColl(nss.db() + ".system.users");
    NamespaceString renamedSystemColl(nss.db() + ".system.users_new");
    Document expectedInvalidate{
        {DSChangeStream::kIdField, makeResumeToken(kDefaultTs, testUuid())},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kClusterTimeField, kDefaultTs},
    };

    OplogEntry dropColl = createCommand(BSON("drop" << systemColl.coll()), testUuid());
    checkTransformation(dropColl, expectedInvalidate);

    OplogEntry rename = createCommand(
        BSON("renameCollection" << systemColl.ns() << "to" << renamedSystemColl.ns()), testUuid());
    checkTransformation(rename, expectedInvalidate);
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersNoOp) {
    OplogEntry noOp = makeOplogEntry(OpTypeEnum::kNoop,
                                     NamespaceString(),
                                     BSON("msg"
                                          << "new primary"));
    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageDBTest, MatchFiltersCreateIndex) {
    auto indexSpec = D{{"v", 2}, {"key", D{{"a", 1}}}, {"name", "a_1"_sd}, {"ns", nss.ns()}};
    NamespaceString indexNs(nss.getSystemIndexesCollection());
    OplogEntry createIndex = makeOplogEntry(OpTypeEnum::kInsert, indexNs, indexSpec.toBson());
    checkTransformation(createIndex, boost::none);
}

}  // namespace
}  // namespace mongo
