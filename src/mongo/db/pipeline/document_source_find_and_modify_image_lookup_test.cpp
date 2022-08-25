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

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/process_interface/stub_lookup_single_document_process_interface.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

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
                                repl::OplogEntry::kOplogVersion,  // version
                                oField,                           // o
                                boost::none,                      // o2
                                sessionInfo,                      // sessionInfo
                                boost::none,                      // upsert
                                Date_t(),                         // wall clock time
                                stmtIds,                          // statement ids
                                boost::none,     // optime of previous write within same transaction
                                preImageOpTime,  // pre-image optime
                                postImageOpTime,    // post-image optime
                                boost::none,        // ShardId of resharding recipient
                                boost::none,        // _id
                                needsRetryImage)};  // needsRetryImage
}

struct MockMongoInterface final : public StubMongoProcessInterface {
    MockMongoInterface(std::vector<Document> documentsForLookup = {})
        : _documentsForLookup{std::move(documentsForLookup)} {}

    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) {
        static const UUID* oplog_uuid = new UUID(UUID::gen());
        return BSON("uuid" << *oplog_uuid);
    }

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) final {
        Matcher matcher(documentKey.toBson(), expCtx);
        auto it = std::find_if(_documentsForLookup.begin(),
                               _documentsForLookup.end(),
                               [&](const Document& lookedUpDoc) {
                                   return matcher.matches(lookedUpDoc.toBson(), nullptr);
                               });
        return (it != _documentsForLookup.end() ? *it : boost::optional<Document>{});
    }

    // These documents are used to feed the 'lookupSingleDocument' method.
    std::vector<Document> _documentsForLookup;
};

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using FindAndModifyImageLookupTest = AggregationContextFixture;

TEST_F(FindAndModifyImageLookupTest, NoopWhenEntryDoesNotHaveNeedsRetryImageField) {
    auto imageLookup = DocumentSourceFindAndModifyImageLookup::create(getExpCtx());
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(1);
    const auto stmtId = 1;
    const auto opTime = repl::OpTime(Timestamp(2, 1), 1);
    const auto preImageOpTime = repl::OpTime(Timestamp(1, 1), 1);
    const auto oplogEntryBson = makeOplogEntry(opTime,
                                               repl::OpTypeEnum::kNoop,
                                               NamespaceString("test.foo"),
                                               UUID::gen(),
                                               BSON("a" << 1),
                                               sessionInfo,
                                               {stmtId},
                                               preImageOpTime)
                                    .getEntry()
                                    .toBSON();
    auto mock = DocumentSourceMock::createForTest(Document(oplogEntryBson), getExpCtx());
    imageLookup->setSource(mock.get());
    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(std::vector<Document>{});

    auto next = imageLookup->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected = Document(oplogEntryBson);
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    ASSERT_TRUE(imageLookup->getNext().isEOF());
    ASSERT_TRUE(imageLookup->getNext().isEOF());
    ASSERT_TRUE(imageLookup->getNext().isEOF());
}

TEST_F(FindAndModifyImageLookupTest, ShouldNotForgeImageEntryWhenImageDocMissing) {
    auto imageLookup = DocumentSourceFindAndModifyImageLookup::create(getExpCtx());
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(1);
    const auto stmtId = 1;
    const auto opTime = repl::OpTime(Timestamp(2, 1), 1);
    const auto oplogEntryBson = makeOplogEntry(opTime,
                                               repl::OpTypeEnum::kUpdate,
                                               NamespaceString("test.foo"),
                                               UUID::gen(),
                                               BSON("a" << 1),
                                               sessionInfo,
                                               {stmtId},
                                               boost::none /* preImageOpTime */,
                                               boost::none /* postImageOpTime */,
                                               repl::RetryImageEnum::kPreImage)
                                    .getEntry()
                                    .toBSON();
    auto mock = DocumentSourceMock::createForTest(Document(oplogEntryBson), getExpCtx());
    imageLookup->setSource(mock.get());

    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(std::vector<Document>{});

    auto next = imageLookup->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected = Document(oplogEntryBson);
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    ASSERT_TRUE(imageLookup->getNext().isEOF());
    ASSERT_TRUE(imageLookup->getNext().isEOF());
    ASSERT_TRUE(imageLookup->getNext().isEOF());
}

TEST_F(FindAndModifyImageLookupTest, ShouldNotForgeImageEntryWhenImageDocHasDifferentTxnNumber) {
    auto imageLookup = DocumentSourceFindAndModifyImageLookup::create(getExpCtx());
    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(1);
    const auto stmtId = 1;
    const auto ts = Timestamp(2, 1);
    const auto opTime = repl::OpTime(ts, 1);
    const auto oplogEntryBson = makeOplogEntry(opTime,
                                               repl::OpTypeEnum::kUpdate,
                                               NamespaceString("test.foo"),
                                               UUID::gen(),
                                               BSON("a" << 1),
                                               sessionInfo,
                                               {stmtId},
                                               boost::none /* preImageOpTime */,
                                               boost::none /* postImageOpTime */,
                                               repl::RetryImageEnum::kPreImage)
                                    .getEntry()
                                    .toBSON();
    auto mock = DocumentSourceMock::createForTest(Document(oplogEntryBson), getExpCtx());
    imageLookup->setSource(mock.get());

    // Create an 'ImageEntry' with a higher 'txnNumber'.
    const auto preImage = BSON("a" << 2);
    repl::ImageEntry imageEntry;
    imageEntry.set_id(sessionId);
    imageEntry.setTxnNumber(2);
    imageEntry.setTs(ts);
    imageEntry.setImageKind(repl::RetryImageEnum::kPreImage);
    imageEntry.setImage(preImage);
    // Mock out the foreign collection.
    getExpCtx()->mongoProcessInterface =
        std::make_unique<MockMongoInterface>(std::vector<Document>{Document{imageEntry.toBSON()}});

    auto next = imageLookup->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected = Document(oplogEntryBson);
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    ASSERT_TRUE(imageLookup->getNext().isEOF());
    ASSERT_TRUE(imageLookup->getNext().isEOF());
    ASSERT_TRUE(imageLookup->getNext().isEOF());
}

TEST_F(FindAndModifyImageLookupTest, ShouldForgeImageEntryWhenMatchingImageDocIsFoundCrudOp) {
    std::vector<repl::RetryImageEnum> cases{repl::RetryImageEnum::kPreImage,
                                            repl::RetryImageEnum::kPostImage};
    for (auto imageType : cases) {
        LOGV2(5806002,
              "ForgeImageEntryTestCase",
              "imageType"_attr = repl::RetryImage_serializer(imageType));
        auto imageLookup = DocumentSourceFindAndModifyImageLookup::create(getExpCtx());
        const auto sessionId = makeLogicalSessionIdForTest();
        const auto txnNum = 1LL;
        const auto stmtId = 1;
        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(sessionId);
        sessionInfo.setTxnNumber(txnNum);
        const auto ts = Timestamp(2, 1);
        const auto opTime = repl::OpTime(ts, 1);
        const auto nss = NamespaceString("test.foo");
        const auto uuid = UUID::gen();

        // Define a findAndModify/update oplog entry with the 'needsRetryImage' field set.
        const auto oplogEntryBson = makeOplogEntry(opTime,
                                                   repl::OpTypeEnum::kUpdate,
                                                   nss,
                                                   uuid,
                                                   BSON("a" << 1),
                                                   sessionInfo,
                                                   {stmtId},
                                                   boost::none /* preImageOpTime */,
                                                   boost::none /* postImageOpTime */,
                                                   imageType)
                                        .getEntry()
                                        .toBSON();

        auto mock = DocumentSourceMock::createForTest(Document(oplogEntryBson), getExpCtx());
        imageLookup->setSource(mock.get());

        const auto prePostImage = BSON("a" << 2);
        repl::ImageEntry imageEntry;
        imageEntry.set_id(sessionId);
        imageEntry.setTxnNumber(txnNum);
        imageEntry.setTs(ts);
        imageEntry.setImageKind(imageType);
        imageEntry.setImage(prePostImage);
        // Mock out the foreign collection.
        getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
            std::vector<Document>{Document{imageEntry.toBSON()}});

        // The next doc should be the doc for the forged image oplog entry.
        auto next = imageLookup->getNext();
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
        next = imageLookup->getNext();
        MutableDocument expectedDownConvertedDoc{Document{oplogEntryBson}};
        expectedDownConvertedDoc.remove(repl::OplogEntryBase::kNeedsRetryImageFieldName);
        const auto expectedImageOpTimeFieldName = imageType == repl::RetryImageEnum::kPreImage
            ? repl::OplogEntry::kPreImageOpTimeFieldName
            : repl::OplogEntry::kPostImageOpTimeFieldName;
        expectedDownConvertedDoc.setField(
            expectedImageOpTimeFieldName,
            Value{Document{
                {repl::OpTime::kTimestampFieldName.toString(), forgedImageEntry.getTimestamp()},
                {repl::OpTime::kTermFieldName.toString(), opTime.getTerm()}}});
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedDownConvertedDoc.freeze());

        ASSERT_TRUE(imageLookup->getNext().isEOF());
        ASSERT_TRUE(imageLookup->getNext().isEOF());
        ASSERT_TRUE(imageLookup->getNext().isEOF());
    }
}

TEST_F(FindAndModifyImageLookupTest, ShouldForgeImageEntryWhenMatchingImageDocIsFoundApplyOpsOp) {
    std::vector<repl::RetryImageEnum> cases{repl::RetryImageEnum::kPreImage,
                                            repl::RetryImageEnum::kPostImage};
    for (auto imageType : cases) {
        LOGV2(6344105,
              "ForgeImageEntryTestCase",
              "imageType"_attr = repl::RetryImage_serializer(imageType));
        auto imageLookup = DocumentSourceFindAndModifyImageLookup::create(
            getExpCtx(), true /* includeCommitTransactionTimestamp */);
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
        const auto nss = NamespaceString("test.foo");
        const auto uuid = UUID::gen();

        // Define an applyOps oplog entry containing a findAndModify/update operation entry with
        // the 'needsRetryImage' field set.
        auto insertOp = repl::MutableOplogEntry::makeInsertOperation(
            nss, uuid, BSON("_id" << 0 << "a" << 0), BSON("_id" << 0));
        auto updateOp = repl::MutableOplogEntry::makeUpdateOperation(
            nss, uuid, BSON("$set" << BSON("a" << 1)), BSON("_id" << 1));
        updateOp.setStatementIds({{stmtId}});
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

        auto mock = DocumentSourceMock::createForTest(Document(oplogEntryBson), getExpCtx());
        imageLookup->setSource(mock.get());

        const auto prePostImage = BSON("_id" << 1);
        repl::ImageEntry imageEntry;
        imageEntry.set_id(sessionId);
        imageEntry.setTxnNumber(txnNum);
        imageEntry.setTs(applyOpsTs);
        imageEntry.setImageKind(imageType);
        imageEntry.setImage(prePostImage);
        // Mock out the foreign collection.
        getExpCtx()->mongoProcessInterface = std::make_unique<MockMongoInterface>(
            std::vector<Document>{Document{imageEntry.toBSON()}});

        // The next doc should be the doc for the forged image oplog entry and it should contain the
        // commit transaction timestamp.
        auto next = imageLookup->getNext();
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
        next = imageLookup->getNext();
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

        ASSERT_TRUE(imageLookup->getNext().isEOF());
        ASSERT_TRUE(imageLookup->getNext().isEOF());
        ASSERT_TRUE(imageLookup->getNext().isEOF());
    }
}
}  // namespace
}  // namespace mongo
