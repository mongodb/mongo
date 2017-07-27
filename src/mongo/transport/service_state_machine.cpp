/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/service_state_machine.h"

#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/stats/counters.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ticket.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/thread_idle_callback.h"
#include "mongo/util/quick_exit.h"

namespace mongo {
namespace {
// Set up proper headers for formatting an exhaust request, if we need to
bool setExhaustMessage(Message* m, const DbResponse& dbresponse) {
    MsgData::View header = dbresponse.response.header();
    QueryResult::View qr = header.view2ptr();
    long long cursorid = qr.getCursorId();

    if (!cursorid) {
        return false;
    }

    invariant(dbresponse.exhaustNS.size() && dbresponse.exhaustNS[0]);

    auto ns = dbresponse.exhaustNS;  // m->reset() will free this so we must cache a copy

    m->reset();

    // Rebuild out the response.
    BufBuilder b(512);
    b.appendNum(static_cast<int>(0) /* size set later in setLen() */);
    b.appendNum(header.getId());               // message id
    b.appendNum(header.getResponseToMsgId());  // in response to
    b.appendNum(static_cast<int>(dbGetMore));  // opCode is OP_GET_MORE
    b.appendNum(static_cast<int>(0));          // Must be ZERO (reserved)
    b.appendStr(ns);                           // Namespace
    b.appendNum(static_cast<int>(0));          // ntoreturn
    b.appendNum(cursorid);                     // cursor id from the OP_REPLY

    MsgData::View(b.buf()).setLen(b.len());
    m->setData(b.release());

    return true;
}

}  // namespace

using transport::TransportLayer;
using transport::ServiceExecutor;

/*
 * This class wraps up the logic for swapping/unswapping the Client during runNext().
 */
class ServiceStateMachine::ThreadGuard {
    ThreadGuard(ThreadGuard&) = delete;
    ThreadGuard& operator=(ThreadGuard&) = delete;

public:
    explicit ThreadGuard(ServiceStateMachine* ssm)
        : _ssm{ssm},
          _haveTakenOwnership{!_ssm->_isOwned.test_and_set()},
          _oldThreadName{getThreadName().toString()} {
        const auto currentOwningThread = _ssm->_currentOwningThread.load();
        const auto currentThreadId = stdx::this_thread::get_id();

        // If this is true, then we are the "owner" of the Client and we should swap the
        // client/thread name before doing any work.
        if (_haveTakenOwnership) {
            _ssm->_currentOwningThread.store(currentThreadId);

            // Set up the thread name
            setThreadName(_ssm->_threadName);

            // These are sanity checks to make sure that the Client is what we expect it to be
            invariant(!haveClient());
            invariant(_ssm->_dbClient.get() == _ssm->_dbClientPtr);

            // Swap the current Client so calls to cc() work as expected
            Client::setCurrent(std::move(_ssm->_dbClient));
        } else if (currentOwningThread != currentThreadId) {
            // If the currentOwningThread does not equal the currentThreadId, then another thread
            // currently "owns" the Client and we should reschedule ourself.
            _okayToRunNext = false;
        }
    }

    ~ThreadGuard() {
        // If we are not the owner of the SSM, then do nothing. Something higher up the call stack
        // will have to clean up.
        if (!_haveTakenOwnership)
            return;

        // If the session has ended, then assume that it's unsafe to do anything but call the
        // cleanup hook.
        if (_ssm->state() == State::Ended) {
            // The cleanup hook may change as soon as we unlock the mutex, so move it out of the
            // ssm before unlocking the lock.
            auto cleanupHook = std::move(_ssm->_cleanupHook);
            if (cleanupHook)
                cleanupHook();

            return;
        }

        // Otherwise swap thread locals and thread names back into the SSM so its ready for the
        // next run.
        if (haveClient()) {
            _ssm->_dbClient = Client::releaseCurrent();
        }
        setThreadName(_oldThreadName);
        _ssm->_isOwned.clear();
    }

    // This bool operator reflects whether the ThreadGuard was able to take ownership of the thread
    // either higher up the call chain, or in this call. If this returns false, then it is not safe
    // to assume the thread has been setup correctly, or that any mutable state of the SSM is safe
    // to access except for the current _state value.
    explicit operator bool() const {
        return _okayToRunNext;
    }

    // Returns whether the thread guard is the owner of the SSM's state or not. Callers can use this
    // to determine whether their callchain is recursive.
    bool isOwner() const {
        return _haveTakenOwnership;
    }

private:
    ServiceStateMachine* _ssm;
    bool _haveTakenOwnership;
    const std::string _oldThreadName;
    bool _okayToRunNext = true;
};

std::shared_ptr<ServiceStateMachine> ServiceStateMachine::create(ServiceContext* svcContext,
                                                                 transport::SessionHandle session,
                                                                 bool sync) {
    return std::make_shared<ServiceStateMachine>(svcContext, std::move(session), sync);
}

ServiceStateMachine::ServiceStateMachine(ServiceContext* svcContext,
                                         transport::SessionHandle session,
                                         bool sync)
    : _state{State::Created},
      _sep{svcContext->getServiceEntryPoint()},
      _sync(sync),
      _serviceContext(svcContext),
      _sessionHandle(session),
      _dbClient{svcContext->makeClient("conn", std::move(session))},
      _dbClientPtr{_dbClient.get()},
      _threadName{str::stream() << "conn" << _session()->id()},
      _currentOwningThread{stdx::this_thread::get_id()} {}

const transport::SessionHandle& ServiceStateMachine::_session() const {
    return _sessionHandle;
}

void ServiceStateMachine::_sourceCallback(Status status) {
    // The first thing to do is create a ThreadGuard which will take ownership of the SSM in this
    // thread.
    ThreadGuard guard(this);
    // If the guard wasn't able to take ownership of the thread, then reschedule this call to
    // runNext() so that this thread can do other useful work with its timeslice instead of going
    // to sleep while waiting for the SSM to be released.
    if (!guard) {
        return _scheduleFunc([this, status] { _sourceCallback(status); },
                             ServiceExecutor::DeferredTask);
    }

    // Make sure we just called sourceMessage();
    invariant(state() == State::SourceWait);
    auto remote = _session()->remote();

    if (status.isOK()) {
        _state.store(State::Process);

        // Since we know that we're going to process a message, call scheduleNext() immediately
        // to schedule the call to processMessage() on the serviceExecutor (or just unwind the
        // stack)

        // If this callback doesn't own the ThreadGuard, then we're being called recursively,
        // and the executor shouldn't start a new thread to process the message - it can use this
        // one just after this returns.
        auto flags = guard.isOwner() ? ServiceExecutor::EmptyFlags : ServiceExecutor::DeferredTask;
        return scheduleNext(flags);
    } else if (ErrorCodes::isInterruption(status.code()) ||
               ErrorCodes::isNetworkError(status.code())) {
        LOG(2) << "Session from " << remote << " encountered a network error during SourceMessage";
        _state.store(State::EndSession);
    } else if (status == TransportLayer::TicketSessionClosedStatus) {
        // Our session may have been closed internally.
        LOG(2) << "Session from " << remote << " was closed internally during SourceMessage";
        _state.store(State::EndSession);
    } else {
        log() << "Error receiving request from client: " << status << ". Ending connection from "
              << remote << " (connection id: " << _session()->id() << ")";
        _state.store(State::EndSession);
    }

    // There was an error receiving a message from the client and we've already printed the error
    // so call runNextInGuard() to clean up the session without waiting.
    _runNextInGuard(guard);
}

void ServiceStateMachine::_sinkCallback(Status status) {
    // The first thing to do is create a ThreadGuard which will take ownership of the SSM in this
    // thread.
    ThreadGuard guard(this);
    // If the guard wasn't able to take ownership of the thread, then reschedule this call to
    // runNext() so that this thread can do other useful work with its timeslice instead of going
    // to sleep while waiting for the SSM to be released.
    if (!guard) {
        return _scheduleFunc([this, status] { _sinkCallback(status); },
                             ServiceExecutor::DeferredTask);
    }

    invariant(state() == State::SinkWait);

    // If there was an error sinking the message to the client, then we should print an error and
    // end the session. No need to unwind the stack, so this will runNextInGuard() and return.
    //
    // Otherwise, update the current state depending on whether we're in exhaust or not, and call
    // scheduleNext() to unwind the stack and do the next step.
    if (!status.isOK()) {
        log() << "Error sending response to client: " << status << ". Ending connection from "
              << _session()->remote() << " (connection id: " << _session()->id() << ")";
        _state.store(State::EndSession);
        return _runNextInGuard(guard);
    } else if (_inExhaust) {
        _state.store(State::Process);
    } else {
        _state.store(State::Source);
    }

    // If the session ended, then runNext to clean it up
    if (state() == State::EndSession) {
        _runNextInGuard(guard);
    } else {  // Otherwise scheduleNext to unwind the stack and run the next step later
        // If this callback doesn't own the ThreadGuard, then we're being called recursively,
        // and the executor shouldn't start a new thread to process the message - it can use this
        // one just after this returns.
        auto flags = guard.isOwner() ? ServiceExecutor::EmptyFlags : ServiceExecutor::DeferredTask;
        return scheduleNext(flags);
    }
}

void ServiceStateMachine::_processMessage(ThreadGuard& guard) {
    // This may have been called just after a failure to source a message, in which case this
    // should return early so the session can be cleaned up.
    if (state() != State::Process) {
        return;
    }
    invariant(!_inMessage.empty());

    auto& compressorMgr = MessageCompressorManager::forSession(_session());

    _compressorId = boost::none;
    if (_inMessage.operation() == dbCompressed) {
        MessageCompressorId compressorId;
        auto swm = compressorMgr.decompressMessage(_inMessage, &compressorId);
        uassertStatusOK(swm.getStatus());
        _inMessage = swm.getValue();
        _compressorId = compressorId;
    }

    networkCounter.hitLogicalIn(_inMessage.size());

    // Pass sourced Message to handler to generate response.
    auto opCtx = Client::getCurrent()->makeOperationContext();

    // The handleRequest is implemented in a subclass for mongod/mongos and actually all the
    // database work for this request.
    DbResponse dbresponse = _sep->handleRequest(opCtx.get(), _inMessage);

    // opCtx must be destroyed here so that the operation cannot show
    // up in currentOp results after the response reaches the client
    opCtx.reset();

    // Format our response, if we have one
    Message& toSink = dbresponse.response;
    if (!toSink.empty()) {
        invariant(!OpMsg::isFlagSet(_inMessage, OpMsg::kMoreToCome));
        toSink.header().setId(nextMessageId());
        toSink.header().setResponseToMsgId(_inMessage.header().getId());

        // If this is an exhaust cursor, don't source more Messages
        if (dbresponse.exhaustNS.size() > 0 && setExhaustMessage(&_inMessage, dbresponse)) {
            _inExhaust = true;
        } else {
            _inExhaust = false;
            _inMessage.reset();
        }

        networkCounter.hitLogicalOut(toSink.size());

        if (_compressorId) {
            auto swm = compressorMgr.compressMessage(toSink, &_compressorId.value());
            uassertStatusOK(swm.getStatus());
            toSink = swm.getValue();
        }

        // Sink our response to the client
        auto ticket = _session()->sinkMessage(toSink);

        _state.store(State::SinkWait);
        if (_sync) {
            _sinkCallback(_session()->getTransportLayer()->wait(std::move(ticket)));
        } else {
            _session()->getTransportLayer()->asyncWait(
                std::move(ticket), [this](Status status) { _sinkCallback(status); });
        }
    } else {
        _state.store(State::Source);
        _inMessage.reset();
        return scheduleNext(ServiceExecutor::DeferredTask);
    }
}

void ServiceStateMachine::runNext() {
    // The first thing to do is create a ThreadGuard which will take ownership of the SSM in this
    // thread.
    ThreadGuard guard(this);
    // If the guard wasn't able to take ownership of the thread, then reschedule this call to
    // runNext() so that this thread can do other useful work with its timeslice instead of going
    // to sleep while waiting for the SSM to be released.
    if (!guard) {
        return scheduleNext(ServiceExecutor::DeferredTask);
    }
    return _runNextInGuard(guard);
}

void ServiceStateMachine::_runNextInGuard(ThreadGuard& guard) {
    auto curState = state();
    invariant(curState != State::Ended);

    // If this is the first run of the SSM, then update its state to Source
    if (curState == State::Created) {
        curState = State::Source;
        _state.store(curState);
    }

    // Make sure the current Client got set correctly
    invariant(Client::getCurrent() == _dbClientPtr);
    try {
        switch (curState) {
            case State::Source: {
                invariant(_inMessage.empty());

                auto ticket = _session()->sourceMessage(&_inMessage);
                _state.store(State::SourceWait);
                if (_sync) {
                    _sourceCallback([this](auto ticket) {
                        MONGO_IDLE_THREAD_BLOCK;
                        return _session()->getTransportLayer()->wait(std::move(ticket));
                    }(std::move(ticket)));
                } else {
                    _session()->getTransportLayer()->asyncWait(
                        std::move(ticket), [this](Status status) { _sourceCallback(status); });
                    break;
                }
            }
            case State::Process:
                _processMessage(guard);
                break;
            case State::EndSession:
                // This will get handled below in an if statement. That way if an error occurs
                // you don't have to call runNext() again to clean up the session.
                break;
            default:
                MONGO_UNREACHABLE;
        }

        if ((_counter++ & 0xf) == 0) {
            markThreadIdle();
        }

        if (state() == State::EndSession) {
            _cleanupSession(guard);
        }

        return;
    } catch (const DBException& e) {
        // must be right above std::exception to avoid catching subclasses
        log() << "DBException handling request, closing client connection: " << redact(e);
    } catch (const std::exception& e) {
        error() << "Uncaught std::exception: " << e.what() << ", terminating";
        quickExit(EXIT_UNCAUGHT);
    }

    _state.store(State::EndSession);
    _cleanupSession(guard);
}

void ServiceStateMachine::scheduleNext(ServiceExecutor::ScheduleFlags flags) {
    _maybeScheduleFunc(_serviceContext->getServiceExecutor(), [this] { runNext(); }, flags);
}

void ServiceStateMachine::terminateIfTagsDontMatch(transport::Session::TagMask tags) {
    if (state() == State::Ended)
        return;

    if (_session()->getTags() & tags) {
        log() << "Skip closing connection for connection # " << _session()->id();
        return;
    }

    _session()->getTransportLayer()->end(_session());
}

void ServiceStateMachine::setCleanupHook(stdx::function<void()> hook) {
    invariant(state() == State::Created);
    _cleanupHook = std::move(hook);
}

ServiceStateMachine::State ServiceStateMachine::state() {
    return _state.load();
}

void ServiceStateMachine::_cleanupSession(ThreadGuard& guard) {
    _state.store(State::Ended);

    _inMessage.reset();

    // By ignoring the return value of Client::releaseCurrent() we destroy the session.
    // _dbClient is now nullptr and _dbClientPtr is invalid and should never be accessed.
    Client::releaseCurrent();
}

}  // namespace mongo
