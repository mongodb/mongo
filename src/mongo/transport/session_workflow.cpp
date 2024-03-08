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


#include <array>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <fmt/format.h>
#include <memory>
#include <ratio>
#include <string>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/kill_cursors_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/executor/split_timer.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/ingress_handshake_metrics.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/session_workflow.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

namespace mongo::transport {
namespace {

MONGO_FAIL_POINT_DEFINE(doNotSetMoreToCome);
MONGO_FAIL_POINT_DEFINE(beforeCompressingExhaustResponse);
MONGO_FAIL_POINT_DEFINE(sessionWorkflowDelayOrFailSendMessage);

namespace metrics_detail {

/** Applies X(id) for each SplitId */
#define EXPAND_TIME_SPLIT_IDS(X) \
    X(started)                   \
    X(yieldedBeforeReceive)      \
    X(receivedWork)              \
    X(processedWork)             \
    X(sentResponse)              \
    X(yieldedAfterSend)          \
    X(done)                      \
    /**/

/**
 * Applies X(id, startSplit, endSplit) for each IntervalId.
 *
 * This table defines the intervals of a per-command `SessionWorkflow` loop
 * iteration as reported to a `SplitTimer`. The splits are time points, and the
 * `intervals` are durations between notable pairs of them.
 *
 *  [started]
 *  |   [yieldedBeforeReceive]
 *  |   |   [receivedWork]
 *  |   |   |   [processedWork]
 *  |   |   |   |   [sentResponse]
 *  |   |   |   |   |   [yieldedAfterSend]
 *  |   |   |   |   |   |   [done]
 *  |<--------------------->| total
 *  |<->|   |   |   |   |   | yieldBeforeReceive
 *  |   |<->|   |   |   |   | receiveWork
 *  |   |   |<------------->| active
 *  |   |   |<->|   |   |   | processWork
 *  |   |   |   |<->|   |   | sendResponse
 *  |   |   |   |   |<->|   | yieldAfterSend
 *  |   |   |   |   |   |<->| finalize
 */
#define EXPAND_INTERVAL_IDS(X)                           \
    X(total, started, done)                              \
    X(yieldBeforeReceive, started, yieldedBeforeReceive) \
    X(receiveWork, yieldedBeforeReceive, receivedWork)   \
    X(active, receivedWork, done)                        \
    X(processWork, receivedWork, processedWork)          \
    X(sendResponse, processedWork, sentResponse)         \
    X(finalize, yieldedAfterSend, done)                  \
    /**/

#define X_ID(id, ...) id,
enum class IntervalId : size_t { EXPAND_INTERVAL_IDS(X_ID) };
enum class TimeSplitId : size_t { EXPAND_TIME_SPLIT_IDS(X_ID) };
#undef X_ID

/** Trait for the count of the elements in a packed enum. */
template <typename T>
static constexpr size_t enumExtent = 0;

#define X_COUNT(...) +1
template <>
constexpr inline size_t enumExtent<IntervalId> = EXPAND_INTERVAL_IDS(X_COUNT);
template <>
constexpr inline size_t enumExtent<TimeSplitId> = EXPAND_TIME_SPLIT_IDS(X_COUNT);
#undef X_COUNT

struct TimeSplitDef {
    TimeSplitId id;
    StringData name;
};

struct IntervalDef {
    IntervalId id;
    StringData name;
    TimeSplitId start;
    TimeSplitId end;
};

constexpr inline auto timeSplitDefs = std::array{
#define X(id) TimeSplitDef{TimeSplitId::id, #id ""_sd},
    EXPAND_TIME_SPLIT_IDS(X)
#undef X
};

constexpr inline auto intervalDefs = std::array{
#define X(id, start, end) \
    IntervalDef{IntervalId::id, #id "Millis"_sd, TimeSplitId::start, TimeSplitId::end},
    EXPAND_INTERVAL_IDS(X)
#undef X
};

#undef EXPAND_TIME_SPLIT_IDS
#undef EXPAND_INTERVAL_IDS

struct SplitTimerPolicy {
    using TimeSplitIdType = TimeSplitId;
    using IntervalIdType = IntervalId;

    static constexpr size_t numTimeSplitIds = enumExtent<TimeSplitIdType>;
    static constexpr size_t numIntervalIds = enumExtent<IntervalIdType>;

    explicit SplitTimerPolicy(ServiceEntryPoint* sep) : _sep(sep) {}

    template <typename E>
    static constexpr size_t toIdx(E e) {
        return static_cast<size_t>(e);
    }

    static constexpr StringData getName(IntervalIdType iId) {
        return intervalDefs[toIdx(iId)].name;
    }

    static constexpr TimeSplitIdType getStartSplit(IntervalIdType iId) {
        return intervalDefs[toIdx(iId)].start;
    }

    static constexpr TimeSplitIdType getEndSplit(IntervalIdType iId) {
        return intervalDefs[toIdx(iId)].end;
    }

    static constexpr StringData getName(TimeSplitIdType tsId) {
        return timeSplitDefs[toIdx(tsId)].name;
    }

    void onStart(SplitTimer<SplitTimerPolicy>* splitTimer) {
        splitTimer->notify(TimeSplitIdType::started);
    }

    void onFinish(SplitTimer<SplitTimerPolicy>* splitTimer) {
        splitTimer->notify(TimeSplitIdType::done);
        auto t = splitTimer->getSplitInterval(IntervalIdType::sendResponse);
        if (MONGO_likely(!t || *t < Milliseconds{serverGlobalParams.slowMS.load()}))
            return;
        BSONObjBuilder bob;
        splitTimer->appendIntervals(bob);

        if (!gEnableDetailedConnectionHealthMetricLogLines.load()) {
            return;
        }

        logv2::LogSeverity severity = sessionWorkflowDelayOrFailSendMessage.shouldFail()
            ? logv2::LogSeverity::Info()
            : _sep->slowSessionWorkflowLogSeverity();

        LOGV2_DEBUG(6983000,
                    severity.toInt(),
                    "Slow network response send time",
                    "elapsed"_attr = bob.obj());
    }

    Timer makeTimer() {
        return Timer{};
    }

    ServiceEntryPoint* _sep;
};

class SessionWorkflowMetrics {
public:
    explicit SessionWorkflowMetrics(ServiceEntryPoint* sep) : _sep(sep) {}

    void start() {
        _t.emplace(SplitTimerPolicy{_sep});
    }
    void yieldedBeforeReceive() {
        _t->notify(TimeSplitId::yieldedBeforeReceive);
    }
    void received() {
        _t->notify(TimeSplitId::receivedWork);
    }
    void processed() {
        _t->notify(TimeSplitId::processedWork);
    }
    void sent(Session& session) {
        _t->notify(TimeSplitId::sentResponse);
        IngressHandshakeMetrics::get(session).onResponseSent(
            duration_cast<Milliseconds>(*_t->getSplitInterval(IntervalId::processWork)),
            duration_cast<Milliseconds>(*_t->getSplitInterval(IntervalId::sendResponse)));
    }
    void yieldedAfterSend() {
        _t->notify(TimeSplitId::yieldedAfterSend);
    }
    void finish() {
        _t.reset();
    }

private:
    ServiceEntryPoint* _sep;
    boost::optional<SplitTimer<SplitTimerPolicy>> _t;
};

// TODO(SERVER-63883): Remove when re-introducing real metrics.
class NoopSessionWorkflowMetrics {
public:
    explicit NoopSessionWorkflowMetrics() {}
    void start() {}
    void yieldedBeforeReceive() {}
    void received() {}
    void processed() {}
    void sent(Session&) {}
    void yieldedAfterSend() {}
    void finish() {}
};

using Metrics = NoopSessionWorkflowMetrics;
}  // namespace metrics_detail

/**
 * Given a request and its already generated response, checks for exhaust flags. If exhaust is
 * allowed, produces the subsequent request message, and modifies the response message to indicate
 * it is part of an exhaust stream. Returns the subsequent request message, which is known as a
 * 'synthetic' exhaust request. Returns an empty optional if exhaust is not allowed.
 */
boost::optional<Message> makeExhaustMessage(Message requestMsg, DbResponse& response) {
    if (!OpMsgRequest::isFlagSet(requestMsg, OpMsg::kExhaustSupported) ||
        !response.shouldRunAgainForExhaust)
        return {};

    const bool checksumPresent = OpMsg::isFlagSet(requestMsg, OpMsg::kChecksumPresent);
    Message exhaustMessage;

    if (auto nextInvocation = response.nextInvocation) {
        // The command provided a new BSONObj for the next invocation.
        OpMsgBuilder builder;
        builder.setBody(*nextInvocation);
        exhaustMessage = builder.finish();
    } else {
        // Reuse the previous invocation for the next invocation.
        OpMsg::removeChecksum(&requestMsg);
        exhaustMessage = requestMsg;
    }

    // The id of the response is used as the request id of this 'synthetic' request. Re-checksum
    // if needed.
    exhaustMessage.header().setId(response.response.header().getId());
    exhaustMessage.header().setResponseToMsgId(response.response.header().getResponseToMsgId());
    OpMsg::setFlag(&exhaustMessage, OpMsg::kExhaustSupported);
    if (checksumPresent) {
        OpMsg::appendChecksum(&exhaustMessage);
    }

    OpMsg::removeChecksum(&response.response);
    // Indicate that the response is part of an exhaust stream (unless the 'doNotSetMoreToCome'
    // failpoint is set). Re-checksum if needed.
    if (!MONGO_unlikely(doNotSetMoreToCome.shouldFail())) {
        OpMsg::setFlag(&response.response, OpMsg::kMoreToCome);
    }
    if (checksumPresent) {
        OpMsg::appendChecksum(&response.response);
    }

    return exhaustMessage;
}

/**
 * If `in` encodes a "getMore" command, make a best-effort attempt to kill its
 * cursor. Returns true if such an attempt was successful. If the killCursors request
 * fails here for any reasons, it will still be cleaned up once the cursor times
 * out.
 */
bool killExhaust(const Message& in, ServiceEntryPoint* sep, Client* client) {
    try {
        auto inRequest = OpMsgRequest::parse(in, client);
        const BSONObj& body = inRequest.body;
        const auto& [cmd, firstElement] = body.firstElement();
        if (cmd != "getMore"_sd)
            return false;
        auto opCtx = client->makeOperationContext();
        sep->handleRequest(opCtx.get(),
                           OpMsgRequestBuilder::create(
                               auth::ValidatedTenancyScope::get(opCtx.get()),
                               inRequest.getDbName(),
                               KillCursorsCommandRequest(
                                   NamespaceStringUtil::deserialize(inRequest.getDbName(),
                                                                    body["collection"].String()),
                                   {CursorId{firstElement.Long()}})
                                   .toBSON(BSONObj{}))
                               .serialize())
            .get();
        return true;
    } catch (const DBException& e) {
        LOGV2(22992, "Error cleaning up resources for exhaust request", "error"_attr = e);
    }
    return false;
}

// Counts the # of responses to completed operations that we were unable to send back to the client.
auto& unsendableCompletedResponses =
    *MetricBuilder<Counter64>("operation.unsendableCompletedResponses");
}  // namespace

class SessionWorkflow::Impl {
public:
    class WorkItem;

    Impl(SessionWorkflow* workflow, ServiceContext::UniqueClient client)
        : _workflow{workflow},
          _serviceContext{client->getServiceContext()},
          _sep{client->getService()->getServiceEntryPoint()},
          _clientStrand{ClientStrand::make(std::move(client))} {}

    Client* client() const {
        return _clientStrand->getClientPointer();
    }

    void start() {
        _scheduleIteration();
    }

    /*
     * Terminates the associated transport Session, regardless of tags.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminate();

    /*
     * Terminates the associated transport Session if the connection tags in the client don't match
     * the supplied tags.  If the connection tags indicate a pending state, before any tags have
     * been set, it will not be terminated.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminateIfTagsDontMatch(Client::TagMask tags);

    const std::shared_ptr<Session>& session() const {
        return client()->session();
    }

    ServiceExecutor* executor() {
        return seCtx()->getServiceExecutor();
    }

    std::shared_ptr<ServiceExecutor::TaskRunner> taskRunner() {
        auto exec = executor();
        // Allows switching the executor between iterations of the workflow.
        if (MONGO_unlikely(!_taskRunner.source || _taskRunner.source != exec))
            _taskRunner = {exec->makeTaskRunner(), exec};
        return _taskRunner.runner;
    }

    bool isTLS() const {
#ifdef MONGO_CONFIG_SSL
        return SSLPeerInfo::forSession(session()).isTLS();
#else
        return false;
#endif
    }

    ServiceExecutorContext* seCtx() {
        return ServiceExecutorContext::get(client());
    }

private:
    struct RunnerAndSource {
        std::shared_ptr<ServiceExecutor::TaskRunner> runner;
        ServiceExecutor* source = nullptr;
    };

    struct IterationFrame {
        explicit IterationFrame(const Impl& impl) : metrics{} {
            metrics.start();
        }
        ~IterationFrame() {
            metrics.finish();
        }

        metrics_detail::Metrics metrics;
    };

    /** Alias: refers to this Impl, but holds a ref to the enclosing workflow. */
    std::shared_ptr<Impl> shared_from_this() {
        return {_workflow->shared_from_this(), this};
    }

    /**
     * Returns a callback that's just like `cb`, but runs under the `_clientStrand`.
     * The wrapper binds a `shared_from_this` so `cb` doesn't need its own copy
     * of that anchoring shared pointer.
     */
    unique_function<void(Status)> _captureContext(unique_function<void(Status)> cb) {
        return [this, a = shared_from_this(), cb = std::move(cb)](Status st) mutable {
            _clientStrand->run([&] { cb(st); });
        };
    }

    void _scheduleIteration();

    Future<void> _doOneIteration();

    /** Returns a Future for the next WorkItem. */
    Future<std::unique_ptr<WorkItem>> _getNextWork() {
        invariant(!_work);
        if (_nextWork)
            return Future{std::move(_nextWork)};  // Already have one ready.

        // Yield here to avoid pinning the CPU. Give other threads some CPU
        // time to avoid a spiky latency distribution (BF-27452). Even if
        // this client can run continuously and receive another command
        // without blocking, we yield anyway. We WANT context switching, and
        // we're trying deliberately to make it happen, to reduce long tail
        // latency.
        _yieldPointReached();
        _iterationFrame->metrics.yieldedBeforeReceive();
        return _receiveRequest();
    }

    /** Receives a message from the session and creates a new WorkItem from it. */
    std::unique_ptr<WorkItem> _receiveRequest();

    /** Sends work to the ServiceEntryPoint, obtaining a future for its completion. */
    Future<DbResponse> _dispatchWork();

    /** Handles the completed response from dispatched work. */
    void _acceptResponse(DbResponse response);

    /** Writes the completed work response to the Session. */
    void _sendResponse();

    void _onLoopError(Status error);

    void _cleanupSession(const Status& status);

    /*
     * Releases all the resources associated with the exhaust request.
     * When the session is closing, the most recently synthesized exhaust
     * `WorkItem` may refer to a cursor that we won't need anymore, so we can
     * try to kill it early as an optimization.
     */
    void _cleanupExhaustResources();

    /**
     * Notify the task runner that this would be a good time to yield. It might
     * not actually yield, depending on implementation and on overall system
     * state.
     *
     * Yielding at certain points in a command's processing pipeline has been
     * considered to be beneficial to performance.
     */
    void _yieldPointReached() {
        executor()->yieldIfAppropriate();
    }

    SessionWorkflow* const _workflow;
    ServiceContext* const _serviceContext;
    ServiceEntryPoint* _sep;
    RunnerAndSource _taskRunner;

    AtomicWord<bool> _isTerminated{false};
    ClientStrandPtr _clientStrand;

    std::unique_ptr<WorkItem> _work;
    std::unique_ptr<WorkItem> _nextWork; /**< created by exhaust responses */
    boost::optional<IterationFrame> _iterationFrame;
};

class SessionWorkflow::Impl::WorkItem {
public:
    WorkItem(Impl* swf, Message in) : _swf{swf}, _in{std::move(in)} {}

    bool isExhaust() const {
        return _isExhaust;
    }

    void initOperation() {
        auto newOpCtx = _swf->client()->makeOperationContext();
        if (_isExhaust)
            newOpCtx->markKillOnClientDisconnect();
        if (_in.operation() == dbCompressed)
            newOpCtx->setOpCompressed(true);
        _opCtx = std::move(newOpCtx);
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

    const Message& in() const {
        return _in;
    }

    void decompressRequest() {
        if (_in.operation() != dbCompressed)
            return;
        MessageCompressorId cid;
        _in = uassertStatusOK(compressorMgr().decompressMessage(_in, &cid));
        _compressorId = cid;
    }

    Message compressResponse(Message msg) {
        if (!_compressorId)
            return msg;
        auto cid = *_compressorId;
        return uassertStatusOK(compressorMgr().compressMessage(msg, &cid));
    }

    bool hasCompressorId() const {
        return !!_compressorId;
    }

    Message consumeOut() {
        return std::move(*std::exchange(_out, {}));
    }

    bool hasOut() const {
        return !!_out;
    }

    void setOut(Message out) {
        _out = std::move(out);
    }

    /**
     * If the incoming message has the exhaust flag set, then we bypass the normal RPC
     * behavior. We will sink the response to the network, but we also synthesize a new
     * request, as if we sourced a new message from the network. This new request is
     * sent to the database once again to be processed. This cycle repeats as long as
     * the command indicates the exhaust stream should continue.
     */
    std::unique_ptr<WorkItem> synthesizeExhaust(DbResponse& response) {
        auto m = makeExhaustMessage(_in, response);
        if (!m)
            return nullptr;
        auto synth = std::make_unique<WorkItem>(_swf, std::move(*m));
        synth->_isExhaust = true;
        synth->_compressorId = _compressorId;
        return synth;
    }

private:
    MessageCompressorManager& compressorMgr() const {
        return MessageCompressorManager::forSession(_swf->session());
    }

    Impl* _swf;
    Message _in;
    bool _isExhaust = false;
    ServiceContext::UniqueOperationContext _opCtx;
    boost::optional<MessageCompressorId> _compressorId;
    boost::optional<Message> _out;
};

std::unique_ptr<SessionWorkflow::Impl::WorkItem> SessionWorkflow::Impl::_receiveRequest() {
    try {
        auto msg = uassertStatusOK([&] {
            MONGO_IDLE_THREAD_BLOCK;
            return session()->sourceMessage();
        }());
        invariant(!msg.empty());
        return std::make_unique<WorkItem>(this, std::move(msg));
    } catch (const DBException& ex) {
        auto remote = session()->remote();
        const auto& status = ex.toStatus();
        if (ErrorCodes::isInterruption(status.code()) ||
            ErrorCodes::isNetworkError(status.code())) {
            LOGV2_DEBUG(22986,
                        2,
                        "Session from remote encountered a network error during SourceMessage",
                        "remote"_attr = remote,
                        "error"_attr = status);
        } else if (status == TransportLayer::TicketSessionClosedStatus) {
            // Our session may have been closed internally.
            LOGV2_DEBUG(22987,
                        2,
                        "Session from {remote} was closed internally during SourceMessage",
                        "remote"_attr = remote);
        } else {
            LOGV2(22988,
                  "Error receiving request from client. Ending connection from remote",
                  "error"_attr = status,
                  "remote"_attr = remote,
                  "connectionId"_attr = session()->id());
        }
        throw;
    }
}

void SessionWorkflow::Impl::_sendResponse() {
    if (!_work->hasOut())
        return;

    try {
        sessionWorkflowDelayOrFailSendMessage.execute([this](auto&& data) {
            if (auto isDelay = data["millis"]; !isDelay.eoo()) {
                Milliseconds delay{isDelay.safeNumberLong()};
                LOGV2(6724101, "sendMessage: failpoint-induced delay", "delay"_attr = delay);
                sleepFor(delay);
            } else {
                auto md = ClientMetadata::get(client());
                if (md && (md->getApplicationName() == data["appName"].str())) {
                    LOGV2(4920200, "sendMessage: failpoint-induced failure");
                    uasserted(ErrorCodes::HostUnreachable, "Sink message failed");
                }
            }
        });
        uassertStatusOK(session()->sinkMessage(_work->consumeOut()));
    } catch (const DBException& ex) {
        LOGV2(22989,
              "Error sending response to client. Ending connection from remote",
              "error"_attr = ex,
              "remote"_attr = session()->remote(),
              "connectionId"_attr = session()->id());
        unsendableCompletedResponses.increment();
        throw;
    }
}

Future<DbResponse> SessionWorkflow::Impl::_dispatchWork() {
    invariant(_work);
    invariant(!_work->in().empty());

    TrafficRecorder::get(_serviceContext)
        .observe(session(), _serviceContext->getPreciseClockSource()->now(), _work->in());

    _work->decompressRequest();

    networkCounter.hitLogicalIn(_work->in().size());

    // Pass sourced Message to handler to generate response.
    _work->initOperation();

    return _sep->handleRequest(_work->opCtx(), _work->in());
}

void SessionWorkflow::Impl::_acceptResponse(DbResponse response) {
    auto&& work = *_work;
    // opCtx must be delisted here so that the operation cannot show up in currentOp results after
    // the response reaches the client. We are assuming that the operation has already been killed
    // once we are accepting the response here, so delisting is sufficient. Destruction of the
    // already killed opCtx is postponed for later (i.e., after completion of the future-chain) to
    // mitigate its performance impact on the critical path of execution.
    // Note that destroying futures after execution, rather that postponing the destruction
    // until completion of the future-chain, would expose the cost of destroying opCtx to
    // the critical path and result in serious performance implications.
    _serviceContext->delistOperation(work.opCtx());
    // Format our response, if we have one
    Message& toSink = response.response;
    if (toSink.empty())
        return;

    tassert(ErrorCodes::InternalError,
            "Attempted to respond to fire-and-forget request",
            !OpMsg::isFlagSet(work.in(), OpMsg::kMoreToCome));
    invariant(!OpMsg::isFlagSet(toSink, OpMsg::kChecksumPresent));

    // Update the header for the response message.
    toSink.header().setId(nextMessageId());
    toSink.header().setResponseToMsgId(work.in().header().getId());
    if (!isTLS() && OpMsg::isFlagSet(work.in(), OpMsg::kChecksumPresent))
        OpMsg::appendChecksum(&toSink);

    // If the incoming message has the exhaust flag set, then bypass the normal RPC
    // behavior. Sink the response to the network, but also synthesize a new
    // request, as if a new message was sourced from the network. This new request is
    // sent to the database once again to be processed. This cycle repeats as long as
    // the dbresponses continue to indicate the exhaust stream should continue.
    _nextWork = work.synthesizeExhaust(response);

    networkCounter.hitLogicalOut(toSink.size());

    beforeCompressingExhaustResponse.executeIf(
        [&](auto&&) {}, [&](auto&&) { return work.hasCompressorId() && _nextWork; });

    toSink = work.compressResponse(toSink);

    TrafficRecorder::get(_serviceContext)
        .observe(session(), _serviceContext->getPreciseClockSource()->now(), toSink);

    work.setOut(std::move(toSink));
}

void SessionWorkflow::Impl::_onLoopError(Status error) {
    LOGV2_DEBUG(5763901, 2, "Terminating session due to error", "error"_attr = error);
    terminate();
    _cleanupSession(error);
}

/** Returns a Future representing the completion of one loop iteration. */
Future<void> SessionWorkflow::Impl::_doOneIteration() {
    _iterationFrame.emplace(*this);
    return _getNextWork()
        .then([&](auto work) {
            _iterationFrame->metrics.received();
            invariant(!_work);
            _work = std::move(work);
            return _dispatchWork();
        })
        .then([&](auto rsp) {
            _acceptResponse(std::move(rsp));
            _iterationFrame->metrics.processed();
            _sendResponse();
            _iterationFrame->metrics.sent(*session());
            _yieldPointReached();
            _iterationFrame->metrics.yieldedAfterSend();
            _iterationFrame.reset();
        });
}

void SessionWorkflow::Impl::_scheduleIteration() try {
    _work = nullptr;
    taskRunner()->schedule(_captureContext([&](Status status) {
        if (MONGO_unlikely(!status.isOK())) {
            _cleanupSession(status);
            return;
        }

        try {
            // All available service executors use dedicated threads, so it's okay to
            // run eager futures in an ordinary loop to bypass scheduler overhead.
            while (true) {
                _doOneIteration().get();
                _work = nullptr;
            }
        } catch (const DBException& ex) {
            _onLoopError(ex.toStatus());
        }
    }));
} catch (const DBException& ex) {
    auto error = ex.toStatus();
    LOGV2_WARNING_OPTIONS(22993,
                          {logv2::LogComponent::kExecutor},
                          "Unable to schedule a new loop for the session workflow",
                          "error"_attr = error);
    _onLoopError(error);
}

void SessionWorkflow::Impl::terminate() {
    if (_isTerminated.swap(true))
        return;

    session()->end();
}

void SessionWorkflow::Impl::terminateIfTagsDontMatch(Client::TagMask tags) {
    if (_isTerminated.load())
        return;

    auto clientTags = client()->getTags();

    // If terminateIfTagsDontMatch gets called when we still are 'pending' where no tags have been
    // set, then skip the termination check.
    if ((clientTags & tags) || (clientTags & Client::kPending)) {
        LOGV2(
            22991, "Skip closing connection for connection", "connectionId"_attr = session()->id());
        return;
    }

    terminate();
}

void SessionWorkflow::Impl::_cleanupExhaustResources() {
    auto clean = [&](auto& w) {
        return w && w->isExhaust() && killExhaust(w->in(), _sep, client());
    };
    clean(_nextWork) || clean(_work);
}

void SessionWorkflow::Impl::_cleanupSession(const Status& status) {
    LOGV2_DEBUG(5127900, 2, "Ending session", "error"_attr = status);
    if (_work && _work->opCtx()) {
        // Make sure we clean up and delist the operation in the case we error between creating
        // the opCtx and getting a response back for the work item. This is required in the case
        // that we need to create a new opCtx to kill existing exhaust resources.
        _serviceContext->killAndDelistOperation(_work->opCtx(),
                                                ErrorCodes::OperationIsKilledAndDelisted);
    }
    _cleanupExhaustResources();
    _taskRunner = {};
    client()->session()->getTransportLayer()->getSessionManager()->endSessionByClient(client());
}

SessionWorkflow::SessionWorkflow(PassKeyTag, ServiceContext::UniqueClient client)
    : _impl{std::make_unique<Impl>(this, std::move(client))} {}

SessionWorkflow::~SessionWorkflow() = default;

Client* SessionWorkflow::client() const {
    return _impl->client();
}

void SessionWorkflow::start() {
    _impl->start();
}

void SessionWorkflow::terminate() {
    _impl->terminate();
}

void SessionWorkflow::terminateIfTagsDontMatch(Client::TagMask tags) {
    _impl->terminateIfTagsDontMatch(tags);
}

}  // namespace mongo::transport
