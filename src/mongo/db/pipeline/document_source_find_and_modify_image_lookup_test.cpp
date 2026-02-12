/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

/**
 * Creates OplogEntry with given field values.
 */
repl::OplogEntry makeOplogEntry(
    repl::OpTime opTime,
    repl::OpTypeEnum opType,
    NamespaceString nss,
    UUID uuid,
    BSONObj oField,
    boost::optional<BSONObj> o2Field,
    OperationSessionInfo sessionInfo,
    std::vector<StmtId> stmtIds,
    boost::optional<repl::OpTime> preImageOpTime = boost::none,
    boost::optional<repl::OpTime> postImageOpTime = boost::none,
    boost::optional<repl::RetryImageEnum> needsRetryImage = boost::none) {
    return {
        repl::DurableOplogEntry(opTime,                           // optime
                                opType,                           // opType
                                nss,                              // namespace
                                uuid,                             // uuid
                                boost::none,                      // fromMigrate
                                boost::none,                      // checkExistenceForDiffInsert
                                boost::none,                      // versionContext
                                repl::OplogEntry::kOplogVersion,  // version
                                oField,                           // o
                                o2Field,                          // o2
                                sessionInfo,                      // sessionInfo
                                boost::none,                      // upsert
                                Date_t(),                         // wall clock time
                                stmtIds,                          // statement ids
                                repl::OpTime(),  // optime of previous write within same transaction
                                preImageOpTime,  // pre-image optime
                                postImageOpTime,    // post-image optime
                                boost::none,        // ShardId of resharding recipient
                                boost::none,        // _id
                                needsRetryImage)};  // needsRetryImage
}

/**
 * Creates OplogEntry with given field values.
 */
repl::OplogEntry makeOplogEntry(
    repl::OpTime opTime,
    repl::OpTypeEnum opType,
    NamespaceString nss,
    UUID uuid,
    BSONObj oField,
    OperationSessionInfo sessionInfo,
    std::vector<StmtId> stmtIds,
    boost::optional<repl::OpTime> preImageOpTime = boost::none,
    boost::optional<repl::OpTime> postImageOpTime = boost::none,
    boost::optional<repl::RetryImageEnum> needsRetryImage = boost::none) {
    return makeOplogEntry(opTime,
                          opType,
                          nss,
                          uuid,
                          oField,
                          boost::none /* o2Field */,
                          sessionInfo,
                          stmtIds,
                          preImageOpTime,
                          postImageOpTime,
                          needsRetryImage);
}

struct MockMongoInterface final : public StubMongoProcessInterface {
    MockMongoInterface(std::vector<Document> documentsForLookup = {})
        : _documentsForLookup{std::move(documentsForLookup)} {}

    boost::optional<Document> lookupSingleDocumentLocally(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey) final {
        Matcher matcher(documentKey.toBson(), expCtx);
        auto it =
            std::find_if(_documentsForLookup.begin(),
                         _documentsForLookup.end(),
                         [&](const Document& lookedUpDoc) {
                             return exec::matcher::matches(&matcher, lookedUpDoc.toBson(), nullptr);
                         });
        return (it != _documentsForLookup.end() ? *it : boost::optional<Document>{});
    }

    // These documents are used to feed the 'lookupSingleDocument' method.
    std::vector<Document> _documentsForLookup;
};

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
class FindAndModifyImageLookupTest : public AggregationContextFixture {
public:
    FindAndModifyImageLookupTest() : AggregationContextFixture() {}

    void mockImageDocument(const LogicalSessionId sessionId,
                           TxnNumber txnNum,
                           Timestamp ts,
                           repl::RetryImageEnum imageType,
                           BSONObj image) {
        repl::ImageEntry imageEntry;
        imageEntry.set_id(sessionId);
        imageEntry.setTxnNumber(txnNum);
        imageEntry.setTs(ts);
        imageEntry.setImageKind(imageType);
        imageEntry.setImage(image);
        getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(
            std::vector<Document>{Document{imageEntry.toBSON()}}));
    }

private:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagDisallowFindAndModifyImageCollection", false};
};

TEST_F(FindAndModifyImageLookupTest, NoopWhenEntryDoesNotHaveNeedsRetryImageField) {
    auto documentSourceImageLookup = DocumentSourceFindAndModifyImageLookup::create(getExpCtx());
    auto imageLookupStage = exec::agg::buildStage(documentSourceImageLookup);
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(1);
    const auto stmtId = 1;
    const auto opTime = repl::OpTime(Timestamp(2, 1), 1);
    const auto preImageOpTime = repl::OpTime(Timestamp(1, 1), 1);
    const auto oplogEntryBson =
        makeOplogEntry(opTime,
                       repl::OpTypeEnum::kNoop,
                       NamespaceString::createNamespaceString_forTest("test.foo"),
                       UUID::gen(),
                       BSON("a" << 1),
                       sessionInfo,
                       {stmtId},
                       preImageOpTime)
            .getEntry()
            .toBSON();
    auto mock = exec::agg::MockStage::createForTest(Document(oplogEntryBson), getExpCtx());
    imageLookupStage->setSource(mock.get());
    // Mock out the foreign collection.
    getExpCtx()->setMongoProcessInterface(
        std::make_unique<MockMongoInterface>(std::vector<Document>{}));

    auto next = imageLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected = Document(oplogEntryBson);
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    ASSERT_TRUE(imageLookupStage->getNext().isEOF());
    ASSERT_TRUE(imageLookupStage->getNext().isEOF());
    ASSERT_TRUE(imageLookupStage->getNext().isEOF());
}

TEST_F(FindAndModifyImageLookupTest,
       ShouldNotForgeImageEntryWhenMatchingImageDocOrSnapshotIsNotFoundCrudOp) {
    for (auto disallowImageCollection : {true, false}) {
        LOGV2(11731900,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "disallowImageCollection"_attr = disallowImageCollection);

        RAIIServerParameterControllerForTest featureFlagController(
            "featureFlagDisallowFindAndModifyImageCollection", disallowImageCollection);
        auto nss = NamespaceString::createNamespaceString_forTest("test.foo");

        boost::optional<FailPoint*> fp;
        boost::optional<int> timesEnteredBefore;
        if (disallowImageCollection) {
            // The image should be fetched from the snapshot rather than the image collection. Force
            // the snapshot read to fail with SnapshotTooOld.
            fp = globalFailPointRegistry().find(
                "failFindAndModifyImageLookupStageFindOneWithSnapshotTooOld");
            timesEnteredBefore =
                fp.get()->setMode(FailPoint::alwaysOn, 0, BSON("nss" << nss.toString_forTest()));
        }

        auto documentSourceImageLookup =
            DocumentSourceFindAndModifyImageLookup::create(getExpCtx());
        auto imageLookupStage = exec::agg::buildStage(documentSourceImageLookup);
        const auto sessionId = makeLogicalSessionIdForTest();
        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(sessionId);
        sessionInfo.setTxnNumber(1);
        const auto stmtId = 1;
        const auto opTime = repl::OpTime(Timestamp(2, 1), 1);
        const auto oplogEntryDoc = Document(makeOplogEntry(opTime,
                                                           repl::OpTypeEnum::kUpdate,
                                                           nss,
                                                           UUID::gen(),
                                                           BSON("$set" << BSON("a" << 1)),
                                                           BSON("_id" << 1),
                                                           sessionInfo,
                                                           {stmtId},
                                                           boost::none /* preImageOpTime */,
                                                           boost::none /* postImageOpTime */,
                                                           repl::RetryImageEnum::kPreImage)
                                                .getEntry()
                                                .toBSON());
        auto mock = exec::agg::MockStage::createForTest(oplogEntryDoc, getExpCtx());
        imageLookupStage->setSource(mock.get());

        // Mock out the foreign collection.
        getExpCtx()->setMongoProcessInterface(
            std::make_unique<MockMongoInterface>(std::vector<Document>{}));

        auto next = imageLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        // The needsRetryImage field should have been stripped even though we are not forging an
        // image entry.
        MutableDocument expected{oplogEntryDoc};
        expected.remove(repl::OplogEntryBase::kNeedsRetryImageFieldName);
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected.freeze());

        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
        ASSERT_TRUE(imageLookupStage->getNext().isEOF());

        if (fp) {
            auto timesEnteredAfter = fp.get()->setMode(FailPoint::off);
            ASSERT_EQ(timesEnteredAfter, *timesEnteredBefore + 1);
        }
    }
}

TEST_F(FindAndModifyImageLookupTest, ShouldNotForgeImageEntryWhenImageDocHasDifferentTxnNumber) {
    auto documentSourceImageLookup = DocumentSourceFindAndModifyImageLookup::create(getExpCtx());
    auto imageLookupStage = exec::agg::buildStage(documentSourceImageLookup);
    const auto sessionId = makeLogicalSessionIdForTest();
    const auto txnNum = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(txnNum);
    const auto stmtId = 1;
    const auto ts = Timestamp(2, 1);
    const auto opTime = repl::OpTime(ts, 1);
    const auto oplogEntryDoc =
        Document(makeOplogEntry(opTime,
                                repl::OpTypeEnum::kUpdate,
                                NamespaceString::createNamespaceString_forTest("test.foo"),
                                UUID::gen(),
                                BSON("a" << 1),
                                sessionInfo,
                                {stmtId},
                                boost::none /* preImageOpTime */,
                                boost::none /* postImageOpTime */,
                                repl::RetryImageEnum::kPreImage)
                     .getEntry()
                     .toBSON());
    auto mock = exec::agg::MockStage::createForTest(oplogEntryDoc, getExpCtx());
    imageLookupStage->setSource(mock.get());

    // Create an 'ImageEntry' with a higher 'txnNumber'.
    const auto preImage = BSON("a" << 2);
    mockImageDocument(sessionId, txnNum + 1, ts, repl::RetryImageEnum::kPreImage, preImage);

    auto next = imageLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    // The needsRetryImage field should have been stripped even though we are not forging an image
    // entry.
    MutableDocument expected{oplogEntryDoc};
    expected.remove(repl::OplogEntryBase::kNeedsRetryImageFieldName);
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected.freeze());

    ASSERT_TRUE(imageLookupStage->getNext().isEOF());
    ASSERT_TRUE(imageLookupStage->getNext().isEOF());
    ASSERT_TRUE(imageLookupStage->getNext().isEOF());
}

TEST_F(FindAndModifyImageLookupTest, ShouldForgeImageEntryWhenMatchingImageDocIsFoundCrudOp) {
    std::vector<repl::RetryImageEnum> cases{repl::RetryImageEnum::kPreImage,
                                            repl::RetryImageEnum::kPostImage};
    for (auto imageType : cases) {
        LOGV2(5806002,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "imageType"_attr = repl::RetryImage_serializer(imageType));
        auto documentSourceImageLookup =
            DocumentSourceFindAndModifyImageLookup::create(getExpCtx());
        auto imageLookupStage = exec::agg::buildStage(documentSourceImageLookup);
        const auto sessionId = makeLogicalSessionIdForTest();
        const auto txnNum = 1LL;
        const auto stmtId = 1;
        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(sessionId);
        sessionInfo.setTxnNumber(txnNum);
        const auto ts = Timestamp(2, 1);
        const auto opTime = repl::OpTime(ts, 1);
        const auto nss = NamespaceString::createNamespaceString_forTest("test.foo");
        const auto uuid = UUID::gen();

        // Define a findAndModify/update oplog entry with the 'needsRetryImage' field set.
        const auto oplogEntryBson = makeOplogEntry(opTime,
                                                   repl::OpTypeEnum::kUpdate,
                                                   nss,
                                                   uuid,
                                                   BSON("$set" << BSON("a" << 1)),
                                                   BSON("_id" << 1),
                                                   sessionInfo,
                                                   {stmtId},
                                                   boost::none /* preImageOpTime */,
                                                   boost::none /* postImageOpTime */,
                                                   imageType)
                                        .getEntry()
                                        .toBSON();

        auto mock = exec::agg::MockStage::createForTest(Document(oplogEntryBson), getExpCtx());
        imageLookupStage->setSource(mock.get());

        const auto prePostImage = BSON("a" << 2);
        mockImageDocument(sessionId, txnNum, ts, imageType, prePostImage);

        // The next doc should be the doc for the forged image oplog entry.
        auto next = imageLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        const auto forgedImageEntry =
            repl::OplogEntry::parse(next.releaseDocument().toBson()).getValue();
        ASSERT_BSONOBJ_EQ(prePostImage, forgedImageEntry.getObject());
        ASSERT_EQUALS(nss, forgedImageEntry.getNss());
        ASSERT_EQUALS(uuid, *forgedImageEntry.getUuid());
        ASSERT_EQUALS(txnNum, forgedImageEntry.getTxnNumber().value());
        ASSERT_EQUALS(sessionId, forgedImageEntry.getSessionId().value());
        ASSERT_EQUALS("n", repl::OpType_serializer(forgedImageEntry.getOpType()));
        const auto stmtIds = forgedImageEntry.getStatementIds();
        ASSERT_EQUALS(1U, stmtIds.size());
        ASSERT_EQUALS(stmtId, stmtIds.front());
        ASSERT_EQUALS(ts - 1, forgedImageEntry.getTimestamp());
        ASSERT_EQUALS(1, forgedImageEntry.getTerm().value());

        // The next doc should be the doc for the original findAndModify oplog entry with the
        // 'needsRetryImage' field removed and 'preImageOpTime'/'postImageOpTime' field appended.
        next = imageLookupStage->getNext();
        MutableDocument expectedDownConvertedDoc{Document{oplogEntryBson}};
        expectedDownConvertedDoc.remove(repl::OplogEntryBase::kNeedsRetryImageFieldName);
        const auto expectedImageOpTimeFieldName = imageType == repl::RetryImageEnum::kPreImage
            ? repl::OplogEntry::kPreImageOpTimeFieldName
            : repl::OplogEntry::kPostImageOpTimeFieldName;
        expectedDownConvertedDoc.setField(
            expectedImageOpTimeFieldName,
            Value{Document{
                {std::string{repl::OpTime::kTimestampFieldName}, forgedImageEntry.getTimestamp()},
                {std::string{repl::OpTime::kTermFieldName}, opTime.getTerm()}}});
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDownConvertedDoc.freeze());

        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
    }
}

TEST_F(FindAndModifyImageLookupTest, ShouldForgeImageEntryWhenMatchingImageDocIsFoundApplyOpsOp) {
    std::vector<repl::RetryImageEnum> cases{repl::RetryImageEnum::kPreImage,
                                            repl::RetryImageEnum::kPostImage};
    for (auto imageType : cases) {
        LOGV2(6344105,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "imageType"_attr = repl::RetryImage_serializer(imageType));
        auto documentSourceImageLookup = DocumentSourceFindAndModifyImageLookup::create(
            getExpCtx(), true /* includeCommitTransactionTimestamp */);
        auto imageLookupStage = exec::agg::buildStage(documentSourceImageLookup);
        const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
        const auto txnNum = 1LL;
        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(sessionId);
        sessionInfo.setTxnNumber(txnNum);
        const auto stmtId = 1;
        const auto applyOpsTs = Timestamp(2, 1);
        const auto applyOpsOpTime = repl::OpTime(applyOpsTs, 1);
        const auto commitTxnTs = Timestamp(3, 1);
        const auto commitTxnTsFieldName = CommitTransactionOplogObject::kCommitTimestampFieldName;
        const auto nss = NamespaceString::createNamespaceString_forTest("test.foo");
        const auto uuid = UUID::gen();

        // Define an applyOps oplog entry containing a findAndModify/update operation entry with
        // the 'needsRetryImage' field set.
        auto insertOp = repl::MutableOplogEntry::makeInsertOperation(
            nss, uuid, BSON("_id" << 0 << "a" << 0), BSON("_id" << 0));
        auto updateOp = repl::MutableOplogEntry::makeUpdateOperation(
            nss, uuid, BSON("$set" << BSON("a" << 1)), BSON("_id" << 1));
        updateOp.setStatementIds({stmtId});
        updateOp.setNeedsRetryImage(imageType);
        BSONObjBuilder applyOpsBuilder;
        applyOpsBuilder.append("applyOps", BSON_ARRAY(insertOp.toBSON() << updateOp.toBSON()));
        auto oplogEntryBson = makeOplogEntry(applyOpsOpTime,
                                             repl::OpTypeEnum::kCommand,
                                             {},
                                             UUID::gen(),
                                             applyOpsBuilder.obj(),
                                             sessionInfo,
                                             {})
                                  .getEntry()
                                  .toBSON()
                                  .addFields(BSON(commitTxnTsFieldName << commitTxnTs));

        auto mock = exec::agg::MockStage::createForTest(Document(oplogEntryBson), getExpCtx());
        imageLookupStage->setSource(mock.get());

        const auto prePostImage = BSON("_id" << 1);
        mockImageDocument(sessionId, txnNum, applyOpsTs, imageType, prePostImage);

        // The next doc should be the doc for the forged image oplog entry and it should contain the
        // commit transaction timestamp.
        auto next = imageLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        const auto forgedNoopOplogEntryBson = next.releaseDocument().toBson();
        ASSERT(forgedNoopOplogEntryBson.hasField(commitTxnTsFieldName));
        ASSERT_EQ(commitTxnTs, forgedNoopOplogEntryBson.getField(commitTxnTsFieldName).timestamp());
        const auto forgedImageEntry =
            repl::OplogEntry::parse(forgedNoopOplogEntryBson.removeField(commitTxnTsFieldName))
                .getValue();
        ASSERT_BSONOBJ_EQ(prePostImage, forgedImageEntry.getObject());
        ASSERT_EQUALS(nss, forgedImageEntry.getNss());
        ASSERT_EQUALS(uuid, *forgedImageEntry.getUuid());
        ASSERT_EQUALS(txnNum, forgedImageEntry.getTxnNumber().value());
        ASSERT_EQUALS(sessionId, forgedImageEntry.getSessionId().value());
        ASSERT_EQUALS("n", repl::OpType_serializer(forgedImageEntry.getOpType()));
        const auto stmtIds = forgedImageEntry.getStatementIds();
        ASSERT_EQUALS(1U, stmtIds.size());
        ASSERT_EQUALS(stmtId, stmtIds.front());
        ASSERT_EQUALS(applyOpsTs - 1, forgedImageEntry.getTimestamp());
        ASSERT_EQUALS(1, forgedImageEntry.getTerm().value());

        // The next doc should be the doc for original applyOps oplog entry but the
        // findAndModify/update operation entry should have 'needsRetryImage' field removed and
        // 'preImageOpTime'/'postImageOpTime' field appended.
        next = imageLookupStage->getNext();
        const auto downConvertedOplogEntryBson = next.releaseDocument().toBson();

        ASSERT_BSONOBJ_EQ(
            oplogEntryBson.removeField(repl::OplogEntry::kObjectFieldName),
            downConvertedOplogEntryBson.removeField(repl::OplogEntry::kObjectFieldName));

        auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(
            downConvertedOplogEntryBson.getObjectField(repl::OplogEntry::kObjectFieldName));
        auto operationDocs = applyOpsInfo.getOperations();
        ASSERT_EQ(operationDocs.size(), 2U);

        ASSERT_BSONOBJ_EQ(operationDocs[0], insertOp.toBSON());

        auto expectedUpdateOpBson =
            updateOp.toBSON().removeField(repl::OplogEntryBase::kNeedsRetryImageFieldName);
        const auto expectedImageOpTimeFieldName = imageType == repl::RetryImageEnum::kPreImage
            ? repl::OplogEntry::kPreImageOpTimeFieldName
            : repl::OplogEntry::kPostImageOpTimeFieldName;
        expectedUpdateOpBson = expectedUpdateOpBson.addFields(
            BSON(expectedImageOpTimeFieldName << forgedImageEntry.getOpTime()));
        ASSERT_EQ(operationDocs[1].getIntField("stmtId"),
                  expectedUpdateOpBson.getIntField("stmtId"));
        ASSERT_BSONOBJ_EQ(operationDocs[1].removeField("stmtId"),
                          expectedUpdateOpBson.removeField("stmtId"));

        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
    }
}
TEST_F(FindAndModifyImageLookupTest,
       ShouldNotForgeImageEntryWhenMatchingImageDocOrSnapshotIsNotFoundApplyOpsOp) {
    for (auto disallowImageCollection : {true, false}) {
        LOGV2(11731901,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "disallowImageCollection"_attr = disallowImageCollection);

        RAIIServerParameterControllerForTest featureFlagController(
            "featureFlagDisallowFindAndModifyImageCollection", disallowImageCollection);
        auto nss = NamespaceString::createNamespaceString_forTest("test.foo");

        boost::optional<FailPoint*> fp;
        boost::optional<int> timesEnteredBefore;
        if (disallowImageCollection) {
            // The image should be fetched from the snapshot rather than the image collection. Force
            // the snapshot read to fail with SnapshotTooOld.
            fp = globalFailPointRegistry().find(
                "failFindAndModifyImageLookupStageFindOneWithSnapshotTooOld");
            timesEnteredBefore =
                fp.get()->setMode(FailPoint::alwaysOn, 0, BSON("nss" << nss.toString_forTest()));
        }

        auto documentSourceImageLookup = DocumentSourceFindAndModifyImageLookup::create(
            getExpCtx(), true /* includeCommitTransactionTimestamp */);
        auto imageLookupStage = exec::agg::buildStage(documentSourceImageLookup);
        const auto sessionId = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
        const auto txnNum = 1LL;
        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(sessionId);
        sessionInfo.setTxnNumber(txnNum);
        const auto stmtId = 1;
        const auto applyOpsTs = Timestamp(2, 1);
        const auto applyOpsOpTime = repl::OpTime(applyOpsTs, 1);
        const auto commitTxnTs = applyOpsTs;
        const auto commitTxnTsFieldName = CommitTransactionOplogObject::kCommitTimestampFieldName;
        const auto uuid = UUID::gen();

        // Define an applyOps oplog entry containing a findAndModify/update operation entry with
        // the 'needsRetryImage' field set.
        auto insertOp = repl::MutableOplogEntry::makeInsertOperation(
            nss, uuid, BSON("_id" << 0 << "a" << 0), BSON("_id" << 0));
        auto updateOp = repl::MutableOplogEntry::makeUpdateOperation(
            nss, uuid, BSON("$set" << BSON("a" << 1)), BSON("_id" << 1));
        updateOp.setStatementIds({stmtId});
        updateOp.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
        BSONObjBuilder applyOpsBuilder;
        applyOpsBuilder.append("applyOps", BSON_ARRAY(insertOp.toBSON() << updateOp.toBSON()));
        auto oplogEntryUUID = UUID::gen();
        auto oplogEntryBson = makeOplogEntry(applyOpsOpTime,
                                             repl::OpTypeEnum::kCommand,
                                             {},
                                             oplogEntryUUID,
                                             applyOpsBuilder.obj(),
                                             sessionInfo,
                                             {})
                                  .getEntry()
                                  .toBSON()
                                  .addFields(BSON(commitTxnTsFieldName << commitTxnTs));

        auto mock = exec::agg::MockStage::createForTest(Document(oplogEntryBson), getExpCtx());
        imageLookupStage->setSource(mock.get());

        if (disallowImageCollection) {
            // Mock the oplog document since fetching the image from the snapshot involves
            // extracting the commit timestamp from the last oplog entry in the oplog chain.
            getExpCtx()->setMongoProcessInterface(std::make_unique<MockMongoInterface>(
                std::vector<Document>{Document{oplogEntryBson.removeField(commitTxnTsFieldName)}}));
        } else {
            // Mock out the foreign collection.
            getExpCtx()->setMongoProcessInterface(
                std::make_unique<MockMongoInterface>(std::vector<Document>{Document{}}));
        }

        // The next doc should be the doc for original applyOps oplog entry but the
        // findAndModify/update operation entry should have 'needsRetryImage' field removed.
        auto next = imageLookupStage->getNext();
        const auto downConvertedOplogEntryBson = next.releaseDocument().toBson();

        auto updateOpWithoutNeedsRetryImage = repl::MutableOplogEntry::makeUpdateOperation(
            nss, uuid, BSON("$set" << BSON("a" << 1)), BSON("_id" << 1));
        updateOpWithoutNeedsRetryImage.setStatementIds({stmtId});
        BSONObjBuilder applyOpsWithoutNeedsRetryImageBuilder;
        applyOpsWithoutNeedsRetryImageBuilder.append(
            "applyOps", BSON_ARRAY(insertOp.toBSON() << updateOpWithoutNeedsRetryImage.toBSON()));
        auto expectedOplogEntryBson = makeOplogEntry(applyOpsOpTime,
                                                     repl::OpTypeEnum::kCommand,
                                                     {},
                                                     oplogEntryUUID,
                                                     applyOpsWithoutNeedsRetryImageBuilder.obj(),
                                                     sessionInfo,
                                                     {})
                                          .getEntry()
                                          .toBSON()
                                          .addFields(BSON(commitTxnTsFieldName << commitTxnTs));

        ASSERT_BSONOBJ_EQ(expectedOplogEntryBson, downConvertedOplogEntryBson);

        auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(
            downConvertedOplogEntryBson.getObjectField(repl::OplogEntry::kObjectFieldName));
        auto operationDocs = applyOpsInfo.getOperations();
        ASSERT_EQ(operationDocs.size(), 2U);

        ASSERT_BSONOBJ_EQ(operationDocs[0], insertOp.toBSON());

        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
        ASSERT_TRUE(imageLookupStage->getNext().isEOF());
        ASSERT_TRUE(imageLookupStage->getNext().isEOF());

        if (fp) {
            auto timesEnteredAfter = fp.get()->setMode(FailPoint::off);
            ASSERT_EQ(timesEnteredAfter, *timesEnteredBefore + 1);
        }
    }
}
}  // namespace
}  // namespace mongo
