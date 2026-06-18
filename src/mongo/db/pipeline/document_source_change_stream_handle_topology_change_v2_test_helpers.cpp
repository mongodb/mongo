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

#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2_test_helpers.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h"

namespace mongo {
namespace test {
using namespace std::literals::string_view_literals;

const ChangeStreamShardTargeterMock::ReaderContextCallback
    V2StageTestHelpers::kEmptyShardTargeterCallback =
        [](ChangeStreamShardTargeterMock::TimestampOrDocument, ChangeStreamReaderContext&) {
        };

// MockWithUndoStage

MockWithUndoStage::MockWithUndoStage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     std::deque<exec::agg::GetNextResult> results)
    : Stage("DocumentSourceWithUndoMock"sv, expCtx), _queue(std::move(results)) {}

boost::intrusive_ptr<MockWithUndoStage> MockWithUndoStage::createForTest(
    std::deque<exec::agg::GetNextResult> results,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return make_intrusive<MockWithUndoStage>(expCtx, std::move(results));
}

void MockWithUndoStage::undo() {
    invariant(_undoResult);
    _queue.push_front(std::move(*_undoResult));
    _undoResult.reset();
}

exec::agg::GetNextResult MockWithUndoStage::doGetNext() {
    exec::agg::GetNextResult next = exec::agg::GetNextResult::makeEOF();
    if (!_queue.empty()) {
        next = std::move(_queue.front());
        _queue.pop_front();
    }
    _undoResult = next;
    return next;
}

// CursorManagerMock

CursorManagerMock::CursorManagerMock(
    const ChangeStream& changeStream,
    ChangeStreamReaderBuilder* readerBuilder,
    boost::optional<boost::intrusive_ptr<MockWithUndoStage>> stageForUndo)
    : _changeStream(changeStream), _readerBuilder(readerBuilder), _stageForUndo(stageForUndo) {}

void CursorManagerMock::initialize(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   V2Stage* stage,
                                   const ResumeTokenData& resumeTokenData) {
    _resumeToken.emplace(ResumeToken(resumeTokenData));
}

void CursorManagerMock::openCursorsOnDataShards(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    OperationContext* opCtx,
    Timestamp atClusterTime,
    const stdx::unordered_set<ShardId>& shardIds) {
    throwShardNotFoundExceptionIfRequired();
    std::for_each(shardIds.begin(), shardIds.end(), [&](const ShardId& shardId) {
        _currentlyTargetedDataShards.insert(shardId);
    });
}

void CursorManagerMock::openCursorOnConfigServer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    OperationContext* opCtx,
    Timestamp atClusterTime) {
    throwShardNotFoundExceptionIfRequired();
    throwRetryChangeStreamExceptionIfRequired();
    tassert(12013807,
            "expecting no prior call to openCursorOnConfigServer()",
            !_cursorOpenedOnConfigServer);
    _cursorOpenedOnConfigServer = true;
}

void CursorManagerMock::closeCursorsOnDataShards(const stdx::unordered_set<ShardId>& shardIds) {
    std::for_each(shardIds.begin(), shardIds.end(), [&](const ShardId& shardId) {
        _currentlyTargetedDataShards.erase(shardId);
    });
}

void CursorManagerMock::closeCursorOnConfigServer(OperationContext* opCtx) {
    tassert(12013808,
            "expecting prior call to openCursorOnConfigServer()",
            _cursorOpenedOnConfigServer);
    _cursorOpenedOnConfigServer = false;
}

bool CursorManagerMock::isCursorOnConfigServerOpen() const {
    return _cursorOpenedOnConfigServer;
}

const stdx::unordered_set<ShardId>& CursorManagerMock::getCurrentlyTargetedDataShards() const {
    return _currentlyTargetedDataShards;
}

const ChangeStream& CursorManagerMock::getChangeStream() const {
    return _changeStream;
}

void CursorManagerMock::enableUndoNextMode() {
    _undoNextMode.emplace(true);
}

void CursorManagerMock::disableUndoNextMode() {
    _undoNextMode.emplace(false);
}

void CursorManagerMock::undoGetNext() {
    _undoGetNextCalled = true;
    invariant(_stageForUndo);
    (*_stageForUndo)->undo();
}

void CursorManagerMock::setHighWaterMark(Timestamp highWaterMark) {
    _restoredHighWaterMark.emplace(highWaterMark);
}

Timestamp CursorManagerMock::getTimestampFromCurrentHighWaterMark() const {
    tassert(10657540,
            "expecting high watermark timestamp to be set in test",
            _highWaterMarkTimestamp.has_value());
    return *_highWaterMarkTimestamp;
}

bool CursorManagerMock::undoGetNextCalled() const {
    return _undoGetNextCalled;
}

boost::optional<bool> CursorManagerMock::getUndoNextMode() const {
    return _undoNextMode;
}

boost::optional<Timestamp> CursorManagerMock::getRestoredHighWaterMark() const {
    return _restoredHighWaterMark;
}

void CursorManagerMock::setTimestampForCurrentHighWaterMark(Timestamp ts) {
    _highWaterMarkTimestamp = ts;
}

bool CursorManagerMock::isInitialized() const {
    return _resumeToken.has_value();
}

ResumeToken CursorManagerMock::getResumeToken() const {
    return *_resumeToken;
}

bool CursorManagerMock::cursorOpenedOnConfigServer() const {
    return _cursorOpenedOnConfigServer;
}

void CursorManagerMock::setThrowShardNotFoundExceptions(int value) {
    _throwShardNotFoundExceptions = value;
}

void CursorManagerMock::setThrowRetryChangeStreamExceptions(int value) {
    _throwRetryChangeStreamExceptions = value;
}

void CursorManagerMock::throwShardNotFoundExceptionIfRequired() {
    if (_throwShardNotFoundExceptions > 0) {
        _throwShardNotFoundExceptions--;
        error_details::throwExceptionForStatus(
            Status(ErrorCodes::ShardNotFound, "shard not found"));
    }
}

void CursorManagerMock::throwRetryChangeStreamExceptionIfRequired() {
    if (_throwRetryChangeStreamExceptions > 0) {
        _throwRetryChangeStreamExceptions--;
        error_details::throwExceptionForStatus(
            Status(ErrorCodes::RetryChangeStream, "please retry change stream"));
    }
}

// DeadlineWaiterMock

void DeadlineWaiterMock::waitUntil(OperationContext* opCtx, Date_t deadline) {
    _lastUsedDeadline = deadline;
    if (!_status.isOK()) {
        error_details::throwExceptionForStatus(_status);
    }
}

Date_t DeadlineWaiterMock::getLastUsedDeadline() const {
    return _lastUsedDeadline;
}

void DeadlineWaiterMock::setStatus(Status status) {
    _status = status;
}

// Helper functions used in the tests.
// -----------------------------------

BSONObj buildHighWaterMarkToken(Timestamp ts) {
    return ResumeToken::makeHighWaterMarkToken(ts, 1 /* version */).toDocument().toBson();
}

DocumentSourceChangeStreamSpec buildChangeStreamSpec(Timestamp ts, ChangeStreamReadMode mode) {
    DocumentSourceChangeStreamSpec spec;
    spec.setIgnoreRemovedShards(mode == ChangeStreamReadMode::kIgnoreRemovedShards);
    spec.setResumeAfter(ResumeToken::makeHighWaterMarkToken(ts, 1 /* version */));
    return spec;
}

BSONObj getStageSpec() {
    return BSON(DocumentSourceChangeStreamHandleTopologyChangeV2::kStageName << BSONObj());
}

std::shared_ptr<V2Stage::Parameters> buildParametersForTest(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    int minAllocationToShardsPollPeriodSecs,
    ChangeStreamReaderBuilder* changeStreamReaderBuilder,
    DataToShardsAllocationQueryService* dataToShardsAllocationQueryService,
    boost::optional<boost::intrusive_ptr<MockWithUndoStage>> stageForUndo) {
    ChangeStream changeStream = ChangeStream::buildFromExpressionContext(expCtx);
    return std::make_shared<V2Stage::Parameters>(
        changeStream,
        change_stream::resolveResumeTokenFromSpec(expCtx, *expCtx->getChangeStreamSpec()),
        minAllocationToShardsPollPeriodSecs,
        std::make_unique<DeadlineWaiterMock>(),
        std::make_unique<CursorManagerMock>(changeStream, changeStreamReaderBuilder, stageForUndo),
        changeStreamReaderBuilder,
        dataToShardsAllocationQueryService);
}

DataToShardsAllocationQueryServiceMock* getDataToShardsAllocationQueryServiceMock(
    std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<DataToShardsAllocationQueryServiceMock*>(
        params->dataToShardsAllocationQueryService);
}

ChangeStreamReaderBuilderMock* getChangeStreamReaderBuilderMock(
    std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<ChangeStreamReaderBuilderMock*>(params->changeStreamReaderBuilder);
}

ChangeStreamShardTargeterMock* getChangeStreamShardTargeterMock(
    std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<ChangeStreamShardTargeterMock*>(
        getChangeStreamReaderBuilderMock(params)->getShardTargeter());
}

CursorManagerMock* getCursorManagerMock(std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<CursorManagerMock*>(params->cursorManager.get());
}

DeadlineWaiterMock* getDeadlineWaiterMock(std::shared_ptr<V2Stage::Parameters>& params) {
    return static_cast<DeadlineWaiterMock*>(params->deadlineWaiter.get());
}

ClockSourceMock* getPreciseClockSource(ServiceContext* serviceContext) {
    return dynamic_cast<ClockSourceMock*>(serviceContext->getPreciseClockSource());
}

}  // namespace test
}  // namespace mongo
