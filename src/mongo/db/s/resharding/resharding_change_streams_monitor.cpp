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
}  // namespace

ReshardingChangeStreamsMonitor::ReshardingChangeStreamsMonitor(
    const NamespaceString monitorNamespace,
    Timestamp startAtOperationTime,
    bool isRecipient,
    BatchProcessedCallback callback)
    : _monitorNS(monitorNamespace),
      _startAt(startAtOperationTime),
      _isRecipient(isRecipient),
      _batchProcessedCallback(callback) {}

ReshardingChangeStreamsMonitor::ReshardingChangeStreamsMonitor(
    const NamespaceString monitorNamespace,
    BSONObj startAfterToken,
    bool isRecipient,
    BatchProcessedCallback callback)
    : _monitorNS(monitorNamespace),
      _startAfter(startAfterToken),
      _isRecipient(isRecipient),
      _batchProcessedCallback(callback) {}


void ReshardingChangeStreamsMonitor::startMonitoring(
    std::shared_ptr<executor::TaskExecutor> executor, CancelableOperationContextFactory factory) {
    if (_finalEventPromise) {
        LOGV2(9981900, "The resharding change streams monitor has already completed.");
        return;
    }

    _finalEventPromise = std::make_unique<SharedPromise<void>>();
    ExecutorFuture<void>(executor)
        .then([this, executor, factory]() {
            auto opCtx = factory.makeOperationContext(&cc());
            auto aggRequest = _makeAggregateCommandRequest();
            _consumeChangeEvents(opCtx.get(), aggRequest);
        })
        .onCompletion([this](Status status) {
            if (!status.isOK()) {
                LOGV2_ERROR(9981901,
                            "The resharding change streams monitor failed",
                            "reason"_attr = status);
                _finalEventPromise->setError(status);
            }

            if (_recievedFinalEvent) {
                _finalEventPromise->emplaceValue();
            }
        })
        .getAsync([](auto) {});
}

SharedSemiFuture<void> ReshardingChangeStreamsMonitor::awaitFinalChangeEvent() {
    tassert(1009073,
            "Attempted to wait for the resharding change streams monitor to complete without "
            "starting it",
            _finalEventPromise);

    // Block until the final change event is observed.
    return _finalEventPromise->getFuture();
}

AggregateCommandRequest ReshardingChangeStreamsMonitor::_makeAggregateCommandRequest() {
    tassert(1009071,
            str::stream() << "Expected the change streams monitor to have 'startAt' or "
                             "'startAfter' but it has neither of them",
            _startAt || _startAfter);
    tassert(1009072,
            str::stream() << "Expected the change streams monitor to have 'startAt' or "
                             "'startAfter' but it has both of them",
            !_startAt || !_startAfter);

    DocumentSourceChangeStreamSpec changeStreamSpec;
    changeStreamSpec.setAllowToRunOnSystemNS(_monitorNS.isSystem());
    // The events against the temporary resharding collection are only ouput when
    // 'showMigrationEvents' is true.
    changeStreamSpec.setShowMigrationEvents(true);
    // The 'reshardBlockingWrites' event, which is the final event when monitoring on a donor, is
    // only output when "showSystemEvents" is true.
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
    return {_monitorNS, std::move(rawPipeline)};
}

void ReshardingChangeStreamsMonitor::_consumeChangeEvents(
    OperationContext* opCtx, const AggregateCommandRequest& aggRequest) {
    DBDirectClient client(opCtx);
    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        &client, aggRequest, false /* secondaryOk */, false /* useExhaust*/));

    while (cursor->more()) {
        auto doc = cursor->next();
        _processChangeEvent(doc);

        if (_recievedFinalEvent ||
            _numEventsProcessed %
                    resharding::gReshardingVerificationChangeStreamsEventsBatchSize.load() ==
                0) {
            const auto resumeToken = doc.getObjectField("_id");
            _batchProcessedCallback(_documentDelta, resumeToken);
            _documentDelta = 0;

            if (MONGO_unlikely(
                    failReshardingChangeStreamsMonitorAfterProcessingBatch.shouldFail())) {
                uasserted(ErrorCodes::InternalError, "Failing for failpoint");
            }

            if (_recievedFinalEvent) {
                return;
            }
        }

        hangReshardingChangeStreamsMonitorBeforeRecievingNextBatch.pauseWhileSet();
    }
}

void ReshardingChangeStreamsMonitor::_processChangeEvent(const BSONObj changeEvent) {
    const StringData eventOpType =
        changeEvent.getStringField(DocumentSourceChangeStream::kOperationTypeField);

    if (eventOpType == DocumentSourceChangeStream::kInsertOpType) {
        _documentDelta += 1;
    } else if (eventOpType == DocumentSourceChangeStream::kDeleteOpType) {
        _documentDelta -= 1;
    } else if (eventOpType == DocumentSourceChangeStream::kReshardDoneCatchUpOpType &&
               _isRecipient) {
        _recievedFinalEvent = true;
    } else if (eventOpType == DocumentSourceChangeStream::kReshardBlockingWritesOpType &&
               !_isRecipient) {
        _recievedFinalEvent = true;
    } else {
        LOGV2(9981902,
              "Unrecognized event while processing change stream event for resharding validation",
              "Event"_attr = changeEvent.toString());
    }

    _numEventsProcessed += 1;
}

}  // namespace mongo
