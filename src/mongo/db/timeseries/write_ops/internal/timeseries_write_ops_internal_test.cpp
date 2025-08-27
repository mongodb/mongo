/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/db/timeseries/timeseries_test_fixture.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::timeseries::write_ops::internal {
namespace {
class TimeseriesWriteOpsInternalTest : public TimeseriesTestFixture {
protected:
    void _setUpRetryableWrites(TxnNumber txnNumber,
                               std::unique_ptr<MongoDSessionCatalog::Session>& session);

    void _addExecutedStatementsToTransactionParticipant(const std::vector<StmtId>& stmtIds);

    mongo::write_ops::InsertCommandRequest _createInsertCommandRequest(
        const NamespaceString& nss,
        const std::vector<BSONObj>& measurements,
        boost::optional<std::vector<StmtId>&> stmtIds = boost::none,
        boost::optional<StmtId> stmtId = boost::none);

    void _testStageUnorderedWritesUnoptimized(
        const NamespaceString& nss,
        const std::vector<BSONObj>& userBatch,
        const std::vector<size_t>& expectedIndices,
        const std::vector<size_t>& docsToRetry,
        boost::optional<std::vector<StmtId>&> stmtIds = boost::none,
        boost::optional<StmtId> stmtId = boost::none,
        boost::optional<std::vector<StmtId>&> executedStmtIds = boost::none);
};

void TimeseriesWriteOpsInternalTest::_setUpRetryableWrites(
    TxnNumber txnNumber, std::unique_ptr<MongoDSessionCatalog::Session>& contextSession) {

    MongoDSessionCatalog::set(
        _opCtx->getServiceContext(),
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

    _opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    _opCtx->setTxnNumber(txnNumber);

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(_opCtx);
    contextSession = mongoDSessionCatalog->checkOutSession(_opCtx);
    auto txnParticipant = TransactionParticipant::get(_opCtx);
    txnParticipant.beginOrContinue(_opCtx,
                                   {*_opCtx->getTxnNumber()},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
}

void TimeseriesWriteOpsInternalTest::_addExecutedStatementsToTransactionParticipant(
    const std::vector<StmtId>& stmtIds) {
    auto txnParticipant = TransactionParticipant::get(_opCtx);
    txnParticipant.addCommittedStmtIds(_opCtx, stmtIds, repl::OpTime());
};

mongo::write_ops::InsertCommandRequest TimeseriesWriteOpsInternalTest::_createInsertCommandRequest(
    const NamespaceString& nss,
    const std::vector<BSONObj>& measurements,
    boost::optional<std::vector<StmtId>&> stmtIds,
    boost::optional<StmtId> stmtId) {

    mongo::write_ops::WriteCommandRequestBase base;
    if (stmtIds) {
        base.setStmtIds(stmtIds.get());
    }
    if (stmtId) {
        base.setStmtId(stmtId.get());
    }
    mongo::write_ops::InsertCommandRequest request(nss);
    request.setWriteCommandRequestBase(base);
    request.setDocuments(measurements);
    return request;
};

void TimeseriesWriteOpsInternalTest::_testStageUnorderedWritesUnoptimized(
    const NamespaceString& nss,
    const std::vector<BSONObj>& userBatch,
    const std::vector<size_t>& expectedIndices,
    const std::vector<size_t>& docsToRetry,
    boost::optional<std::vector<StmtId>&> stmtIds,
    boost::optional<StmtId> stmtId,
    boost::optional<std::vector<StmtId>&> executedStmtIds) {
    // Set up retryable writes.
    std::unique_ptr<MongoDSessionCatalog::Session> session;
    _setUpRetryableWrites(TxnNumber(), session);

    auto request = _createInsertCommandRequest(nss, userBatch, stmtIds, stmtId);
    boost::optional<UUID> optUuid = boost::none;
    std::vector<mongo::write_ops::WriteError> errors;

    if (executedStmtIds) {
        // Simulate some measurements having already been executed.
        _addExecutedStatementsToTransactionParticipant(executedStmtIds.get());
    }

    auto [preConditions, _] = timeseries::getCollectionPreConditionsAndIsTimeseriesLogicalRequest(
        _opCtx, nss, request, /*expectedUUID=*/boost::none);

    auto batches = write_ops::internal::stageUnorderedWritesToBucketCatalogUnoptimized(
        _opCtx,
        request,
        preConditions,
        0,
        request.getDocuments().size(),
        bucket_catalog::AllowQueryBasedReopening::kAllow,
        docsToRetry,
        optUuid,
        &errors);
    ASSERT_EQ(batches.size(), 1);
    auto batch = batches.front();
    ASSERT_EQ(batch->measurements.size(), expectedIndices.size());
    ASSERT_EQ(batch->userBatchIndices, expectedIndices);
}


TEST_F(TimeseriesWriteOpsInternalTest, TestRewriteIndicesForSubsetOfBatch) {
    auto tsOptions = _getTimeseriesOptions(_nsNoMeta);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> originalUserBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
    };

    std::vector<BSONObj> filteredUserBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)), /*Original Index = 0*/
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)), /*Original Index = 2*/
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)), /*Original Index = 3*/
    };

    std::vector<size_t> originalIndices{0, 2, 3};

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_nsNoMeta),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/false,
                                                  _compressBucketFuncUnused,
                                                  filteredUserBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/filteredUserBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  bucket_catalog::AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);
    ASSERT_OK(swWriteBatches);
    auto writeBatches = swWriteBatches.getValue();

    auto request = _createInsertCommandRequest(_ns1, filteredUserBatch);

    internal::rewriteIndicesForSubsetOfBatch(_opCtx, request, originalIndices, writeBatches);

    ASSERT_EQ(writeBatches.size(), 1);
    auto& batch = writeBatches.front();
    for (size_t i = 0; i < batch->measurements.size(); i++) {
        auto userBatchIndex = batch->userBatchIndices.at(i);
        ASSERT_EQ(batch->measurements[i].woCompare(originalUserBatch[userBatchIndex]), 0);
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, TestRewriteIndicesForSubsetOfBatchWithStmtIds) {
    // Simulate that we are performing retryable time-series writes.
    _opCtx->setLogicalSessionId(LogicalSessionId());
    _opCtx->setTxnNumber(TxnNumber());

    auto tsOptions = _getTimeseriesOptions(_nsNoMeta);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> originalUserBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
    };

    std::vector<StmtId> stmtIds{10, 20, 30};

    std::vector<BSONObj> filteredUserBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)), /*Original Index = 2*/
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)), /*Original Index = 0*/
    };

    std::vector<size_t> originalIndices{2, 0};

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_nsNoMeta),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/false,
                                                  _compressBucketFuncUnused,
                                                  filteredUserBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/filteredUserBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  bucket_catalog::AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);
    ASSERT_OK(swWriteBatches);
    auto writeBatches = swWriteBatches.getValue();

    auto request = _createInsertCommandRequest(_ns1, filteredUserBatch, stmtIds);

    internal::rewriteIndicesForSubsetOfBatch(_opCtx, request, originalIndices, writeBatches);

    ASSERT_EQ(writeBatches.size(), 1);
    auto& batch = writeBatches.front();

    for (size_t i = 0; i < batch->measurements.size(); i++) {
        auto userBatchIndex = batch->userBatchIndices.at(i);
        ASSERT_EQ(batch->measurements[i].woCompare(originalUserBatch[userBatchIndex]), 0);
        ASSERT_EQ(batch->stmtIds[i], stmtIds[userBatchIndex]);
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, TestRewriteIndicesForSubsetOfBatchWithSingleStmtId) {
    // Simulate that we are performing retryable time-series writes.
    _opCtx->setLogicalSessionId(LogicalSessionId());
    _opCtx->setTxnNumber(TxnNumber());

    auto tsOptions = _getTimeseriesOptions(_nsNoMeta);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> originalUserBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
    };

    std::vector<BSONObj> filteredUserBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)), /*Original Index = 2*/
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)), /*Original Index = 0*/
    };

    std::vector<size_t> originalIndices{2, 0};
    size_t stmtId = 10;

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_nsNoMeta),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/false,
                                                  _compressBucketFuncUnused,
                                                  filteredUserBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/filteredUserBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  bucket_catalog::AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);
    ASSERT_OK(swWriteBatches);
    auto writeBatches = swWriteBatches.getValue();

    auto request = _createInsertCommandRequest(_ns1, filteredUserBatch, boost::none, stmtId);

    internal::rewriteIndicesForSubsetOfBatch(_opCtx, request, originalIndices, writeBatches);

    ASSERT_EQ(writeBatches.size(), 1);
    auto& batch = writeBatches.front();

    for (size_t i = 0; i < batch->measurements.size(); i++) {
        auto userBatchIndex = batch->userBatchIndices.at(i);
        ASSERT_EQ(batch->measurements[i].woCompare(originalUserBatch[userBatchIndex]), 0);
        ASSERT_EQ(batch->stmtIds[i], stmtId + userBatchIndex);
    }
}

TEST_F(TimeseriesWriteOpsInternalTest, TestProcessErrorsForSubsetOfBatchWithErrors) {
    auto tsOptions = _getTimeseriesOptions(_nsNoMeta);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IS);
    const auto& bucketsColl = autoColl.getCollection();

    std::vector<BSONObj> originalUserBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON("x" << 1),
    };

    std::vector<BSONObj> filteredUserBatch{
        BSON("x" << 1),                                      /*Original Index = 2*/
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)), /*Original Index = 0*/
    };

    std::vector<size_t> originalIndices{2, 0};

    auto swWriteBatches = prepareInsertsToBuckets(_opCtx,
                                                  *_bucketCatalog,
                                                  bucketsColl.get(),
                                                  tsOptions,
                                                  _opCtx->getOpID(),
                                                  _getCollator(_nsNoMeta),
                                                  _getStorageCacheSizeBytes(),
                                                  /*earlyReturnOnError=*/false,
                                                  _compressBucketFuncUnused,
                                                  filteredUserBatch,
                                                  /*startIndex=*/0,
                                                  /*numDocsToStage=*/filteredUserBatch.size(),
                                                  /*docsToRetry=*/{},
                                                  bucket_catalog::AllowQueryBasedReopening::kAllow,
                                                  errorsAndIndices);
    ASSERT_OK(swWriteBatches);
    auto writeBatches = swWriteBatches.getValue();

    std::vector<mongo::write_ops::WriteError> errors;

    internal::processErrorsForSubsetOfBatch(_opCtx, errorsAndIndices, originalIndices, &errors);

    ASSERT_EQ(errors.size(), 1);
    auto& error = errors.front();
    ASSERT_EQ(error.getIndex(), 2);
}

TEST_F(TimeseriesWriteOpsInternalTest, StageUnorderedWritesToBucketCatalogHandlesDocsToRetry) {
    std::vector<BSONObj> userBatch{
        BSON(_metaField << _metaValue << _timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_metaField << _metaValue2 << _timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_metaField << _metaValue2 << _timeField << Date_t::fromMillisSinceEpoch(1)),
    };
    std::vector<size_t> docsToRetry{1};
    std::vector<bucket_catalog::UserBatchIndex> expectedIndices{1};
    _testStageUnorderedWritesUnoptimized(_ns1, userBatch, expectedIndices, docsToRetry);
}

TEST_F(TimeseriesWriteOpsInternalTest,
       StageUnorderedWritesToBucketCatalogHandlesExecutedStatementsNoMeta) {
    std::vector<BSONObj> userBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
    };
    std::vector<StmtId> stmtIds{0, 10, 20, 30, 40, 50};
    std::vector<StmtId> executedStmtIds{0, 10, 20};
    std::vector<bucket_catalog::UserBatchIndex> expectedIndices{3, 5, 4};
    _testStageUnorderedWritesUnoptimized(_nsNoMeta,
                                         userBatch,
                                         expectedIndices,
                                         /*docsToRetry=*/{},
                                         stmtIds,
                                         boost::none,
                                         executedStmtIds);
}

TEST_F(TimeseriesWriteOpsInternalTest,
       StageUnorderedWritesToBucketCatalogHandlesExecutedStatementsWithMeta) {
    std::vector<BSONObj> userBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
    };
    std::vector<StmtId> stmtIds{0, 10, 20, 30, 40, 50};
    std::vector<StmtId> executedStmtIds{0, 10, 20};
    std::vector<bucket_catalog::UserBatchIndex> expectedIndices{3, 5, 4};
    _testStageUnorderedWritesUnoptimized(_ns1,
                                         userBatch,
                                         expectedIndices,
                                         /*docsToRetry=*/{},
                                         stmtIds,
                                         boost::none,
                                         executedStmtIds);
}

TEST_F(TimeseriesWriteOpsInternalTest,
       StageUnorderedWritesToBucketCatalogHandlesDocsToRetryAndExecutedStatementsNoMeta) {
    std::vector<BSONObj> userBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
    };
    std::vector<StmtId> stmtIds{0, 10, 20, 30, 40, 50};
    std::vector<StmtId> executedStmtIds{0, 10, 20};
    std::vector<bucket_catalog::UserBatchIndex> expectedIndices{3, 4};
    std::vector<size_t> docsToRetry{1, 2, 3, 4};
    _testStageUnorderedWritesUnoptimized(
        _nsNoMeta, userBatch, expectedIndices, docsToRetry, stmtIds, boost::none, executedStmtIds);
}

TEST_F(TimeseriesWriteOpsInternalTest,
       StageUnorderedWritesToBucketCatalogHandlesDocsToRetryAndExecutedStatementsWithMeta) {
    std::vector<BSONObj> userBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
    };
    std::vector<StmtId> stmtIds{0, 10, 20, 30, 40, 50};
    std::vector<StmtId> executedStmtIds{0, 10, 20};
    std::vector<bucket_catalog::UserBatchIndex> expectedIndices{3, 4};
    std::vector<size_t> docsToRetry{1, 2, 3, 4};
    _testStageUnorderedWritesUnoptimized(
        _ns1, userBatch, expectedIndices, docsToRetry, stmtIds, boost::none, executedStmtIds);
}

TEST_F(
    TimeseriesWriteOpsInternalTest,
    StageUnorderedWritesToBucketCatalogHandlesDocsToRetryWithExecutedStatementsReverseOrderingNoMeta) {
    // Measurements are sorted in perfectly reverse order by time.
    std::vector<BSONObj> userBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
    };

    std::vector<StmtId> executedStmtIds{0, 20, 40, 50};
    std::vector<StmtId> stmtIds{0, 10, 20, 30, 40, 50};
    std::vector<size_t> docsToRetry{0, 1, 2, 3};
    std::vector<bucket_catalog::UserBatchIndex> expectedIndices{3, 1};
    _testStageUnorderedWritesUnoptimized(
        _nsNoMeta, userBatch, expectedIndices, docsToRetry, stmtIds, boost::none, executedStmtIds);
}

TEST_F(
    TimeseriesWriteOpsInternalTest,
    StageUnorderedWritesToBucketCatalogHandlesDocsToRetryWithExecutedStatementsReverseOrderingWithMeta) {
    // Measurements are sorted in perfectly reverse order by time.
    std::vector<BSONObj> userBatch{
        BSON(_timeField << Date_t::fromMillisSinceEpoch(5)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(4)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(3)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(2)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(1)),
        BSON(_timeField << Date_t::fromMillisSinceEpoch(0)),
    };

    std::vector<StmtId> executedStmtIds{0, 20, 40, 50};
    std::vector<StmtId> stmtIds{0, 10, 20, 30, 40, 50};
    std::vector<size_t> docsToRetry{0, 1, 2, 3};
    std::vector<bucket_catalog::UserBatchIndex> expectedIndices{3, 1};
    _testStageUnorderedWritesUnoptimized(
        _ns1, userBatch, expectedIndices, docsToRetry, stmtIds, boost::none, executedStmtIds);
}

}  // namespace
}  // namespace mongo::timeseries::write_ops::internal
