/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/repl/oplog_fetcher.h"

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/counter.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/resharding/resume_token_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <algorithm>
#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(stopReplProducer);
MONGO_FAIL_POINT_DEFINE(stopReplProducerOnDocument);
MONGO_FAIL_POINT_DEFINE(setSmallOplogGetMoreMaxTimeMS);
MONGO_FAIL_POINT_DEFINE(hangAfterOplogFetcherCallbackScheduled);
MONGO_FAIL_POINT_DEFINE(hangBeforeStartingOplogFetcher);
MONGO_FAIL_POINT_DEFINE(hangBeforeOplogFetcherRetries);
MONGO_FAIL_POINT_DEFINE(hangBeforeProcessingSuccessfulBatch);
MONGO_FAIL_POINT_DEFINE(hangOplogFetcherBeforeAdvancingLastFetched);

namespace {
class OplogBatchStats {
public:
    void recordMillis(int millis, bool isEmptyBatch);
    BSONObj getReport() const;
    operator BSONObj() const {
        return getReport();
    }

private:
    TimerStats _getMores;
    Counter64 _numEmptyBatches;
};

void OplogBatchStats::recordMillis(int millis, bool isEmptyBatch) {
    _getMores.recordMillis(millis);
    if (isEmptyBatch) {
        _numEmptyBatches.increment();
    }
}

BSONObj OplogBatchStats::getReport() const {
    BSONObjBuilder b(_getMores.getReport());
    b.append("numEmptyBatches", _numEmptyBatches.get());
    return b.obj();
}

// The number and time spent reading batches off the network
auto& oplogBatchStats = *MetricBuilder<OplogBatchStats>("repl.network.getmores");
// The oplog entries read via the oplog reader
auto& opsReadStats = *MetricBuilder<Counter64>{"repl.network.ops"};
// The bytes read via the oplog reader
auto& networkByteStats = *MetricBuilder<Counter64>{"repl.network.bytes"};

auto& readersCreatedStats = *MetricBuilder<Counter64>{"repl.network.readersCreated"};

const Milliseconds maximumAwaitDataTimeoutMS = Seconds{30};

/**
 * Calculates await data timeout based on the current replica set configuration.
 */
Milliseconds calculateAwaitDataTimeout(const ReplSetConfig& config) {
    if (MONGO_unlikely(setSmallOplogGetMoreMaxTimeMS.shouldFail())) {
        return Milliseconds(50);
    }

    // Under protocol version 1, make the awaitData timeout (maxTimeMS) dependent on the election
    // timeout. This enables the sync source to communicate liveness of the primary to secondaries.
    // We never wait longer than 30 seconds.
    return std::min((config.getElectionTimeoutPeriod() / 2), maximumAwaitDataTimeoutMS);
}
}  // namespace


StatusWith<OplogFetcher::DocumentsInfo> OplogFetcher::validateDocuments(
    const OplogFetcher::Documents& documents,
    bool first,
    Timestamp lastTS,
    StartingPoint startingPoint) {
    if (first && documents.empty()) {
        return Status(ErrorCodes::OplogStartMissing,
                      str::stream() << "The first batch of oplog entries is empty, but expected at "
                                       "least 1 document matching ts: "
                                    << lastTS.toString());
    }
    DocumentsInfo info;
    // The count of the bytes of the documents read off the network.
    info.networkDocumentBytes = 0;
    info.networkDocumentCount = 0;
    for (auto&& doc : documents) {
        if (feature_flags::gReduceMajorityWriteLatency.isEnabled()) {
            // Check for oplog version change.
            auto version = doc[OplogEntry::kVersionFieldName].numberLong();
            if (version != OplogEntry::kOplogVersion) {
                static constexpr char message[] = "Unexpected oplog version";
                LOGV2_FATAL_CONTINUE(8539101,
                                     message,
                                     "expectedVersion"_attr = OplogEntry::kOplogVersion,
                                     "foundVersion"_attr = version,
                                     "oplogEntry"_attr = redact(doc));
                return Status(ErrorCodes::BadValue,
                              str::stream() << message << ", expected oplog version "
                                            << OplogEntry::kOplogVersion << ", found version "
                                            << version << ", oplog entry: " << redact(doc));
            }
        }

        info.networkDocumentBytes += doc.objsize();
        ++info.networkDocumentCount;

        // If this is the first response (to the $gte query) then we already applied the first doc.
        if (first && info.networkDocumentCount == 1U) {
            continue;
        }

        auto docOpTime = OpTime::parseFromOplogEntry(doc);
        if (!docOpTime.isOK()) {
            return docOpTime.getStatus();
        }
        info.lastDocument = docOpTime.getValue();

        // Check to see if the oplog entry goes back in time for this document.
        const auto docTS = info.lastDocument.getTimestamp();
        if (lastTS >= docTS) {
            return Status(ErrorCodes::OplogOutOfOrder,
                          str::stream() << "Out of order entries in oplog. lastTS: "
                                        << lastTS.toString() << " outOfOrderTS:" << docTS.toString()
                                        << " in batch with " << info.networkDocumentCount
                                        << "docs; first-batch:" << first << ", doc:" << doc);
        }
        lastTS = docTS;
    }

    // These numbers are for the documents we will apply.
    info.toApplyDocumentCount = documents.size();
    info.toApplyDocumentBytes = info.networkDocumentBytes;
    if (first && startingPoint == StartingPoint::kSkipFirstDoc) {
        // The count is one less since the first document found was already applied ($gte $ts query)
        // and we will not apply it again.
        --info.toApplyDocumentCount;
        auto alreadyAppliedDocument = documents.cbegin();
        info.toApplyDocumentBytes -= alreadyAppliedDocument->objsize();
    }

    return info;
}

OplogFetcher::OplogFetcher(executor::TaskExecutor* executor,
                           std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
                           DataReplicatorExternalState* dataReplicatorExternalState,
                           EnqueueDocumentsFn enqueueDocumentsFn,
                           OnShutdownCallbackFn onShutdownCallbackFn,
                           Config config)
    : AbstractAsyncComponent(executor, config.name),
      _oplogFetcherRestartDecision(std::move(oplogFetcherRestartDecision)),
      _onShutdownCallbackFn(onShutdownCallbackFn),
      _lastFetched(config.initialLastFetched),
      _createClientFn(
          [] { return std::make_unique<DBClientConnection>(true /* autoReconnect */); }),
      _dataReplicatorExternalState(dataReplicatorExternalState),
      _enqueueDocumentsFn(enqueueDocumentsFn),
      _awaitDataTimeout(calculateAwaitDataTimeout(config.replSetConfig)),
      _config(std::move(config)) {
    invariant(_config.replSetConfig.isInitialized());
    invariant(!_lastFetched.isNull());
    invariant(onShutdownCallbackFn);
    invariant(enqueueDocumentsFn);
}

OplogFetcher::~OplogFetcher() {
    shutdown();
    join();
}

void OplogFetcher::setConnection(std::unique_ptr<DBClientConnection>&& _connectedClient) {
    // Can only call this once, before startup.
    invariant(!_conn);
    _conn = std::move(_connectedClient);
}

void OplogFetcher::_doStartup(WithLock lk) {
    uassertStatusOK(_scheduleWorkAndSaveHandle(
        lk,
        [this](const executor::TaskExecutor::CallbackArgs& args) {
            // Tests use this failpoint to prevent the oplog fetcher from starting.  If those
            // tests fail and the oplog fetcher is canceled, we want to continue so we see
            // a test failure quickly instead of a test timeout eventually.
            while (hangBeforeStartingOplogFetcher.shouldFail() && !args.myHandle.isCanceled()) {
                sleepmillis(100);
            }
            _runQuery(args);
        },
        &_runQueryHandle,
        "_runQuery"));
}

void OplogFetcher::_doShutdown(WithLock lk) noexcept {
    _cancelHandle(lk, _runQueryHandle);

    if (_conn) {
        _conn->shutdownAndDisallowReconnect();
    }
    _shutdownCondVar.notify_all();
}

stdx::mutex* OplogFetcher::_getMutex() noexcept {
    return &_mutex;
}

std::string OplogFetcher::toString() {
    stdx::lock_guard lock(_mutex);
    str::stream output;
    output << "OplogFetcher -";
    output << " last optime fetched: " << _lastFetched.toString();
    output << " source: " << _config.source.toString();
    output << " namespace: " << toStringForLogging(_nss);
    output << " active: " << _isActive(lock);
    output << " shutting down?:" << _isShuttingDown(lock);
    output << " first batch: " << _firstBatch;
    output << " initial find timeout: " << _getInitialFindMaxTime();
    output << " retried find timeout: " << _getRetriedFindMaxTime();
    output << " awaitData timeout: " << _awaitDataTimeout;
    return output;
}

OplogFetcher::StartingPoint OplogFetcher::getStartingPoint_forTest() const {
    return _config.startingPoint;
}

FindCommandRequest OplogFetcher::makeFindCmdRequest_forTest(long long findTimeout) const {
    return _makeFindCmdRequest(findTimeout);
}

Milliseconds OplogFetcher::getAwaitDataTimeout_forTest() const {
    return _awaitDataTimeout;
}

void OplogFetcher::setCreateClientFn_forTest(const CreateClientFn& createClientFn) {
    stdx::lock_guard lock(_mutex);
    _createClientFn = createClientFn;
}

DBClientConnection* OplogFetcher::getDBClientConnection_forTest() const {
    stdx::lock_guard lock(_mutex);
    return _conn.get();
}

Milliseconds OplogFetcher::getInitialFindMaxTime_forTest() const {
    return _getInitialFindMaxTime();
}

Milliseconds OplogFetcher::getRetriedFindMaxTime_forTest() const {
    return _getRetriedFindMaxTime();
}

void OplogFetcher::_setSocketTimeout(long long timeout) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_conn);
    // setSoTimeout takes a double representing the number of seconds for send and receive
    // timeouts. Thus, we must express the timeout in milliseconds and divide by 1000.0 to get
    // the number of seconds with a fractional part.
    _conn->setSoTimeout(timeout / 1000.0 + oplogNetworkTimeoutBufferSeconds.load());
}

OpTime OplogFetcher::getLastOpTimeFetched() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _lastFetched;
}

Milliseconds OplogFetcher::_getInitialFindMaxTime() const {
    return Milliseconds(oplogInitialFindMaxSeconds.load() * 1000);
}

Milliseconds OplogFetcher::_getRetriedFindMaxTime() const {
    return Milliseconds(oplogRetriedFindMaxSeconds.load() * 1000);
}

void OplogFetcher::_finishCallback(Status status) {
    invariant(isActive());
    // If the oplog fetcher is shutting down, consolidate return code to CallbackCanceled.
    if (_isShuttingDown() && status != ErrorCodes::CallbackCanceled) {
        status = Status(ErrorCodes::CallbackCanceled,
                        str::stream() << "Got error: \"" << status.toString()
                                      << "\" while oplog fetcher is shutting down");
    }
    _onShutdownCallbackFn(status);

    decltype(_onShutdownCallbackFn) onShutdownCallbackFn;
    decltype(_oplogFetcherRestartDecision) oplogFetcherRestartDecision;
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _transitionToComplete(lock);

    // Release any resources that might be held by the '_onShutdownCallbackFn' function object.
    // The function object will be destroyed outside the lock since the temporary variable
    // 'onShutdownCallbackFn' is declared before 'lock'.
    invariant(_onShutdownCallbackFn);
    std::swap(_onShutdownCallbackFn, onShutdownCallbackFn);

    // Release any resources held by the OplogFetcherRestartDecision.
    invariant(_oplogFetcherRestartDecision);
    std::swap(_oplogFetcherRestartDecision, oplogFetcherRestartDecision);
}

void OplogFetcher::_runQuery(const executor::TaskExecutor::CallbackArgs& callbackData) noexcept {
    Status responseStatus =
        _checkForShutdownAndConvertStatus(callbackData, "error running oplog fetcher");
    if (!responseStatus.isOK()) {
        _finishCallback(responseStatus);
        return;
    }

    bool hadExistingConnection = true;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (!_conn) {
            _conn = _createClientFn();
            hadExistingConnection = false;
        }
    }

    hangAfterOplogFetcherCallbackScheduled.pauseWhileSet();

    if (!hadExistingConnection) {
        auto connectStatus = _connect();

        // Error out if we failed to connect after exhausting the allowed retry attempts.
        if (!connectStatus.isOK()) {
            _finishCallback(connectStatus);
            return;
        }
    }

    _setMetadataWriterAndReader();
    auto cursorStatus = _createNewCursor(true /* initialFind */);
    while (true) {
        Status status{Status::OK()};
        {
            // Both of these checks need to happen while holding the mutex since they could race
            // with shutdown.
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            if (_isShuttingDown(lock)) {
                status = {ErrorCodes::CallbackCanceled, "oplog fetcher shutting down"};
            } else if (_runQueryHandle.isCanceled()) {
                invariant(_getExecutor()->isShuttingDown());
                status = {ErrorCodes::CallbackCanceled, "oplog fetcher task executor shutdown"};
            }
        }
        if (!status.isOK()) {
            _finishCallback(status);
            return;
        }

        auto batchResult = _getNextBatch();
        if (!batchResult.isOK()) {
            auto brStatus = batchResult.getStatus();
            // Determine if we should stop syncing from our current sync source. If we're going
            // to change sync sources anyway, do it immediately rather than checking if we can
            // retry the error.
            const bool stopFetching = _dataReplicatorExternalState->shouldStopFetchingOnError(
                                          _config.source, getLastOpTimeFetched()) !=
                ChangeSyncSourceAction::kContinueSyncing;

            // Recreate a cursor if we have enough retries left.
            if (!stopFetching && _oplogFetcherRestartDecision->shouldContinue(this, brStatus)) {
                hangBeforeOplogFetcherRetries.pauseWhileSet();
                _cursor.reset();
                continue;
            } else {
                _finishCallback(brStatus);
                return;
            }
        }

        // This will advance our view of _lastFetched.
        status = _onSuccessfulBatch(batchResult.getValue());
        if (!status.isOK()) {
            // The stopReplProducer fail point expects this to return successfully. If another fail
            // point wants this to return unsuccessfully, it should use a different error code.
            if (status == ErrorCodes::FailPointEnabled) {
                _finishCallback(Status::OK());
                return;
            }

            _finishCallback(status);
            return;
        }

        if (_cursor->isDead() || _cursor->wasError()) {
            if (!_cursor->tailable()) {
                try {
                    auto opCtx = cc().makeOperationContext();
                    stdx::unique_lock<stdx::mutex> lk(_mutex);
                    // Wait a little before re-running the aggregation command on the donor's
                    // oplog. We are not actually intending to wait for shutdown here, we use
                    // this as a way to wait while still being able to be interrupted outside of
                    // primary-only service shutdown.
                    opCtx->waitForConditionOrInterruptFor(_shutdownCondVar,
                                                          lk,
                                                          _awaitDataTimeout,
                                                          [&] { return _isShuttingDown(lk); });
                    _cursor.reset();
                    continue;
                } catch (const DBException& e) {
                    _finishCallback(e.toStatus().withContext(
                        "Interrupted while waiting to create a new aggregation cursor"));
                    return;
                }
            } else {
                // This means the sync source closes the tailable cursor with a returned cursorId of
                // 0. Any users of the oplog fetcher should create a new oplog fetcher if they see a
                // successful status and would like to continue fetching more oplog entries.
                _finishCallback(Status::OK());
                return;
            }
        }
    }
}

Status OplogFetcher::_connect() {
    Status connectStatus = Status::OK();
    do {
        if (_isShuttingDown()) {
            return Status(ErrorCodes::CallbackCanceled, "oplog fetcher shutting down");
        }
        connectStatus = [&] {
            try {
                if (!connectStatus.isOK()) {
                    // If this is a retry, let the DBClientConnection handle the reconnect itself
                    // for proper backoff behavior.
                    LOGV2(23437,
                          "OplogFetcher reconnecting due to error",
                          "error"_attr = connectStatus);
                    _conn->ensureConnection();
                } else {
                    _conn->connect(_config.source, "OplogFetcher", boost::none);
                }
                uassertStatusOK(replAuthenticate(_conn.get())
                                    .withContext(str::stream()
                                                 << "OplogFetcher failed to authenticate to "
                                                 << _config.source));
                // Reset any state needed to track restarts on successful connection.
                _oplogFetcherRestartDecision->fetchSuccessful(this);
                return Status::OK();
            } catch (const DBException& e) {
                hangBeforeOplogFetcherRetries.pauseWhileSet();
                return e.toStatus();
            }
        }();
    } while (!connectStatus.isOK() &&
             _dataReplicatorExternalState->shouldStopFetchingOnError(_config.source,
                                                                     getLastOpTimeFetched()) ==
                 ChangeSyncSourceAction::kContinueSyncing &&
             _oplogFetcherRestartDecision->shouldContinue(this, connectStatus));

    return connectStatus;
}

void OplogFetcher::_setMetadataWriterAndReader() {
    invariant(_conn);

    _vectorClockMetadataHook =
        std::make_unique<rpc::VectorClockMetadataHook>(getGlobalServiceContext());

    _conn->setRequestMetadataWriter([this](OperationContext* opCtx, BSONObjBuilder* metadataBob) {
        *metadataBob << rpc::kReplSetMetadataFieldName << 1;
        *metadataBob << rpc::kOplogQueryMetadataFieldName << 1;
        metadataBob->appendElements(ReadPreferenceSetting::secondaryPreferredMetadata());

        // Run VectorClockMetadataHook on request metadata so this matches the behavior of
        // the connections in the replication coordinator thread pool.
        return _vectorClockMetadataHook->writeRequestMetadata(opCtx, metadataBob);
    });

    _conn->setReplyMetadataReader(
        [this](OperationContext* opCtx, const BSONObj& metadataObj, StringData source) {
            _metadataObj = metadataObj.getOwned();

            // Run VectorClockMetadataHook on reply metadata so this matches the behavior of the
            // connections in the replication coordinator thread pool.
            return _vectorClockMetadataHook->readReplyMetadata(opCtx, _metadataObj);
        });
}

FindCommandRequest OplogFetcher::_makeFindCmdRequest(long long findTimeout) const {
    FindCommandRequest findCmd{_nss};

    // Construct the find command's filter and set it on the 'FindCommandRequest'.
    {
        BSONObjBuilder queryBob;

        auto lastOpTimeFetched = getLastOpTimeFetched();
        BSONObjBuilder filterBob;
        filterBob.append("ts", BSON("$gte" << lastOpTimeFetched.getTimestamp()));
        // Handle caller-provided filter.
        if (!_config.queryFilter.isEmpty()) {
            filterBob.append(
                "$or",
                BSON_ARRAY(_config.queryFilter << BSON("ts" << lastOpTimeFetched.getTimestamp())));
        }
        findCmd.setFilter(filterBob.obj());
    }

    findCmd.setTailable(true);
    findCmd.setAwaitData(true);
    findCmd.setMaxTimeMS(findTimeout);

    if (_config.batchSize) {
        findCmd.setBatchSize(_config.batchSize);
    }

    if (_config.requestResumeToken) {
        findCmd.setHint(BSON("$natural" << 1));
        findCmd.setRequestResumeToken(true);
    }

    auto lastCommittedWithCurrentTerm =
        _dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime();
    auto term = lastCommittedWithCurrentTerm.value;
    if (term != OpTime::kUninitializedTerm) {
        findCmd.setTerm(term);
    }

    if (_config.queryReadConcern.isEmpty()) {
        // This ensures that the sync source waits for all earlier oplog writes to be visible.
        // Since Timestamp(0, 0) isn't allowed, Timestamp(0, 1) is the minimal we can use.
        findCmd.setReadConcern(repl::ReadConcernArgs(LogicalTime(Timestamp(0, 1)),
                                                     repl::ReadConcernLevel::kLocalReadConcern));
    } else {
        // Caller-provided read concern.
        findCmd.setReadConcern(_config.queryReadConcern);
    }
    return findCmd;
}

Status OplogFetcher::_createNewCursor(bool initialFind) {
    invariant(_conn);

    // Set the socket timeout to the 'find' timeout plus a network buffer.
    auto maxTimeMs = durationCount<Milliseconds>(initialFind ? _getInitialFindMaxTime()
                                                             : _getRetriedFindMaxTime());
    _setSocketTimeout(maxTimeMs);

    auto findCmd = _makeFindCmdRequest(maxTimeMs);
    _cursor = std::make_unique<DBClientCursor>(
        _conn.get(), std::move(findCmd), ReadPreferenceSetting{}, oplogFetcherUsesExhaust);

    _firstBatch = true;

    readersCreatedStats.increment();
    return Status::OK();
}

StatusWith<OplogFetcher::Documents> OplogFetcher::_getNextBatch() {
    Documents batch;
    try {
        Timer timer;
        if (!_cursor) {
            // An error occurred and we should recreate the cursor.
            // When recreating the cursor, use the longer initial max time
            // to avoid timing out.
            auto status = _createNewCursor(false);
            if (!status.isOK()) {
                return status;
            }
        }
        // If it is the first batch, we should initialize the cursor, which will run the find query.
        // Otherwise we should call more() to get the next batch.
        if (_firstBatch) {
            // Network errors manifest as exceptions that are handled in the catch block. If init
            // returns false it means that the sync source responded with nothing, which could
            // indicate a problem with the sync source.
            // We can't call `init()` when using an aggregate command because
            // `DBClientCursor::fromAggregationRequest` will already have processed the `cursorId`.
            if (!_cursor->init()) {
                _cursor.reset();
                return {ErrorCodes::InvalidSyncSource,
                        str::stream() << "Oplog fetcher could not create cursor on source: "
                                      << _config.source};
            }

            // This will also set maxTimeMS on the generated getMore command.
            _cursor->setAwaitDataTimeoutMS(_awaitDataTimeout);

            // The 'find' command has already been executed, so reset the socket timeout to reflect
            // the awaitData timeout with a network buffer.
            _setSocketTimeout(durationCount<Milliseconds>(_awaitDataTimeout));

            // TODO SERVER-46240: Handle batchSize 1 in DBClientCursor.
            // Due to a bug in DBClientCursor, it actually uses batchSize 2 if the given batchSize
            // is 1 for the find command. So if the given batchSize is 1, we need to set it
            // explicitly for getMores.
            if (_config.batchSize == 1) {
                _cursor->setBatchSize(_config.batchSize);
            }
        } else {
            auto lastCommittedWithCurrentTerm =
                _dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime();
            if (lastCommittedWithCurrentTerm.value != OpTime::kUninitializedTerm) {
                if (!_cursor->isExhaust() && lastCommittedWithCurrentTerm.opTime.isNull()) {
                    // For non-exhaust cursors, only set the lastKnownCommittedOpTime when it is not
                    // a null opTime. This is to avoid sending null opTime again and again and
                    // triggering oplog empty batches every single time in case we can't advance our
                    // commit point (e.g. during initial sync).
                    _cursor->setCurrentTermAndLastCommittedOpTime(
                        lastCommittedWithCurrentTerm.value, boost::none);
                } else {
                    // For exhaust cursors, it is safe to set a null lastKnownCommittedOpTime in the
                    // initial getMore because the sync source will update the exhaust cursor's
                    // lastKnownCommittedOpTime to the commit point sent in the last response after
                    // each oplog batch.
                    _cursor->setCurrentTermAndLastCommittedOpTime(
                        lastCommittedWithCurrentTerm.value, lastCommittedWithCurrentTerm.opTime);
                }
            }
            _cursor->more();
        }
        while (_cursor->moreInCurrentBatch()) {
            batch.emplace_back(_cursor->nextSafe());
        }

        // This value is only used on a successful batch for metrics.repl.network.getmores. This
        // metric intentionally tracks the time taken by the initial find as well.
        _lastBatchElapsedMS = timer.millis();
    } catch (const DBException& ex) {
        if (_cursor->connectionHasPendingReplies()) {
            // Close the connection because the connection cannot be used anymore as more data is on
            // the way from the server for the exhaust stream. Thus, we have to reconnect. The
            // DBClientConnection does autoreconnect on the next network operation.
            _conn->shutdown();
        }
        return ex.toStatus("Error while getting the next batch in the oplog fetcher");
    }

    return batch;
}

Status OplogFetcher::_onSuccessfulBatch(const Documents& documents) {
    hangBeforeProcessingSuccessfulBatch.pauseWhileSet();

    if (_isShuttingDown()) {
        return Status(ErrorCodes::CallbackCanceled, "oplog fetcher shutting down");
    }
    // We call fetchSuccessful after any batch succeeds and it will reset _numRestarts to 0. Note
    // that if the first batch is succesful and the second batch due to an error that is not handled
    // in shouldContinue(), it will create an indefinite loop. The loop occurs because the restart
    // after the second batch will recreate the cursor and repeat the successful first batch and
    // reset _numRestarts, never letting it increment enough to exceed maxRestarts. This can be
    // resolved by adding the error to shouldContinue() and returning false in that case so that it
    // doesn't infinitely loop.
    _oplogFetcherRestartDecision->fetchSuccessful(this);

    // Stop fetching and return on fail point.
    // This fail point makes the oplog fetcher ignore the downloaded batch of operations and not
    // error out. The FailPointEnabled error will be caught by the caller.
    if (MONGO_unlikely(stopReplProducer.shouldFail())) {
        return Status(ErrorCodes::FailPointEnabled, "stopReplProducer fail point is enabled");
    }

    // Stop fetching and return when we reach a particular document in a batch.
    // Consider using the parameter setParameter bgSyncOplogFetcherBatchSize=1 to fetch documents
    // one at a time.
    {
        Status status = Status::OK();
        stopReplProducerOnDocument.executeIf(
            [&](auto&&) {
                static constexpr char message[] =
                    "stopReplProducerOnDocument fail point is enabled";
                LOGV2(21269, message);
                status = {ErrorCodes::FailPointEnabled, message};
            },
            [&](const BSONObj& data) {
                auto opCtx = cc().makeOperationContext();
                auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(_nss).build();
                Matcher m(data["document"].Obj(), expCtx);
                for (const auto& doc : documents) {
                    const auto& obj = doc["o"].Obj();
                    std::vector<BSONObj> objToBeMatched;
                    // We batch inserts into a single applyOps oplog entry with an internal array of
                    // operations as inserts, and set the 'multiOpType' flag.
                    if (doc.hasField("multiOpType") &&
                        doc["multiOpType"].numberInt() ==
                            int(repl::MultiOplogEntryType::kApplyOpsAppliedSeparately)) {
                        auto applyOpsElement = obj.getField("applyOps");
                        for (const auto& element : applyOpsElement.Array()) {
                            objToBeMatched.push_back(element.Obj()["o"].Obj());
                        }
                    } else {
                        objToBeMatched.push_back(obj);
                    }

                    for (const auto& obj : objToBeMatched) {
                        if (exec::matcher::matches(&m, obj)) {
                            LOGV2(9918500,
                                  "stopReplProducerOnDocument matched a document.",
                                  "doc"_attr = doc,
                                  "batchSize"_attr = documents.size(),
                                  "firstTimestamp"_attr = documents.front()["ts"],
                                  "lastTimestamp"_attr = documents.back()["ts"]);
                            return true;
                        }
                    }
                }
                return false;
            });
        if (!status.isOK()) {
            return status;
        }
    }

    auto firstDocToApply = documents.cbegin();

    if (!documents.empty()) {
        LOGV2_DEBUG(21270,
                    2,
                    "Oplog fetcher read batch from remote oplog",
                    "batchSize"_attr = documents.size(),
                    "firstTimestamp"_attr = documents.front()["ts"],
                    "lastTimestamp"_attr = documents.back()["ts"]);
    } else {
        LOGV2_DEBUG(21271, 2, "Oplog fetcher read 0 operations from remote oplog");
    }

    auto oqMetadataResult = rpc::OplogQueryMetadata::readFromMetadata(_metadataObj);
    if (!oqMetadataResult.isOK()) {
        LOGV2_ERROR(21278,
                    "Invalid oplog query metadata from sync source",
                    "syncSource"_attr = _config.source,
                    "error"_attr = oqMetadataResult.getStatus(),
                    "metadata"_attr = _metadataObj);
        return oqMetadataResult.getStatus();
    }
    const auto& oqMetadata = oqMetadataResult.getValue();

    if (_firstBatch) {
        auto status = _checkRemoteOplogStart(documents, oqMetadata.getLastOpApplied());
        if (!status.isOK()) {
            // Stop oplog fetcher and execute rollback if necessary.
            return status;
        }

        LOGV2_DEBUG(21272,
                    1,
                    "Oplog fetcher successfully fetched from sync source",
                    "syncSource"_attr = _config.source);

        // We do not always enqueue the first document. We elect to skip it for the following
        // reasons:
        //    1. This is the first batch and no rollback is needed. Callers specify
        //       StartingPoint::kSkipFirstDoc when they want this behavior.
        //    2. We have already enqueued that document in a previous attempt. We can get into
        //       this situation if we had a batch with StartingPoint::kEnqueueFirstDoc that failed
        //       right after that first document was enqueued. In such a scenario, we would not
        //       have advanced the lastFetched opTime, so we skip past that document to avoid
        //       duplicating it.
        //    3. We have a query filter, and the first document doesn't match that filter.  This
        //       happens on the first batch when we always accept a document with the previous
        //       fetched timestamp.

        if (_config.startingPoint == StartingPoint::kSkipFirstDoc) {
            firstDocToApply++;
        } else if (!_config.queryFilter.isEmpty()) {
            auto opCtx = cc().makeOperationContext();
            auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx.get()).ns(_nss).build();
            Matcher m(_config.queryFilter, expCtx);
            if (!exec::matcher::matches(&m, *firstDocToApply))
                firstDocToApply++;
        }
    }

    // This lastFetched value is the last OpTime from the previous batch.
    auto previousOpTimeFetched = getLastOpTimeFetched();

    auto validateResult = OplogFetcher::validateDocuments(
        documents, _firstBatch, previousOpTimeFetched.getTimestamp(), _config.startingPoint);
    if (!validateResult.isOK()) {
        return validateResult.getStatus();
    }
    auto info = validateResult.getValue();
    // If the batch is empty, set 'lastDocOpTime' to the lastFetched from the previous batch.
    auto lastDocOpTime = info.lastDocument.isNull() ? previousOpTimeFetched : info.lastDocument;

    // Process replset metadata.  It is important that this happen after we've validated the
    // first batch, so we don't progress our knowledge of the commit point from a
    // response that triggers a rollback.
    auto metadataResult = rpc::ReplSetMetadata::readFromMetadata(_metadataObj);
    if (!metadataResult.isOK()) {
        LOGV2_ERROR(21279,
                    "Invalid replication metadata from sync source",
                    "syncSource"_attr = _config.source,
                    "error"_attr = metadataResult.getStatus(),
                    "metadata"_attr = _metadataObj);
        return metadataResult.getStatus();
    }
    const auto& replSetMetadata = metadataResult.getValue();

    // Determine if we should stop syncing from our current sync source.
    auto changeSyncSourceAction = _dataReplicatorExternalState->shouldStopFetching(
        _config.source, replSetMetadata, oqMetadata, previousOpTimeFetched, lastDocOpTime);

    auto getErrMsg = [&] {
        str::stream errMsg;
        errMsg << "sync source " << _config.source.toString();
        errMsg << " (config version: " << replSetMetadata.getConfigVersion();
        errMsg << "; last applied optime: " << oqMetadata.getLastOpApplied().toString();
        errMsg << "; sync source index: " << oqMetadata.getSyncSourceIndex();
        errMsg << "; has primary index: " << oqMetadata.hasPrimaryIndex();
        errMsg << ") is no longer valid";
        errMsg << " previous batch last fetched optime: " << previousOpTimeFetched.toString();
        errMsg << " current batch last fetched optime: " << lastDocOpTime.toString();
        return errMsg;
    };

    if (changeSyncSourceAction == ChangeSyncSourceAction::kStopSyncingAndDropLastBatchIfPresent) {
        return Status(ErrorCodes::InvalidSyncSource, getErrMsg());
    }

    _dataReplicatorExternalState->processMetadata(replSetMetadata, oqMetadata);

    // Increment stats. We read all of the docs in the query.
    opsReadStats.increment(info.networkDocumentCount);
    networkByteStats.increment(info.networkDocumentBytes);

    oplogBatchStats.recordMillis(_lastBatchElapsedMS, documents.empty());

    if (_cursor->getPostBatchResumeToken()) {
        auto pbrt =
            ResumeTokenOplogTimestamp::parse(*_cursor->getPostBatchResumeToken(),
                                             IDLParserContext("OplogFetcher PostBatchResumeToken"));
        info.resumeToken = pbrt.getTs();
    }

    try {
        auto status = _enqueueDocumentsFn(firstDocToApply, documents.cend(), info);
        if (!status.isOK()) {
            return status;
        }
    } catch (const DBException& e) {
        return e.toStatus().withContext("Error inserting documents into oplog buffer collection");
    }

    if (changeSyncSourceAction == ChangeSyncSourceAction::kStopSyncingAndEnqueueLastBatch) {
        return Status(ErrorCodes::InvalidSyncSource, getErrMsg());
    }

    if (MONGO_unlikely(hangOplogFetcherBeforeAdvancingLastFetched.shouldFail())) {
        hangOplogFetcherBeforeAdvancingLastFetched.pauseWhileSet();
    }

    // Start skipping the first doc after at least one doc has been enqueued in the lifetime
    // of this fetcher.
    _config.startingPoint = StartingPoint::kSkipFirstDoc;

    // We have now processed the batch. We should only move forward our view of _lastFetched if the
    // batch was not empty.
    if (lastDocOpTime != previousOpTimeFetched) {
        LOGV2_DEBUG(21273,
                    3,
                    "Oplog fetcher setting last fetched optime ahead after batch",
                    "lastDocOpTime"_attr = lastDocOpTime);

        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _lastFetched = lastDocOpTime;
    }

    _firstBatch = false;
    return Status::OK();
}

Status OplogFetcher::_checkRemoteOplogStart(const OplogFetcher::Documents& documents,
                                            OpTime remoteLastOpApplied) {
    // Sometimes our remoteLastOpApplied may be stale; if we received a document with an
    // opTime later than remoteLastApplied, we can assume the remote is at least up to that
    // opTime.
    if (!documents.empty()) {
        const auto docOpTime = OpTime::parseFromOplogEntry(documents.back());
        if (docOpTime.isOK()) {
            remoteLastOpApplied = std::max(remoteLastOpApplied, docOpTime.getValue());
        }
    }

    auto lastFetched = getLastOpTimeFetched();

    // The sync source could be behind us if it rolled back after we selected it. We could have
    // failed to detect the rollback if it occurred between sync source selection (when we check the
    // candidate is ahead of us) and sync source resolution. If the sync source is now behind us,
    // choose a new sync source to prevent going into rollback.
    if (remoteLastOpApplied < lastFetched) {
        return Status(
            ErrorCodes::InvalidSyncSource,
            fmt::format(
                "Sync source's last applied OpTime {} is older than our last fetched OpTime "
                "{}. Choosing new sync source.",
                remoteLastOpApplied.toString(),
                lastFetched.toString()));
    }

    // If '_requireFresherSyncSource' is true, we must check that the sync source's
    // lastApplied is ahead of us to prevent forming a cycle. Although we check for
    // this condition in sync source selection, if an undetected rollback occurred between sync
    // source selection and sync source resolution, this condition may no longer hold.
    // '_requireFresherSyncSource' is false for initial sync, since no other node can sync off an
    // initial syncing node, so we do not need to check for cycles. In addition, it would be
    // problematic to check this condition for initial sync, since the 'lastFetched' OpTime will
    // almost always equal the 'remoteLastApplied', since we fetch the sync source's last applied
    // OpTime to determine where to start our OplogFetcher.
    if (_config.requireFresherSyncSource == RequireFresherSyncSource::kRequireFresherSyncSource &&
        remoteLastOpApplied <= lastFetched) {
        return Status(
            ErrorCodes::InvalidSyncSource,
            fmt::format("Sync source must be ahead of me. My last fetched oplog optime: {}, latest "
                        "oplog optime of sync source: {}",
                        lastFetched.toString(),
                        remoteLastOpApplied.toString()));
    }

    // At this point we know that our sync source has our minValid and is not behind us, so if our
    // history diverges from our sync source's we should prefer its history and roll back ours.

    // Since we checked for rollback and our sync source is ahead of us, an empty batch means that
    // we have a higher timestamp on our last fetched OpTime than our sync source's last applied
    // OpTime, but a lower term. When this occurs, we must roll back our inconsistent oplog entry.
    if (documents.empty()) {
        return Status(ErrorCodes::OplogStartMissing, "Received an empty batch from sync source.");
    }

    const auto& o = documents.front();
    auto opTimeResult = OpTime::parseFromOplogEntry(o);

    if (!opTimeResult.isOK()) {
        return Status(
            ErrorCodes::InvalidBSON,
            fmt::format("our last optime fetched: {}. failed to parse optime from first oplog in "
                        "batch on source: {}: {}",
                        lastFetched.toString(),
                        o.toString(),
                        opTimeResult.getStatus().toString()));
    }
    auto opTime = opTimeResult.getValue();
    if (opTime != lastFetched) {
        Status status = _checkTooStaleToSyncFromSource(lastFetched, opTime);

        // We should never return an OK status here.
        invariant(!status.isOK());
        return status;
    }
    return Status::OK();
}

Status OplogFetcher::_checkTooStaleToSyncFromSource(const OpTime lastFetched,
                                                    const OpTime firstOpTimeInBatch) {
    // Check to see if the sync source's first oplog entry is later than 'lastFetched'. If it is, we
    // are too stale to sync from this node. If it isn't, we should go into rollback instead.
    BSONObj remoteFirstOplogEntry;
    try {
        // Query for the first oplog entry in the sync source's oplog.
        FindCommandRequest findRequest{_nss};
        findRequest.setSort(BSON("$natural" << 1));
        // Since this function is called after the first batch, the exhaust stream has not been
        // started yet. As a result, using the same connection is safe.
        remoteFirstOplogEntry = _conn->findOne(std::move(findRequest));
    } catch (DBException& e) {
        // If an error occurs with the query, throw an error.
        return Status(ErrorCodes::TooStaleToSyncFromSource, e.reason());
    }

    StatusWith<OpTime> remoteFirstOpTimeResult = OpTime::parseFromOplogEntry(remoteFirstOplogEntry);
    if (!remoteFirstOpTimeResult.isOK()) {
        return Status(
            ErrorCodes::InvalidBSON,
            fmt::format("failed to parse optime from first entry in source's oplog: {}: {}",
                        remoteFirstOplogEntry.toString(),
                        remoteFirstOpTimeResult.getStatus().toString()));
    }

    auto remoteFirstOpTime = remoteFirstOpTimeResult.getValue();
    if (remoteFirstOpTime.isNull()) {
        return Status(ErrorCodes::InvalidBSON,
                      fmt::format("optime of first entry in source's oplog cannot be null: {}",
                                  remoteFirstOplogEntry.toString()));
    }

    // remoteFirstOpTime may come from a very old config, so we cannot compare their terms.
    if (lastFetched.getTimestamp() < remoteFirstOpTime.getTimestamp()) {
        // We are too stale to sync from our current sync source.
        return Status(
            ErrorCodes::TooStaleToSyncFromSource,
            fmt::format("we are too stale to sync from the sync source's oplog. our last fetched "
                        "timestamp is earlier than the sync source's first timestamp. our last "
                        "optime fetched: {}. sync source's first optime: {}",
                        lastFetched.toString(),
                        remoteFirstOpTime.toString()));
    }

    // If we are not too stale to sync from the source, we should go into rollback.
    std::string message = fmt::format(
        "the sync source's oplog and our oplog have diverged, going into rollback. our last "
        "optime fetched: {}. optime of first document in batch: {}. sync source's first optime: {}",
        lastFetched.toString(),
        firstOpTimeInBatch.toString(),
        remoteFirstOpTime.toString());
    return Status(ErrorCodes::OplogStartMissing, message);
}

bool OplogFetcher::OplogFetcherRestartDecisionDefault::shouldContinue(OplogFetcher* fetcher,
                                                                      Status status) {
    // If we try to sync from a node that is shutting down, do not attempt to reconnect.
    // We should choose a new sync source.
    if (status.code() == ErrorCodes::ShutdownInProgress) {
        LOGV2(4696202,
              "Not recreating cursor for oplog fetcher because sync source is shutting down",
              "error"_attr = redact(status));
        return false;
    }
    // If the oplog batch fetched exceeds the size limit, do not attempt to restart.
    else if (status.code() == ErrorCodes::BSONObjectTooLarge) {
        LOGV2_FATAL_CONTINUE(
            10033601,
            "BSON document and metadata fetched by oplog fetcher exceeds the BSON limit",
            "error"_attr = redact(status));
        fassert(9995200, status);
    }
    if (_numRestarts == _maxRestarts) {
        LOGV2(21274,
              "Error returned from oplog query (no more query restarts left)",
              "error"_attr = redact(status));
        return false;
    }
    LOGV2(21275,
          "Recreating cursor for oplog fetcher due to error",
          "lastOpTimeFetched"_attr = fetcher->getLastOpTimeFetched(),
          "attemptsRemaining"_attr = (_maxRestarts - _numRestarts),
          "error"_attr = redact(status));
    // Exponential delay between retry attempts, starting at 5ms and increasing exponentially by a
    // power of 2 with an upper bound of 500ms. The delayTime calculation will exceed 500 and be
    // unused if _numRestarts >= 7, so we only calculate this variable if _numRestarts < 7 to
    // prevent overflow on std::pow in the case that the user defines a very high _maxRestarts.
    int delayTime = 500;
    if (_numRestarts < 7) {
        delayTime = 5 * (std::pow(2, _numRestarts));
    };
    sleepmillis(std::min(delayTime, 500));
    _numRestarts++;
    return true;
}

void OplogFetcher::OplogFetcherRestartDecisionDefault::fetchSuccessful(OplogFetcher* fetcher) {
    _numRestarts = 0;
}

OplogFetcher::OplogFetcherRestartDecision::~OplogFetcherRestartDecision() {};

}  // namespace repl
}  // namespace mongo
