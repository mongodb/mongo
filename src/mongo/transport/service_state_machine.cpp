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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/service_state_machine.h"

#include <memory>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/query/kill_cursors_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/traffic_recorder.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_peer_info.h"

namespace mongo {
namespace transport {
namespace {
MONGO_FAIL_POINT_DEFINE(doNotSetMoreToCome);
MONGO_FAIL_POINT_DEFINE(beforeCompressingExhaustResponse);
/**
 * Creates and returns a legacy exhaust message, if exhaust is allowed. The returned message is to
 * be used as the subsequent 'synthetic' exhaust request. Returns an empty message if exhaust is not
 * allowed. Any messages that do not have an opcode of OP_MSG are considered legacy.
 */
Message makeLegacyExhaustMessage(Message* m, const DbResponse& dbresponse) {
    // OP_QUERY responses are always of type OP_REPLY.
    invariant(dbresponse.response.operation() == opReply);

    if (!dbresponse.shouldRunAgainForExhaust) {
        return Message();
    }

    // Legacy find operations via the OP_QUERY/OP_GET_MORE network protocol never provide the next
    // invocation for exhaust.
    invariant(!dbresponse.nextInvocation);

    DbMessage dbmsg(*m);
    invariant(dbmsg.messageShouldHaveNs());
    const char* ns = dbmsg.getns();

    MsgData::View header = dbresponse.response.header();
    QueryResult::View qr = header.view2ptr();
    long long cursorid = qr.getCursorId();

    if (cursorid == 0) {
        return Message();
    }

    // Generate a message that will act as the subsequent 'synthetic' exhaust request.
    BufBuilder b(512);
    b.appendNum(static_cast<int>(0));          // size set later in setLen()
    b.appendNum(header.getId());               // message id
    b.appendNum(header.getResponseToMsgId());  // in response to
    b.appendNum(static_cast<int>(dbGetMore));  // opCode is OP_GET_MORE
    b.appendNum(static_cast<int>(0));          // Must be ZERO (reserved)
    b.appendStr(StringData(ns));               // Namespace
    b.appendNum(static_cast<int>(0));          // ntoreturn
    b.appendNum(cursorid);                     // cursor id from the OP_REPLY

    MsgData::View(b.buf()).setLen(b.len());

    return Message(b.release());
}

/**
 * Given a request and its already generated response, checks for exhaust flags. If exhaust is
 * allowed, produces the subsequent request message, and modifies the response message to indicate
 * it is part of an exhaust stream. Returns the subsequent request message, which is known as a
 * 'synthetic' exhaust request. Returns an empty message if exhaust is not allowed.
 */
Message makeExhaustMessage(Message requestMsg, DbResponse* dbresponse) {
    if (requestMsg.operation() == dbQuery || requestMsg.operation() == dbGetMore) {
        return makeLegacyExhaustMessage(&requestMsg, *dbresponse);
    }

    if (!OpMsgRequest::isFlagSet(requestMsg, OpMsg::kExhaustSupported)) {
        return Message();
    }

    if (!dbresponse->shouldRunAgainForExhaust) {
        return Message();
    }

    const bool checksumPresent = OpMsg::isFlagSet(requestMsg, OpMsg::kChecksumPresent);
    Message exhaustMessage;

    if (auto nextInvocation = dbresponse->nextInvocation) {
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
    exhaustMessage.header().setId(dbresponse->response.header().getId());
    exhaustMessage.header().setResponseToMsgId(dbresponse->response.header().getResponseToMsgId());
    OpMsg::setFlag(&exhaustMessage, OpMsg::kExhaustSupported);
    if (checksumPresent) {
        OpMsg::appendChecksum(&exhaustMessage);
    }

    OpMsg::removeChecksum(&dbresponse->response);
    // Indicate that the response is part of an exhaust stream (unless the 'doNotSetMoreToCome'
    // failpoint is set). Re-checksum if needed.
    if (!MONGO_unlikely(doNotSetMoreToCome.shouldFail())) {
        OpMsg::setFlag(&dbresponse->response, OpMsg::kMoreToCome);
    }
    if (checksumPresent) {
        OpMsg::appendChecksum(&dbresponse->response);
    }

    return exhaustMessage;
}
}  // namespace

class ServiceStateMachine::Impl final
    : public std::enable_shared_from_this<ServiceStateMachine::Impl> {
public:
    /*
     * Any state may transition to EndSession in case of an error, otherwise the valid state
     * transitions are:
     * Source -> SourceWait -> Process -> SinkWait -> Source (standard RPC)
     * Source -> SourceWait -> Process -> SinkWait -> Process -> SinkWait ... (exhaust)
     * Source -> SourceWait -> Process -> Source (fire-and-forget)
     */
    enum class State {
        Created,     // The session has been created, but no operations have been performed yet
        Source,      // Request a new Message from the network to handle
        SourceWait,  // Wait for the new Message to arrive from the network
        Process,     // Run the Message through the database
        SinkWait,    // Wait for the database result to be sent by the network
        EndSession,  // End the session - the ServiceStateMachine will be invalid after this
        Ended        // The session has ended. It is illegal to call any method besides
                     // state() if this is the current state.
    };

    Impl(ServiceContext::UniqueClient client)
        : _state{State::Created},
          _serviceContext{client->getServiceContext()},
          _sep{_serviceContext->getServiceEntryPoint()},
          _clientStrand{ClientStrand::make(std::move(client))} {}

    ~Impl() {
        _sep->onEndSession(session());
    }

    void start(ServiceExecutorContext seCtx);

    void setCleanupHook(std::function<void()> hook);

    /*
     * Terminates the associated transport Session, regardless of tags.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminate();

    /*
     * Terminates the associated transport Session if its tags don't match the supplied tags.  If
     * the session is in a pending state, before any tags have been set, it will not be terminated.
     *
     * This will not block on the session terminating cleaning itself up, it returns immediately.
     */
    void terminateIfTagsDontMatch(transport::Session::TagMask tags);

    /*
     * This function actually calls into the database and processes a request. It's broken out
     * into its own inline function for better readability.
     */
    Future<void> processMessage();

    /*
     * These get called by the TransportLayer when requested network I/O has completed.
     */
    void sourceCallback(Status status);
    void sinkCallback(Status status);

    /*
     * Source/Sink message from the TransportLayer.
     */
    Future<void> sourceMessage();
    Future<void> sinkMessage();

    /*
     * Releases all the resources associated with the session and call the cleanupHook.
     */
    void cleanupSession(const Status& status);

    /*
     * Schedules a new loop for this state machine on a service executor. The status argument
     * specifies whether the last execution of the loop, if any, was successful.
     */
    void scheduleNewLoop(Status status);

    /*
     * Starts a new loop by running an iteration for this state machine (e.g., source, process and
     * then sink).
     */
    void startNewLoop(const Status& execStatus);

    /*
     * Releases all the resources associated with the exhaust request.
     */
    void cleanupExhaustResources() noexcept;

    /*
     * Gets the current state of connection for testing/diagnostic purposes.
     */
    State state() const {
        return _state.load();
    }

    /*
     * Gets the transport::Session associated with this connection
     */
    const transport::SessionHandle& session() {
        return _clientStrand->getClientPointer()->session();
    }

    /*
     * Gets the transport::ServiceExecutor associated with this connection.
     */
    ServiceExecutor* executor() {
        return ServiceExecutorContext::get(_clientStrand->getClientPointer())->getServiceExecutor();
    }

private:
    AtomicWord<State> _state{State::Created};

    ServiceContext* const _serviceContext;
    ServiceEntryPoint* const _sep;

    ClientStrandPtr _clientStrand;
    std::function<void()> _cleanupHook;

    bool _inExhaust = false;
    boost::optional<MessageCompressorId> _compressorId;
    Message _inMessage;
    Message _outMessage;

    ServiceContext::UniqueOperationContext _opCtx;
};

Future<void> ServiceStateMachine::Impl::sourceMessage() {
    invariant(_inMessage.empty());
    invariant(_state.load() == State::Source);
    _state.store(State::SourceWait);

    // Reset the compressor only before sourcing a new message. This ensures the same compressor,
    // if any, is used for sinking exhaust messages. For moreToCome messages, this allows resetting
    // the compressor for each incoming (i.e., sourced) message, and using the latest compressor id
    // for compressing the sink message.
    _compressorId = boost::none;

    auto sourceMsgImpl = [&] {
        const auto& transportMode = executor()->transportMode();
        if (transportMode == transport::Mode::kSynchronous) {
            MONGO_IDLE_THREAD_BLOCK;
            return Future<Message>::makeReady(session()->sourceMessage());
        } else {
            invariant(transportMode == transport::Mode::kAsynchronous);
            return session()->asyncSourceMessage();
        }
    };

    return sourceMsgImpl().onCompletion([this](StatusWith<Message> msg) -> Future<void> {
        if (msg.isOK()) {
            _inMessage = std::move(msg.getValue());
            invariant(!_inMessage.empty());
        }
        sourceCallback(msg.getStatus());
        return Status::OK();
    });
}

Future<void> ServiceStateMachine::Impl::sinkMessage() {
    // Sink our response to the client
    invariant(_state.load() == State::Process);
    _state.store(State::SinkWait);
    auto toSink = std::exchange(_outMessage, {});

    auto sinkMsgImpl = [&] {
        const auto& transportMode = executor()->transportMode();
        if (transportMode == transport::Mode::kSynchronous) {
            // We don't consider ourselves idle while sending the reply since we are still doing
            // work on behalf of the client. Contrast that with sourceMessage() where we are waiting
            // for the client to send us more work to do.
            return Future<void>::makeReady(session()->sinkMessage(std::move(toSink)));
        } else {
            invariant(transportMode == transport::Mode::kAsynchronous);
            return session()->asyncSinkMessage(std::move(toSink));
        }
    };

    return sinkMsgImpl().onCompletion([this](Status status) {
        sinkCallback(std::move(status));
        return Status::OK();
    });
}

void ServiceStateMachine::Impl::sourceCallback(Status status) {
    invariant(state() == State::SourceWait);

    auto remote = session()->remote();

    if (status.isOK()) {
        _state.store(State::Process);

        // If the sourceMessage succeeded then we can move to on to process the message. We simply
        // return from here and the future chain in startNewLoop() will continue to the next state
        // normally.

        // If any other issues arise, close the session.
    } else if (ErrorCodes::isInterruption(status.code()) ||
               ErrorCodes::isNetworkError(status.code())) {
        LOGV2_DEBUG(
            22986,
            2,
            "Session from {remote} encountered a network error during SourceMessage: {error}",
            "Session from remote encountered a network error during SourceMessage",
            "remote"_attr = remote,
            "error"_attr = status);
        _state.store(State::EndSession);
    } else if (status == TransportLayer::TicketSessionClosedStatus) {
        // Our session may have been closed internally.
        LOGV2_DEBUG(22987,
                    2,
                    "Session from {remote} was closed internally during SourceMessage",
                    "remote"_attr = remote);
        _state.store(State::EndSession);
    } else {
        LOGV2(22988,
              "Error receiving request from client. Ending connection from remote",
              "error"_attr = status,
              "remote"_attr = remote,
              "connectionId"_attr = session()->id());
        _state.store(State::EndSession);
    }
    uassertStatusOK(status);
}

void ServiceStateMachine::Impl::sinkCallback(Status status) {
    invariant(state() == State::SinkWait);

    // If there was an error sinking the message to the client, then we should print an error and
    // end the session.
    //
    // Otherwise, update the current state depending on whether we're in exhaust or not and return
    // from this function to let startNewLoop() continue the future chaining of state transitions.
    if (!status.isOK()) {
        LOGV2(22989,
              "Error sending response to client. Ending connection from remote",
              "error"_attr = status,
              "remote"_attr = session()->remote(),
              "connectionId"_attr = session()->id());
        _state.store(State::EndSession);
        uassertStatusOK(status);
    } else if (_inExhaust) {
        _state.store(State::Process);
    } else {
        _state.store(State::Source);
    }
    // Performance testing showed a significant benefit from yielding here.
    // TODO SERVER-57531: Once we enable the use of a fixed-size thread pool
    // for handling client connection handshaking, we should only yield here if
    // we're on a dedicated thread.
    executor()->yieldIfAppropriate();
}

Future<void> ServiceStateMachine::Impl::processMessage() {
    invariant(!_inMessage.empty());

    TrafficRecorder::get(_serviceContext)
        .observe(session(), _serviceContext->getPreciseClockSource()->now(), _inMessage);

    auto& compressorMgr = MessageCompressorManager::forSession(session());

    // Setup compressor and acquire a compressor id when processing compressed messages. Exhaust
    // messages produced via `makeExhaustMessage(...)` are not compressed, so the body of this if
    // statement only runs for sourced compressed messages.
    if (_inMessage.operation() == dbCompressed) {
        MessageCompressorId compressorId;
        auto swm = compressorMgr.decompressMessage(_inMessage, &compressorId);
        uassertStatusOK(swm.getStatus());
        _inMessage = swm.getValue();
        _compressorId = compressorId;
    }

    networkCounter.hitLogicalIn(_inMessage.size());

    // Pass sourced Message to handler to generate response.
    _opCtx = Client::getCurrent()->makeOperationContext();
    if (_inExhaust) {
        _opCtx->markKillOnClientDisconnect();
    }

    // The handleRequest is implemented in a subclass for mongod/mongos and actually all the
    // database work for this request.
    return _sep->handleRequest(_opCtx.get(), _inMessage)
        .then([this, &compressorMgr = compressorMgr](DbResponse dbresponse) mutable -> void {
            // opCtx must be killed and delisted here so that the operation cannot show up in
            // currentOp results after the response reaches the client. Destruction of the already
            // killed opCtx is postponed for later (i.e., after completion of the future-chain) to
            // mitigate its performance impact on the critical path of execution.
            // Note that destroying futures after execution, rather that postponing the destruction
            // until completion of the future-chain, would expose the cost of destroying opCtx to
            // the critical path and result in serious performance implications.
            _serviceContext->killAndDelistOperation(_opCtx.get(),
                                                    ErrorCodes::OperationIsKilledAndDelisted);
            // Format our response, if we have one
            Message& toSink = dbresponse.response;
            if (!toSink.empty()) {
                invariant(!OpMsg::isFlagSet(_inMessage, OpMsg::kMoreToCome));
                invariant(!OpMsg::isFlagSet(toSink, OpMsg::kChecksumPresent));

                // Update the header for the response message.
                toSink.header().setId(nextMessageId());
                toSink.header().setResponseToMsgId(_inMessage.header().getId());
                if (OpMsg::isFlagSet(_inMessage, OpMsg::kChecksumPresent)) {
#ifdef MONGO_CONFIG_SSL
                    if (!SSLPeerInfo::forSession(session()).isTLS) {
                        OpMsg::appendChecksum(&toSink);
                    }
#else
                    OpMsg::appendChecksum(&toSink);
#endif
                }

                // If the incoming message has the exhaust flag set, then we bypass the normal RPC
                // behavior. We will sink the response to the network, but we also synthesize a new
                // request, as if we sourced a new message from the network. This new request is
                // sent to the database once again to be processed. This cycle repeats as long as
                // the command indicates the exhaust stream should continue.
                _inMessage = makeExhaustMessage(_inMessage, &dbresponse);
                _inExhaust = !_inMessage.empty();

                networkCounter.hitLogicalOut(toSink.size());

                beforeCompressingExhaustResponse.executeIf(
                    [&](const BSONObj&) {
                        // Nothing to do as we only need to record the incident.
                    },
                    [&](const BSONObj&) { return _compressorId.has_value() && _inExhaust; });

                if (_compressorId) {
                    auto swm = compressorMgr.compressMessage(toSink, &_compressorId.value());
                    uassertStatusOK(swm.getStatus());
                    toSink = swm.getValue();
                }

                TrafficRecorder::get(_serviceContext)
                    .observe(session(), _serviceContext->getPreciseClockSource()->now(), toSink);

                _outMessage = std::move(toSink);
            } else {
                _state.store(State::Source);
                _inMessage.reset();
                _outMessage.reset();
                _inExhaust = false;
            }
        });
}

void ServiceStateMachine::Impl::start(ServiceExecutorContext seCtx) {
    {
        auto client = _clientStrand->getClientPointer();
        stdx::lock_guard lk(*client);
        ServiceExecutorContext::set(client, std::move(seCtx));
    }

    invariant(_state.swap(State::Source) == State::Created);
    invariant(!_inExhaust, "Cannot start the state machine in exhaust mode");

    scheduleNewLoop(Status::OK());
}

void ServiceStateMachine::Impl::scheduleNewLoop(Status status) try {
    // We may or may not have an operation context, but it should definitely be gone now.
    _opCtx.reset();

    uassertStatusOK(status);

    auto cb = [this, anchor = shared_from_this()](Status executorStatus) {
        _clientStrand->run([&] { startNewLoop(executorStatus); });
    };

    try {
        // Start our loop again with a new stack.
        if (_inExhaust) {
            // If we're in exhaust, we're not expecting more data.
            executor()->schedule(std::move(cb));
        } else {
            executor()->runOnDataAvailable(session(), std::move(cb));
        }
    } catch (const DBException& ex) {
        LOGV2_WARNING_OPTIONS(22993,
                              {logv2::LogComponent::kExecutor},
                              "Unable to schedule a new loop for the service state machine",
                              "error"_attr = ex.toStatus());
        throw;
    }
} catch (const DBException& ex) {
    LOGV2_DEBUG(5763901, 2, "Terminating session due to error", "error"_attr = ex.toStatus());
    _state.store(State::EndSession);
    terminate();
    cleanupSession(ex.toStatus());
}

void ServiceStateMachine::Impl::startNewLoop(const Status& executorStatus) {
    if (!executorStatus.isOK()) {
        cleanupSession(executorStatus);
        return;
    }

    makeReadyFutureWith([&]() -> Future<void> {
        if (_inExhaust) {
            return Status::OK();
        } else {
            return sourceMessage();
        }
    })
        .then([this]() { return processMessage(); })
        .then([this]() -> Future<void> {
            if (_outMessage.empty()) {
                return Status::OK();
            }

            return sinkMessage();
        })
        .getAsync([this, anchor = shared_from_this()](Status status) {
            scheduleNewLoop(std::move(status));
        });
}

void ServiceStateMachine::Impl::terminate() {
    if (state() == State::Ended)
        return;

    session()->end();
}

void ServiceStateMachine::Impl::terminateIfTagsDontMatch(transport::Session::TagMask tags) {
    if (state() == State::Ended)
        return;

    auto sessionTags = session()->getTags();

    // If terminateIfTagsDontMatch gets called when we still are 'pending' where no tags have been
    // set, then skip the termination check.
    if ((sessionTags & tags) || (sessionTags & transport::Session::kPending)) {
        LOGV2(
            22991, "Skip closing connection for connection", "connectionId"_attr = session()->id());
        return;
    }

    terminate();
}

void ServiceStateMachine::Impl::cleanupExhaustResources() noexcept try {
    if (!_inExhaust) {
        return;
    }
    auto request = OpMsgRequest::parse(_inMessage);
    // Clean up cursor for exhaust getMore request.
    if (request.getCommandName() == "getMore"_sd) {
        auto cursorId = request.body["getMore"].Long();
        auto opCtx = Client::getCurrent()->makeOperationContext();
        // Fire and forget. This is a best effort attempt to immediately clean up the exhaust
        // cursor. If the killCursors request fails here for any reasons, it will still be cleaned
        // up once the cursor times out.
        auto nss = NamespaceString(request.getDatabase(), request.body["collection"].String());
        auto req = OpMsgRequest::fromDBAndBody(
            request.getDatabase(), KillCursorsCommandRequest(nss, {cursorId}).toBSON(BSONObj{}));
        _sep->handleRequest(opCtx.get(), req.serialize()).get();
    }
} catch (const DBException& e) {
    LOGV2(22992,
          "Error cleaning up resources for exhaust requests: {error}",
          "Error cleaning up resources for exhaust requests",
          "error"_attr = e.toStatus());
}

void ServiceStateMachine::Impl::setCleanupHook(std::function<void()> hook) {
    invariant(state() == State::Created);
    _cleanupHook = std::move(hook);
}

void ServiceStateMachine::Impl::cleanupSession(const Status& status) {
    LOGV2_DEBUG(5127900, 2, "Ending session", "error"_attr = status);

    cleanupExhaustResources();
    auto client = _clientStrand->getClientPointer();
    _sep->onClientDisconnect(client);

    {
        stdx::lock_guard lk(*client);
        transport::ServiceExecutorContext::reset(client);
    }

    _state.store(State::Ended);

    _inMessage.reset();

    _outMessage.reset();

    if (auto cleanupHook = std::exchange(_cleanupHook, {})) {
        cleanupHook();
    }
}

ServiceStateMachine::ServiceStateMachine(ServiceContext::UniqueClient client)
    : _impl{std::make_shared<Impl>(std::move(client))} {}

void ServiceStateMachine::start(ServiceExecutorContext seCtx) {
    _impl->start(std::move(seCtx));
}

void ServiceStateMachine::setCleanupHook(std::function<void()> hook) {
    _impl->setCleanupHook(std::move(hook));
}

void ServiceStateMachine::terminate() {
    _impl->terminate();
}

void ServiceStateMachine::terminateIfTagsDontMatch(transport::Session::TagMask tags) {
    _impl->terminateIfTagsDontMatch(tags);
}

}  // namespace transport
}  // namespace mongo
