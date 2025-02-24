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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/s/resharding/resharding_change_streams_monitor.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(failReshardingChangeStreamsMonitorAfterProcessingBatch);
MONGO_FAIL_POINT_DEFINE(hangReshardingChangeStreamsMonitorBeforeRecievingNextBatch);

const StringData kAggregateCommentFieldName = "reshardingChangeStreamsMonitor"_sd;
const StringData kCommonUUIDFieldName = "commonUUID"_sd;
const StringData kReshardingUUIDFieldName = "reshardingUUID"_sd;

const UUID commonUUID = UUID::gen();

/**
 * Returns the 'comment' for the $changeStream aggregate command that is unique for the given
 * resharding UUID. The 'comment' is used to identify the cursors to kill when the monitor
 * completes. The common UUID generated above is included to prevent accidentally killing other
 * cursors whose originating command happens to include the resharding UUID in its 'comment'.
 */
BSONObj makeAggregateComment(const UUID& reshardingUUID) {
    return BSON(kAggregateCommentFieldName
                << BSON(kCommonUUIDFieldName << commonUUID.toString() << kReshardingUUIDFieldName
                                             << reshardingUUID.toString()));
}

/**
 * Runs $currentOp to check if there are open change stream cursors with the given resharding UUID.
 * If there are, returns their cursor ids.
 */
std::vector<CursorId> lookUpCursorIds(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const UUID& reshardingUUID) {
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$currentOp" << BSON("allUsers" << true << "idleCursors" << true)));
    pipeline.push_back(BSON("$match" << BSON("cursor.originatingCommand.comment"
                                             << makeAggregateComment(reshardingUUID) << "ns"
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
 * If there are open change stream cursors with the given resharding UUID, kills them and returns
 * the status.
 */
Status killCursors(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& reshardingUUID) {
    auto cursorIds = lookUpCursorIds(opCtx, nss, reshardingUUID);

    if (cursorIds.empty()) {
        return Status::OK();
    }

    LOGV2(1006680,
          "Found idle change stream cursors",
          "reshardingUUID"_attr = reshardingUUID,
          "cursorIds"_attr = cursorIds);

    DBDirectClient client(opCtx);
    BSONObj result;
    client.runCommand(nss.dbName(), KillCursorsCommandRequest(nss, cursorIds).toBSON(), result);
    auto status = getStatusFromCommandResult(result);

    if (!status.isOK()) {
        LOGV2(1006681,
              "Failed to kill the idle change stream cursors",
              "reshardingUUID"_attr = reshardingUUID,
              "error"_attr = status);
    } else {
        LOGV2(1006682,
              "Killed the idle change stream cursors",
              "reshardingUUID"_attr = reshardingUUID);
    }
    return status;
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

}  // namespace

ReshardingChangeStreamsMonitor::ReshardingChangeStreamsMonitor(UUID reshardingUUID,
                                                               NamespaceString monitorNss,
                                                               Timestamp startAtOperationTime,
                                                               BatchProcessedCallback callback)
    : _reshardingUUID(reshardingUUID),
      _monitorNss(monitorNss),
      _startAt(startAtOperationTime),
      _isRecipient(monitorNss.isTemporaryReshardingCollection()),
      _batchProcessedCallback(callback) {}

ReshardingChangeStreamsMonitor::ReshardingChangeStreamsMonitor(UUID reshardingUUID,
                                                               NamespaceString monitorNss,
                                                               BSONObj startAfterToken,
                                                               BatchProcessedCallback callback)
    : _reshardingUUID(reshardingUUID),
      _monitorNss(monitorNss),
      _startAfter(startAfterToken),
      _isRecipient(monitorNss.isTemporaryReshardingCollection()),
      _batchProcessedCallback(callback) {}

SemiFuture<void> ReshardingChangeStreamsMonitor::startMonitoring(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancelableOperationContextFactory factory) {
    if (_finalEventPromise || _cleanupPromise) {
        return SemiFuture<void>::makeReady(
            Status{ErrorCodes::Error{1006687}, "Cannot start monitoring more than once"});
    }

    _finalEventPromise = std::make_unique<SharedPromise<void>>();
    _cleanupPromise = std::make_unique<SharedPromise<void>>();
    return ExecutorFuture<void>(executor)
        .then([this, executor, factory]() {
            tassert(1009071,
                    str::stream() << "Expected the change streams monitor to have 'startAt' or "
                                     "'startAfter' but it has neither of them",
                    _startAt || _startAfter);
            tassert(1009072,
                    str::stream() << "Expected the change streams monitor to have 'startAt' or "
                                     "'startAfter' but it has both of them",
                    !_startAt || !_startAfter);

            if (_startAt) {
                LOGV2(1006683,
                      "The change streams monitor is starting",
                      "reshardingUUID"_attr = _reshardingUUID,
                      "startAtOperationTime"_attr = _startAt);
            } else {
                LOGV2(1006684,
                      "The change streams monitor is resuming",
                      "reshardingUUID"_attr = _reshardingUUID);
            }

            auto opCtx = factory.makeOperationContext(&cc());
            auto aggRequest = _makeAggregateCommandRequest();
            _consumeChangeEvents(opCtx.get(), aggRequest);
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
            auto killCursorsStatus = killCursors(opCtx.get(), _monitorNss, _reshardingUUID);

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

AggregateCommandRequest ReshardingChangeStreamsMonitor::_makeAggregateCommandRequest() {
    DocumentSourceChangeStreamSpec changeStreamSpec;
    changeStreamSpec.setAllowToRunOnSystemNS(_monitorNss.isSystem());
    // The monitor for a recipient needs to set 'showMigrationEvents' to true since the events
    // against the temporary resharding collection are only output when 'showMigrationEvents'
    // is true. The monitor for a donor needs to set 'showMigrationEvents' to false to avoid
    // capturing range deletions.
    changeStreamSpec.setShowMigrationEvents(_isRecipient);
    // The monitor for a donor needs to set "showSystemEvents" to true since the
    // 'reshardBlockingWrites' event, which is the final event on a donor, is only output when
    // "showSystemEvents" is true.
    changeStreamSpec.setShowSystemEvents(!_isRecipient);
    if (_startAt) {
        changeStreamSpec.setStartAtOperationTime(*_startAt);
    } else {
        auto resumeToken = ResumeToken::parse(*_startAfter);
        changeStreamSpec.setStartAfter(std::move(resumeToken));
    }
    BSONObj changeStreamStage =
        BSON(DocumentSourceChangeStream::kStageName << changeStreamSpec.toBSON());

    BSONArrayBuilder operationTypesArrayBuilder;
    operationTypesArrayBuilder.append(DocumentSourceChangeStream::kInsertOpType);
    operationTypesArrayBuilder.append(DocumentSourceChangeStream::kDeleteOpType);
    if (_isRecipient) {
        operationTypesArrayBuilder.append(DocumentSourceChangeStream::kReshardDoneCatchUpOpType);
    } else {
        operationTypesArrayBuilder.append(DocumentSourceChangeStream::kReshardBlockingWritesOpType);
    }
    BSONObj matchStage = BSON("$match" << BSON(DocumentSourceChangeStream::kOperationTypeField
                                               << BSON("$in" << operationTypesArrayBuilder.arr())));

    const std::vector<BSONObj> rawPipeline = {changeStreamStage, matchStage};

    AggregateCommandRequest aggRequest(_monitorNss, std::move(rawPipeline));
    aggRequest.setComment(
        mongo::IDLAnyTypeOwned(BSON("" << makeAggregateComment(_reshardingUUID)).firstElement()));
    return aggRequest;
}

void ReshardingChangeStreamsMonitor::_consumeChangeEvents(
    OperationContext* opCtx, const AggregateCommandRequest& aggRequest) {
    DBDirectClient client(opCtx);
    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        &client, aggRequest, false /* secondaryOk */, false /* useExhaust*/));

    while (!_receivedFinalEvent) {
        if (cursor->more()) {
            auto doc = cursor->next();
            _processChangeEvent(doc);

            if (_receivedFinalEvent ||
                _numEventsProcessed %
                        resharding::gReshardingVerificationChangeStreamsEventsBatchSize.load() ==
                    0) {
                const auto resumeToken = doc.getObjectField("_id");

                // Create an alternative client so the callback can create its own opCtx to do
                // writes without impacting the opCtx used to open the change stream cursor above.
                auto newClient =
                    opCtx->getServiceContext()->getService()->makeClient("AlternativeClient");
                AlternativeClientRegion acr(newClient);
                _batchProcessedCallback(_documentsDelta, resumeToken, _receivedFinalEvent);
                _documentsDelta = 0;

                if (MONGO_unlikely(
                        failReshardingChangeStreamsMonitorAfterProcessingBatch.shouldFail())) {
                    uasserted(ErrorCodes::InternalError, "Failing for failpoint");
                }
            }

            hangReshardingChangeStreamsMonitorBeforeRecievingNextBatch.pauseWhileSet();
        } else {
            // TODO (SERVER-101189): Make each getMore command in ReshardingChangeStreamsMonitor
            // wait longer for change events.
            opCtx->sleepFor(
                Milliseconds(resharding::gReshardingVerificationChangeStreamsSleepMS.load()));
        }
    }
}

void ReshardingChangeStreamsMonitor::_processChangeEvent(const BSONObj changeEvent) {
    const StringData eventOpType =
        changeEvent.getStringField(DocumentSourceChangeStream::kOperationTypeField);

    if (eventOpType == DocumentSourceChangeStream::kInsertOpType) {
        _documentsDelta += 1;
    } else if (eventOpType == DocumentSourceChangeStream::kDeleteOpType) {
        _documentsDelta -= 1;
    } else if (eventOpType == DocumentSourceChangeStream::kReshardDoneCatchUpOpType &&
               _isRecipient) {
        _receivedFinalEvent = true;
    } else if (eventOpType == DocumentSourceChangeStream::kReshardBlockingWritesOpType &&
               !_isRecipient) {
        _receivedFinalEvent = true;
    } else {
        LOGV2(9981902,
              "Unrecognized event while processing change stream event for resharding validation",
              "Event"_attr = changeEvent.toString());
    }

    _numEventsProcessed += 1;
}

}  // namespace mongo
