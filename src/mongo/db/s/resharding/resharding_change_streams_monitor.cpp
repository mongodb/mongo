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
        LOGV2(9981900, "The resharding change streams monitor has already been scheduled.");
        return;
    }

    _finalEventPromise = std::make_unique<SharedPromise<void>>();
    ExecutorFuture<void>(executor)
        .then([this, executor, factory]() {
            auto opCtx = factory.makeOperationContext(&cc());
            auto aggReq = _createChangeStreamAggregation(opCtx.get());

            _consumeChangeStream(opCtx.get(), aggReq);
        })
        .onCompletion([this](Status status) {
            if (!status.isOK()) {
                LOGV2_ERROR(9981901,
                            "The resharding change streams monitor failed",
                            "reason"_attr = status);
                _finalEventPromise->setError(status);
                return;
            }

            _finalEventPromise->emplaceValue();
        })
        .getAsync([](auto) {});
}

SharedSemiFuture<void> ReshardingChangeStreamsMonitor::awaitFinalChangeEvent() {
    invariant(_finalEventPromise,
              "Attempted to await completion before starting the resharding change streams.");

    // Block until the final change event is observed.
    return _finalEventPromise->getFuture();
}

AggregateCommandRequest ReshardingChangeStreamsMonitor::_createChangeStreamAggregation(
    OperationContext* opCtx) {
    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;

    auto mongoProcessInterface = MongoProcessInterface::create(opCtx);
    BSONArray operationTypesArray = _getOperationTypesToMonitor();

    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .mongoProcessInterface(std::move(mongoProcessInterface))
                      .ns(_monitorNS)
                      .build();

    BSONObjBuilder builder;
    builder.append(DocumentSourceChangeStreamSpec::kAllowToRunOnSystemNSFieldName, true);
    builder.append(DocumentSourceChangeStreamSpec::kShowMigrationEventsFieldName, true);

    invariant(_startAt || _startAfter);
    invariant(!_startAt || !_startAfter);
    if (_startAt) {
        builder.append(DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName, *_startAt);
    } else {
        builder.append(DocumentSourceChangeStreamSpec::kStartAfterFieldName, *_startAfter);
    }
    BSONObj changeStreamStage = BSON(DocumentSourceChangeStream::kStageName << builder.obj());

    BSONObj matchStage =
        BSON("$match" << BSON("operationType" << BSON("$in" << operationTypesArray)));

    const std::vector<BSONObj> rawPipeline = {changeStreamStage, matchStage};

    return AggregateCommandRequest(_monitorNS, rawPipeline);
}

void ReshardingChangeStreamsMonitor::_consumeChangeStream(OperationContext* opCtx,
                                                          AggregateCommandRequest aggRequest) {
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
                // writes without impacting the opCtx used to open the change streams cursor above.
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
            // TODO (SERVER-100668): Investigate if the sleep can be avoided.
            opCtx->sleepFor(Milliseconds(1));
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

BSONArray ReshardingChangeStreamsMonitor::_getOperationTypesToMonitor() {
    std::vector<StringData> operationTypes = {DocumentSourceChangeStream::kInsertOpType,
                                              DocumentSourceChangeStream::kDeleteOpType};
    if (_isRecipient) {
        operationTypes.push_back(DocumentSourceChangeStream::kReshardDoneCatchUpOpType);
    } else {
        operationTypes.push_back(DocumentSourceChangeStream::kReshardBlockingWritesOpType);
    }

    BSONArrayBuilder operationTypesArrayBuilder;
    for (const auto& opType : operationTypes) {
        operationTypesArrayBuilder.append(opType);
    }

    return operationTypesArrayBuilder.arr();
}

}  // namespace mongo
