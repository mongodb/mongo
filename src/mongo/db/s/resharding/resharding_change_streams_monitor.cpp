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

#include "mongo/db/s/resharding/resharding_change_streams_monitor.h"

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(failReshardingChangeStreamsMonitorAfterProcessingBatch);
MONGO_FAIL_POINT_DEFINE(hangReshardingChangeStreamsMonitorBeforeReceivingNextBatch);
MONGO_FAIL_POINT_DEFINE(hangReshardingChangeStreamsMonitorBeforeKillingCursors);

const StringData kAggregateCommentFieldName = "reshardingChangeStreamsMonitor"_sd;
const StringData kCommonUUIDFieldName = "commonUUID"_sd;
const StringData kReshardingUUIDFieldName = "reshardingUUID"_sd;

const UUID commonUUID = UUID::gen();

/**
 * Runs $currentOp to check if there are open cursors with the given namespace and comment.
 * If there are, returns their cursor ids.
 */
std::vector<CursorId> lookUpCursorIds(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const BSONObj& aggComment) {
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$currentOp" << BSON("allUsers" << true << "idleCursors" << true)));
    pipeline.push_back(BSON("$match" << BSON("cursor.originatingCommand.comment"
                                             << aggComment << "ns"
                                             << NamespaceStringUtil::serialize(
                                                    nss, SerializationContext::stateDefault()))));

    DBDirectClient client(opCtx);
    AggregateCommandRequest aggRequest(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin), pipeline);
    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        &client, aggRequest, true /* secondaryOk */, false /* useExhaust*/));

    std::vector<CursorId> cursorIds;
    while (cursor->more()) {
        auto doc = cursor->next();
        auto cursorObj = doc.getObjectField("cursor");
        cursorIds.push_back(cursorObj["cursorId"].Long());
    }
    return cursorIds;
}

/**
 * Fulfills the given promise based on the given status.
 */
void fulfilledPromise(SharedPromise<void>& sp, Status status) {
    if (status.isOK()) {
        sp.emplaceValue();
    } else {
        sp.setError(status);
    }
}

/**
 * Returns the role for monitor based on the namespace it is monitoring.
 */
ReshardingChangeStreamsMonitor::Role getRole(NamespaceString monitorNss) {
    return monitorNss.isTemporaryReshardingCollection()
        ? ReshardingChangeStreamsMonitor::Role::kRecipient
        : ReshardingChangeStreamsMonitor::Role::kDonor;
}

}  // namespace

ReshardingChangeStreamsMonitor::ReshardingChangeStreamsMonitor(
    UUID reshardingUUID,
    NamespaceString monitorNss,
    Timestamp startAtOperationTime,
    boost::optional<BSONObj> startAfterResumeToken,
    BatchProcessedCallback callback)
    : _reshardingUUID(reshardingUUID),
      _monitorNss(monitorNss),
      _startAtOperationTime(startAtOperationTime),
      _startAfterResumeToken(startAfterResumeToken),
      _role(getRole(monitorNss)),
      _batchProcessedCallback(callback) {
    uassert(10220100,
            "Expected the resume token to be non-empty",
            !_startAfterResumeToken.has_value() || !_startAfterResumeToken->isEmpty());
}

SemiFuture<void> ReshardingChangeStreamsMonitor::startMonitoring(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    if (_finalEventPromise) {
        return SemiFuture<void>::makeReady(
            Status{ErrorCodes::Error{1006687}, "Cannot start monitoring more than once"});
    }

    _finalEventPromise = std::make_unique<SharedPromise<void>>();
    _cleanupPromise = std::make_unique<SharedPromise<void>>();

    return ExecutorFuture<void>(executor)
        .then([this, executor, cancelToken, factory]() {
            if (_startAfterResumeToken) {
                LOGV2(1006684,
                      "The change streams monitor is resuming",
                      "reshardingUUID"_attr = _reshardingUUID);
            } else {
                LOGV2(1006683,
                      "The change streams monitor is starting",
                      "reshardingUUID"_attr = _reshardingUUID,
                      "startAtOperationTime"_attr = _startAtOperationTime);
            }

            return _consumeChangeEvents(executor, cancelToken, factory);
        })
        .onCompletion([this, anchor = shared_from_this()](Status status) {
            LOGV2(9981901,
                  "The resharding change streams monitor finished waiting for final event",
                  "reshardingUUID"_attr = _reshardingUUID,
                  "status"_attr = status);

            if (status.isOK() && !_receivedFinalEvent) {
                status = {ErrorCodes::Error{1006688},
                          "Expect to have consumed the final event since the monitor ran to "
                          "completion without any error"};
            }

            fulfilledPromise(*_finalEventPromise, status);
            return status;
        })
        .thenRunOn(cleanupExecutor)
        .onCompletion([this, anchor = shared_from_this()](Status finalEventStatus) {
            auto opCtx = cc().makeOperationContext();
            auto killCursorsStatus = killCursors(opCtx.get());

            LOGV2(1006685,
                  "The resharding change streams monitor finished cleaning up",
                  "reshardingUUID"_attr = _reshardingUUID,
                  "finalEventStatus"_attr = finalEventStatus,
                  "killCursorsStatus"_attr = killCursorsStatus);

            if (!_finalEventPromise->getFuture().isReady()) {
                fulfilledPromise(*_finalEventPromise, finalEventStatus);
            }
            fulfilledPromise(*_cleanupPromise, killCursorsStatus);
        })
        .semi();
}

SharedSemiFuture<void> ReshardingChangeStreamsMonitor::awaitFinalChangeEvent() {
    tassert(1009073,
            "Attempted to wait for the resharding change streams monitor to consume the final "
            "change event without starting it",
            _finalEventPromise);

    return _finalEventPromise->getFuture();
}

SharedSemiFuture<void> ReshardingChangeStreamsMonitor::awaitCleanup() {
    tassert(1006686,
            "Attempted to wait for the resharding change streams monitor to clean up without "
            "starting it",
            _cleanupPromise);

    return _cleanupPromise->getFuture();
}

int64_t ReshardingChangeStreamsMonitor::numEventsTotalForTest() {
    return _numEventsTotal;
}

int64_t ReshardingChangeStreamsMonitor::numBatchesForTest() {
    return _numBatches;
}

std::vector<BSONObj> ReshardingChangeStreamsMonitor::_makeAggregatePipeline() const {
    DocumentSourceChangeStreamSpec changeStreamSpec;

    // This field must be enabled so that change streams return the 'commitTimestamp' field for
    // events that are part of a prepared transaction.
    changeStreamSpec.setShowCommitTimestamp(true);

    changeStreamSpec.setAllowToRunOnSystemNS(_monitorNss.isSystem());
    // The monitor for a recipient needs to set 'showMigrationEvents' to true since the events
    // against the temporary resharding collection are only output when 'showMigrationEvents'
    // is true. The monitor for a donor needs to set 'showMigrationEvents' to false to avoid
    // capturing range deletions.
    changeStreamSpec.setShowMigrationEvents(_role == Role::kRecipient);
    // The monitor for a donor needs to set "showSystemEvents" to true since the
    // 'reshardBlockingWrites' event, which is the final event on a donor, is only output when
    // "showSystemEvents" is true.
    changeStreamSpec.setShowSystemEvents(_role == Role::kDonor);
    if (_startAfterResumeToken) {
        auto resumeToken = ResumeToken::parse(*_startAfterResumeToken);
        changeStreamSpec.setStartAfter(std::move(resumeToken));
    } else {
        changeStreamSpec.setStartAtOperationTime(_startAtOperationTime);
    }

    BSONArrayBuilder operationTypesArrayBuilder;
    operationTypesArrayBuilder.append(DocumentSourceChangeStream::kInsertOpType);
    operationTypesArrayBuilder.append(DocumentSourceChangeStream::kDeleteOpType);
    if (_role == Role::kRecipient) {
        operationTypesArrayBuilder.append(DocumentSourceChangeStream::kReshardDoneCatchUpOpType);
    } else {
        operationTypesArrayBuilder.append(DocumentSourceChangeStream::kReshardBlockingWritesOpType);
    }

    auto matchStage = [&] {
        if (_role == Role::kRecipient) {
            return BSON("$match" << BSON(DocumentSourceChangeStream::kOperationTypeField
                                         << BSON("$in" << operationTypesArrayBuilder.arr())));
        }

        // The monitor for a donor needs to additionally filter out events for prepared transactions
        // have commit timestamp (i.e. visible timestamp) less than the 'startAtOperationTime' but
        // commit oplog entry after the 'startAtOperationTime'. The monitor for a recipient does
        // not need to do this because resharding cloning does not involve running prepared
        // (cross-shard) transactions.

        BSONArrayBuilder matchAndArrayBuilder;
        matchAndArrayBuilder.append(BSON(DocumentSourceChangeStream::kOperationTypeField
                                         << BSON("$in" << operationTypesArrayBuilder.arr())));

        BSONArrayBuilder commitTimestampArrayBuilder;
        commitTimestampArrayBuilder.append(
            BSON(DocumentSourceChangeStream::kCommitTimestampField << BSON("$exists" << false)));
        commitTimestampArrayBuilder.append(BSON(DocumentSourceChangeStream::kCommitTimestampField
                                                << BSON("$gte" << _startAtOperationTime)));
        matchAndArrayBuilder.append(BSON("$or" << commitTimestampArrayBuilder.arr()));

        return BSON("$match" << BSON("$and" << matchAndArrayBuilder.arr()));
    }();

    BSONObj changeStreamStage =
        BSON(DocumentSourceChangeStream::kStageName << changeStreamSpec.toBSON());

    BSONObj projectStage = BSON("$project" << BSON("operationType" << 1));

    return {std::move(changeStreamStage), std::move(matchStage), std::move(projectStage)};
}

BSONObj ReshardingChangeStreamsMonitor::makeAggregateComment(const UUID& reshardingUUID) {
    // The common UUID is included to prevent accidentally killing other cursors whose
    // originating command happens to include the resharding UUID in its 'comment'.
    return BSON(kAggregateCommentFieldName
                << BSON(kCommonUUIDFieldName << commonUUID.toString() << kReshardingUUIDFieldName
                                             << reshardingUUID.toString()));
}

AggregateCommandRequest ReshardingChangeStreamsMonitor::makeAggregateCommandRequest() {
    auto pipeline = _makeAggregatePipeline();
    AggregateCommandRequest aggRequest(_monitorNss, std::move(pipeline));

    auto batchSize = std::min(
        (long long)resharding::gReshardingVerificationChangeStreamsEventsBatchSizeLimit.load(),
        aggregation_request_helper::kDefaultBatchSize);
    mongo::SimpleCursorOptions cursorOptions;
    cursorOptions.setBatchSize(batchSize);
    aggRequest.setCursor(std::move(cursorOptions));

    aggRequest.setComment(
        mongo::IDLAnyTypeOwned(BSON("" << makeAggregateComment(_reshardingUUID)).firstElement()));

    return aggRequest;
}

std::unique_ptr<mongo::DBClientCursor> ReshardingChangeStreamsMonitor::_makeDBClientCursor(
    DBDirectClient* client) {
    // Use exhaust to make the cursor eagerly generate the next batches as soon as the events are
    // ready to make fetching faster.
    bool useExhaust = true;
    // Prevent the cursor from being killed after this DBClientCursor is thrown away after each
    // iteration. The cursor will be killed at the end using the cleanup executor.
    bool keepCursorOpen = true;

    if (!_cursorId) {
        auto aggRequest = makeAggregateCommandRequest();
        auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
            client, aggRequest, false /* secondaryOk */, useExhaust, keepCursorOpen));
        _cursorId = cursor->getCursorId();
        return cursor;
    }
    return std::make_unique<DBClientCursor>(client,
                                            _monitorNss,
                                            *_cursorId,
                                            useExhaust,
                                            std::vector<BSONObj>{} /* initialBatch */,
                                            boost::none /* operationTime*/,
                                            boost::none /* postBatchResumeToken*/,
                                            keepCursorOpen);
}

ExecutorFuture<void> ReshardingChangeStreamsMonitor::_consumeChangeEvents(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    return AsyncTry([this, factory] {
               EventBatch batch(_role);

               auto opCtx = factory.makeOperationContext(&cc());
               DBDirectClient client(opCtx.get());
               auto cursor = _makeDBClientCursor(&client);

               while (!batch.shouldDispose()) {
                   if (cursor->more()) {
                       auto doc = cursor->next();
                       batch.add(doc);
                   } else {
                       // TODO (SERVER-101189): Make each getMore command in
                       // ReshardingChangeStreamsMonitor wait longer for change events.
                       opCtx->sleepFor(Milliseconds(
                           resharding::gReshardingVerificationChangeStreamsSleepMS.load()));
                   }
               }

               // If there are remaining events in the last getMore batch, process all of them
               // unless we have reached the final event.
               while (!batch.containsFinalEvent() && cursor->moreInCurrentBatch()) {
                   auto doc = cursor->next();
                   batch.add(doc);
               }

               uassert(10178201,
                       str::stream()
                           << "Expected '" << ResponseCursorBase::kPostBatchResumeTokenFieldName
                           << "' to be available",
                       cursor->getPostBatchResumeToken().has_value());
               uassert(10178202,
                       str::stream()
                           << "Expected '" << ResponseCursorBase::kPostBatchResumeTokenFieldName
                           << "' to be non-empty",
                       !cursor->getPostBatchResumeToken()->isEmpty());
               batch.setResumeToken(*cursor->getPostBatchResumeToken());

               {
                   // Create an alternative client so the callback can create its own opCtx to do
                   // writes without impacting the opCtx used to open the change stream cursor.
                   auto newClient =
                       opCtx->getServiceContext()->getService()->makeClient("AlternativeClient");
                   AlternativeClientRegion acr(newClient);
                   _batchProcessedCallback(batch);
               }

               _numEventsTotal += batch.getNumEvents();
               _numBatches++;
               _receivedFinalEvent = batch.containsFinalEvent();

               if (MONGO_unlikely(
                       failReshardingChangeStreamsMonitorAfterProcessingBatch.shouldFail())) {
                   uasserted(ErrorCodes::InternalError, "Failing for failpoint");
               }
               hangReshardingChangeStreamsMonitorBeforeReceivingNextBatch.pauseWhileSet();
           })
        .until([this](Status status) { return _receivedFinalEvent || !status.isOK(); })
        .on(executor, cancelToken);
}

Status ReshardingChangeStreamsMonitor::killCursors(OperationContext* opCtx) {
    hangReshardingChangeStreamsMonitorBeforeKillingCursors.pauseWhileSet();

    auto aggComment = makeAggregateComment(_reshardingUUID);
    auto cursorIds = lookUpCursorIds(opCtx, _monitorNss, aggComment);

    if (cursorIds.empty()) {
        return Status::OK();
    }

    LOGV2(1006680,
          "Found idle change stream cursors",
          "reshardingUUID"_attr = _reshardingUUID,
          "cursorIds"_attr = cursorIds);

    DBDirectClient client(opCtx);
    BSONObj result;
    client.runCommand(
        _monitorNss.dbName(), KillCursorsCommandRequest(_monitorNss, cursorIds).toBSON(), result);
    auto status = getStatusFromCommandResult(result);

    if (!status.isOK()) {
        LOGV2(1006681,
              "Failed to kill the idle change stream cursors",
              "reshardingUUID"_attr = _reshardingUUID,
              "error"_attr = status);
    } else {
        LOGV2(1006682,
              "Killed the idle change stream cursors",
              "reshardingUUID"_attr = _reshardingUUID);
    }
    return status;
}

ReshardingChangeStreamsMonitor::EventBatch::EventBatch(Role role)
    : _role(role), _createdAt(Date_t::now()) {}

void ReshardingChangeStreamsMonitor::EventBatch::add(const BSONObj& event) {
    const StringData eventOpType =
        event.getStringField(DocumentSourceChangeStream::kOperationTypeField);

    if (eventOpType == DocumentSourceChangeStream::kInsertOpType) {
        _documentsDelta += 1;
    } else if (eventOpType == DocumentSourceChangeStream::kDeleteOpType) {
        _documentsDelta -= 1;
    } else if (eventOpType == DocumentSourceChangeStream::kReshardDoneCatchUpOpType &&
               _role == Role::kRecipient) {
        _containsFinalEvent = true;
    } else if (eventOpType == DocumentSourceChangeStream::kReshardBlockingWritesOpType &&
               _role == Role::kDonor) {
        _containsFinalEvent = true;
    } else {
        LOGV2(9981902,
              "Unrecognized event while processing change stream event for resharding validation",
              "Event"_attr = event.toString());
    }

    _numEvents += 1;
}

bool ReshardingChangeStreamsMonitor::EventBatch::shouldDispose() {
    if (_containsFinalEvent) {
        return true;
    }

    auto batchSizeLimit =
        resharding::gReshardingVerificationChangeStreamsEventsBatchSizeLimit.load();
    auto batchTimeLimit =
        Seconds(resharding::gReshardingVerificationChangeStreamsEventsBatchTimeLimitSeconds.load());
    return _numEvents >= batchSizeLimit || (Date_t::now() - _createdAt) >= batchTimeLimit;
}

void ReshardingChangeStreamsMonitor::EventBatch::setResumeToken(BSONObj resumeToken) {
    _resumeToken = resumeToken;
}

bool ReshardingChangeStreamsMonitor::EventBatch::containsFinalEvent() const {
    return _containsFinalEvent;
}

bool ReshardingChangeStreamsMonitor::EventBatch::empty() const {
    return _numEvents == 0;
}

int64_t ReshardingChangeStreamsMonitor::EventBatch::getNumEvents() const {
    return _numEvents;
}

int64_t ReshardingChangeStreamsMonitor::EventBatch::getDocumentsDelta() const {
    return _documentsDelta;
}

BSONObj ReshardingChangeStreamsMonitor::EventBatch::getResumeToken() const {
    return _resumeToken;
}

}  // namespace mongo
