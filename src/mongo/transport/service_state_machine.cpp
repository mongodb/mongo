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

#include "mongo/transport/service_state_machine.h"
#include "mongo/base/object_pool.h"
#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor_task_names.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_exception.h"
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

using transport::ServiceExecutor;
using transport::TransportLayer;

/*
 * This class wraps up the logic for swapping/unswapping the Client during runNext().
 *
 * In debug builds this also ensures that only one thread is working on the SSM at once.
 */
class ServiceStateMachine::ThreadGuard {
    ThreadGuard(ThreadGuard&) = delete;
    ThreadGuard& operator=(ThreadGuard&) = delete;

public:
    explicit ThreadGuard(ServiceStateMachine* ssm) : _ssm{ssm} {
        auto owned = _ssm->_owned.compareAndSwap(Ownership::kUnowned, Ownership::kOwned);
        if (owned == Ownership::kStatic) {
            dassert(haveClient());
            dassert(Client::getCurrent() == _ssm->_dbClientPtr);
            _haveTakenOwnership = true;
            return;
        }

#ifdef MONGO_CONFIG_DEBUG_BUILD
        invariant(owned == Ownership::kUnowned);
        _ssm->_owningThread.store(stdx::this_thread::get_id());
#endif

        // Set up the thread name
        // auto oldThreadName = getThreadName();
        // if (oldThreadName != _ssm->_threadName) {
        //     _ssm->_oldThreadName = getThreadName().toString();
        //     setThreadName(_ssm->_threadName);
        // }

        // Swap the current Client so calls to cc() work as expected
        Client::setCurrent(std::move(_ssm->_dbClient));
        _haveTakenOwnership = true;
    }

    // Constructing from a moved ThreadGuard invalidates the other thread guard.
    ThreadGuard(ThreadGuard&& other)
        : _ssm(other._ssm), _haveTakenOwnership(other._haveTakenOwnership) {
        other._haveTakenOwnership = false;
    }

    ThreadGuard& operator=(ThreadGuard&& other) {
        if (this != &other) {
            _ssm = other._ssm;
            _haveTakenOwnership = other._haveTakenOwnership;
            other._haveTakenOwnership = false;
        }
        return *this;
    };

    ThreadGuard() = delete;

    ~ThreadGuard() {
        if (_haveTakenOwnership)
            release();
    }

    explicit operator bool() const {
#ifdef MONGO_CONFIG_DEBUG_BUILD
        if (_haveTakenOwnership) {
            invariant(_ssm->_owned.load() != Ownership::kUnowned);
            invariant(_ssm->_owningThread.load() == stdx::this_thread::get_id());
            return true;
        } else {
            return false;
        }
#else
        return _haveTakenOwnership;
#endif
    }

    void markStaticOwnership() {
        dassert(static_cast<bool>(*this));
        _ssm->_owned.store(Ownership::kStatic);
    }

    void release() {
        auto owned = _ssm->_owned.load();

#ifdef MONGO_CONFIG_DEBUG_BUILD
        dassert(_haveTakenOwnership);
        dassert(owned != Ownership::kUnowned);
        dassert(_ssm->_owningThread.load() == stdx::this_thread::get_id());
#endif
        if (owned != Ownership::kStatic) {
            if (haveClient()) {
                _ssm->_dbClient = Client::releaseCurrent();
            }

            // if (!_ssm->_oldThreadName.empty()) {
            //     setThreadName(_ssm->_oldThreadName);
            // }
        }

        // If the session has ended, then it's unsafe to do anything but call the cleanup hook.
        if (_ssm->state() == State::Ended) {
            // The cleanup hook gets moved out of _ssm->_cleanupHook so that it can only be called
            // once.
            auto cleanupHook = std::move(_ssm->_cleanupHook);
            if (cleanupHook)
                cleanupHook();

            // It's very important that the Guard returns here and that the SSM's state does not
            // get modified in any way after the cleanup hook is called.
            return;
        }

        _haveTakenOwnership = false;
        // If owned != Ownership::kOwned here then it can only equal Ownership::kStatic and we
        // should just return
        if (owned == Ownership::kOwned) {
            _ssm->_owned.store(Ownership::kUnowned);
        }
    }

private:
    ServiceStateMachine* _ssm;
    bool _haveTakenOwnership = false;
};

// static moodycamel::ConcurrentQueue<std::unique_ptr<ServiceStateMachine>> ssmPool{};

std::shared_ptr<ServiceStateMachine> ServiceStateMachine::create(ServiceContext* svcContext,
                                                                 transport::SessionHandle session,
                                                                 transport::Mode transportMode,
                                                                 uint16_t groupId) {
    // return std::make_shared<ServiceStateMachine>(svcContext, std::move(session), transportMode,
    // 0);
    return ObjectPool<ServiceStateMachine>::newObjectSharedPointer(
        svcContext, std::move(session), transportMode, groupId);
    // ServiceStateMachine* ssm{nullptr};
    // std::unique_ptr<ServiceStateMachine> ssmUptr{nullptr};
    // bool success = ssmPool.try_dequeue(ssmUptr);
    // if (success) {
    //     ssm = ssmUptr.release();
    //     ssm->Reset(svcContext, std::move(session), transportMode);
    // } else {
    //     ssm = new ServiceStateMachine(svcContext, std::move(session), transportMode, group_id);
    // }
    // return std::shared_ptr<ServiceStateMachine>(ssm, [](ServiceStateMachine* ptr) {
    //     ssmPool.enqueue(std::unique_ptr<ServiceStateMachine>(ptr));
    // });
}

ServiceStateMachine::ServiceStateMachine(ServiceContext* svcContext,
                                         transport::SessionHandle session,
                                         transport::Mode transportMode,
                                         uint16_t groupId)
    : _state{State::Created},
      _sep{svcContext->getServiceEntryPoint()},
      _transportMode(transportMode),
      _serviceContext(svcContext),
      _serviceExecutor(_serviceContext->getServiceExecutor()),
      _sessionHandle(session),
      _threadName{str::stream() << "conn" << _session()->id()},
      _dbClient{svcContext->makeClient(_threadName, std::move(session))},
      _dbClientPtr{_dbClient.get()},
      _threadGroupId(groupId) {
    MONGO_LOG(1) << "ServiceStateMachine::ServiceStateMachine";
}

void ServiceStateMachine::reset(ServiceContext* svcContext,
                                transport::SessionHandle session,
                                transport::Mode transportMode,
                                uint16_t groupId) {
    MONGO_LOG(1) << "ServiceStateMachine::Reset";
    _state.store(State::Created);
    _sep = svcContext->getServiceEntryPoint();
    _transportMode = transportMode;
    _serviceContext = svcContext;
    _serviceExecutor = _serviceContext->getServiceExecutor();
    _sessionHandle = session;
    // _threadName = str::stream() << "conn" << _session()->id();
    _dbClient = svcContext->makeClient(_threadName, std::move(session));
    _dbClientPtr = _dbClient.get();
    _threadGroupId = groupId;
    _owned.store(Ownership::kUnowned);
}

const transport::SessionHandle& ServiceStateMachine::_session() const {
    return _sessionHandle;
}

void ServiceStateMachine::_sourceMessage(ThreadGuard guard) {
    MONGO_LOG(1) << "ServiceStateMachine::_sourceMessage";
    invariant(_inMessage.empty());
    invariant(_state.load() == State::Source);
    _state.store(State::SourceWait);
    guard.release();

    auto sourceMsgImpl = [&] {
        if (_transportMode == transport::Mode::kSynchronous) {
            MONGO_IDLE_THREAD_BLOCK;
            return Future<Message>::makeReady(_session()->sourceMessage());
        } else {
            invariant(_transportMode == transport::Mode::kAsynchronous);
            return _session()->asyncSourceMessage();
        }
    };

    sourceMsgImpl().getAsync([this](StatusWith<Message> msg) {
        if (msg.isOK()) {
            _inMessage = std::move(msg.getValue());
            invariant(!_inMessage.empty());
        }
        _sourceCallback(msg.getStatus());
    });
}

void ServiceStateMachine::_sinkMessage(ThreadGuard guard, Message toSink) {
    MONGO_LOG(1) << "ServiceStateMachine::_sinkMessage";
    // Sink our response to the client
    invariant(_state.load() == State::Process);
    _state.store(State::SinkWait);
    guard.release();

    auto sinkMsgImpl = [&] {
        if (_transportMode == transport::Mode::kSynchronous) {
            // We don't consider ourselves idle while sending the reply since we are still doing
            // work on behalf of the client. Contrast that with sourceMessage() where we are waiting
            // for the client to send us more work to do.
            return Future<void>::makeReady(_session()->sinkMessage(std::move(toSink)));
        } else {
            invariant(_transportMode == transport::Mode::kAsynchronous);
            return _session()->asyncSinkMessage(std::move(toSink));
        }
    };

    sinkMsgImpl().getAsync([this](Status status) { _sinkCallback(std::move(status)); });
}

void ServiceStateMachine::_sourceCallback(Status status) {
    MONGO_LOG(1) << "ServiceStateMachine::_sourceCallback";
    // The first thing to do is create a ThreadGuard which will take ownership of the SSM in this
    // thread.
    ThreadGuard guard(this);

    // Make sure we just called sourceMessage();
    dassert(state() == State::SourceWait);
    auto remote = _session()->remote();

    if (status.isOK()) {
        _state.store(State::Process);

        // Since we know that we're going to process a message, call scheduleNext() immediately
        // to schedule the call to processMessage() on the serviceExecutor (or just unwind the
        // stack)

        // If this callback doesn't own the ThreadGuard, then we're being called recursively,
        // and the executor shouldn't start a new thread to process the message - it can use this
        // one just after this returns.
        return _scheduleNextWithGuard(std::move(guard),
                                      ServiceExecutor::kMayRecurse,
                                      transport::ServiceExecutorTaskName::kSSMProcessMessage);
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
    _runNextInGuard(std::move(guard));
}

void ServiceStateMachine::_sinkCallback(Status status) {
    MONGO_LOG(1) << "ServiceStateMachine::_sinkCallback";
    // The first thing to do is create a ThreadGuard which will take ownership of the SSM in this
    // thread.
    ThreadGuard guard(this);

    dassert(state() == State::SinkWait);

    // If there was an error sinking the message to the client, then we should print an error and
    // end the session. No need to unwind the stack, so this will runNextInGuard() and return.
    //
    // Otherwise, update the current state depending on whether we're in exhaust or not, and call
    // scheduleNext() to unwind the stack and do the next step.
    if (!status.isOK()) {
        log() << "Error sending response to client: " << status << ". Ending connection from "
              << _session()->remote() << " (connection id: " << _session()->id() << ")";
        _state.store(State::EndSession);
        return _runNextInGuard(std::move(guard));
    } else if (_inExhaust) {
        _state.store(State::Process);
        return _scheduleNextWithGuard(std::move(guard),
                                      ServiceExecutor::kDeferredTask |
                                          ServiceExecutor::kMayYieldBeforeSchedule,
                                      transport::ServiceExecutorTaskName::kSSMExhaustMessage);
    } else {
        _state.store(State::Source);
        return _scheduleNextWithGuard(std::move(guard),
                                      ServiceExecutor::kDeferredTask |
                                          ServiceExecutor::kMayYieldBeforeSchedule,
                                      transport::ServiceExecutorTaskName::kSSMSourceMessage);
    }
}

void ServiceStateMachine::_processMessage(ThreadGuard guard) {
    MONGO_LOG(1) << "ServiceStateMachine::_processMessage";
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


    DbResponse dbresponse;
    {
        // OperationContext opCtx(Client::getCurrent(), _serviceContext->nextOpId());
        // _serviceContext->initOperationContext(&opCtx);
        auto opCtx = Client::getCurrent()->makeOperationContext();

        // MONGO_LOG(0) << "Operation Context decorations memeory usage: " << opCtx->sizeBytes();
        
        if (serverGlobalParams.enableCoroutine) {
            opCtx->setCoroutineFunctors(&_coroYield, &_coroResume);
        }

        // The handleRequest is implemented in a subclass for mongod/mongos and actually all the
        // database work for this request.
        dbresponse = _sep->handleRequest(opCtx.get(), _inMessage);


        _coroStatus = CoroStatus::Empty;
        _serviceExecutor->ongoingCoroutineCountUpdate(_threadGroupId, -1);

        // opCtx must be destroyed here so that the operation cannot show
        // up in currentOp results after the response reaches the client
        // opCtx.reset();
        // _serviceContext->destoryOperationContext(&opCtx);
    }

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
        _sinkMessage(std::move(guard), std::move(toSink));

    } else {
        _state.store(State::Source);
        _inMessage.reset();
        return _scheduleNextWithGuard(std::move(guard),
                                      ServiceExecutor::kDeferredTask,
                                      transport::ServiceExecutorTaskName::kSSMSourceMessage);
    }
}

void ServiceStateMachine::runNext() {
    MONGO_LOG(1) << "ServiceStateMachine::runNext";
    return _runNextInGuard(ThreadGuard(this));
}

void ServiceStateMachine::_runNextInGuard(ThreadGuard guard) {
    MONGO_LOG(1) << "ServiceStateMachine::_runNextInGuard";
    auto curState = state();
    dassert(curState != State::Ended);

    // If this is the first run of the SSM, then update its state to Source
    if (curState == State::Created) {
        curState = State::Source;
        _state.store(curState);
    }

    // Make sure the current Client got set correctly
    dassert(Client::getCurrent() == _dbClientPtr);
    try {
        switch (curState) {
            case State::Source:
                _sourceMessage(std::move(guard));
                break;
            case State::Process: {
                if (!serverGlobalParams.enableCoroutine) {
                    _processMessage(std::move(guard));
                } else {
                    if (_coroStatus == CoroStatus::Empty) {
                        MONGO_LOG(1) << "coroutine begin";
                        _coroStatus = CoroStatus::OnGoing;
                        _serviceExecutor->ongoingCoroutineCountUpdate(_threadGroupId, 1);
                        auto func = [this, ssm = shared_from_this()] {
                            Client::setCurrent(std::move(ssm->_dbClient));
                            _runResumeProcess();
                        };

                        _coroResume =
                            _serviceExecutor->coroutineResumeFunctor(_threadGroupId, func);

                        boost::context::stack_context sc = coroStackContext();
                        boost::context::preallocated prealloc(sc.sp, sc.size, sc);
                        _source = boost::context::callcc(
                            std::allocator_arg,
                            prealloc,
                            NoopAllocator(),
                            [this, &guard](boost::context::continuation&& sink) {
                                _coroYield = [this, &sink]() {
                                    MONGO_LOG(1) << "call yield";
                                    _dbClient = Client::releaseCurrent();
                                    sink = sink.resume();
                                };
                                _processMessage(std::move(guard));
                                return std::move(sink);
                            });

                        // _source =
                        //     boost::context::callcc([this, &guard](boost::context::continuation&&
                        //     sink) {
                        //         _coroYield = [this, &sink]() {
                        //             MONGO_LOG(1) << "call yield";
                        //             _dbClient = Client::releaseCurrent();
                        //             sink = sink.resume();
                        //         };
                        //         _processMessage(std::move(guard));
                        //         return std::move(sink);
                        //     });
                    } else if (_coroStatus == CoroStatus::OnGoing) {
                        MONGO_LOG(1) << "coroutine ongoing";
                        _source = _source.resume();
                    }
                }
            } break;
            case State::EndSession:
                _cleanupSession(std::move(guard));
                break;
            default:
                MONGO_UNREACHABLE;
        }

        return;
    } catch (const DBException& e) {
        // must be right above std::exception to avoid catching subclasses
        log() << "DBException handling request, closing client connection: " << redact(e);
    } catch (const std::exception& e) {
        error() << "Uncaught std::exception: " << e.what() << ", terminating";
        quickExit(EXIT_UNCAUGHT);
    }

    if (!guard) {
        guard = ThreadGuard(this);
    }
    _state.store(State::EndSession);
    _cleanupSession(std::move(guard));
}

void ServiceStateMachine::_runResumeProcess() {
    MONGO_LOG(1) << "ServiceStateMachine::_resumeRun";
    if (_coroStatus == CoroStatus::OnGoing) {
        MONGO_LOG(1) << "coroutine ongoing";
        _source = _source.resume();
    }
}

void ServiceStateMachine::start(Ownership ownershipModel) {
    MONGO_LOG(1) << "ServiceStateMachine::start";
    _scheduleNextWithGuard(ThreadGuard(this),
                           transport::ServiceExecutor::kEmptyFlags,
                           transport::ServiceExecutorTaskName::kSSMStartSession,
                           ownershipModel);
}

void ServiceStateMachine::_scheduleNextWithGuard(ThreadGuard guard,
                                                 transport::ServiceExecutor::ScheduleFlags flags,
                                                 transport::ServiceExecutorTaskName taskName,
                                                 Ownership ownershipModel) {
    MONGO_LOG(1) << "ServiceStateMachine::_scheduleNextWithGuard";
    auto func = [ssm = shared_from_this(), ownershipModel] {
        ThreadGuard guard(ssm.get());
        if (ownershipModel == Ownership::kStatic)
            guard.markStaticOwnership();
        ssm->_runNextInGuard(std::move(guard));
    };

    guard.release();
    // Status status = Status::OK();
    Status status = _serviceExecutor->schedule(std::move(func), flags, taskName, _threadGroupId);
    // if (taskName == transport::ServiceExecutorTaskName::kSSMProcessMessage) {
    //     // coroutine mode in actually
    //     status = _serviceExecutor->schedule(std::move(func), flags, taskName, _threadGroupId);
    // } else {
    //     // use adaptive mode to handle network task
    //     status = _serviceContext->getServiceExecutor()->schedule(std::move(func), flags,
    //     taskName);
    // }

    if (status.isOK()) {
        return;
    }

    // We've had an error, reacquire the ThreadGuard and destroy the SSM
    ThreadGuard terminateGuard(this);

    // The service executor failed to schedule the task. This could for example be that we failed
    // to start a worker thread. Terminate this connection to leave the system in a valid state.
    _terminateAndLogIfError(status);
    _cleanupSession(std::move(terminateGuard));
}

void ServiceStateMachine::terminate() {
    MONGO_LOG(1) << "ServiceStateMachine::terminate";
    if (state() == State::Ended)
        return;

    _session()->end();
}

void ServiceStateMachine::terminateIfTagsDontMatch(transport::Session::TagMask tags) {
    MONGO_LOG(1) << "ServiceStateMachine::terminateIfTagsDontMatch";
    if (state() == State::Ended)
        return;

    auto sessionTags = _session()->getTags();

    // If terminateIfTagsDontMatch gets called when we still are 'pending' where no tags have been
    // set, then skip the termination check.
    if ((sessionTags & tags) || (sessionTags & transport::Session::kPending)) {
        log() << "Skip closing connection for connection # " << _session()->id();
        return;
    }

    terminate();
}

void ServiceStateMachine::setCleanupHook(stdx::function<void()> hook) {
    MONGO_LOG(1) << "ServiceStateMachine::setCleanupHook";
    invariant(state() == State::Created);
    _cleanupHook = std::move(hook);
}

void ServiceStateMachine::setServiceExecutor(transport::ServiceExecutor* serviceExecutor) {
    MONGO_LOG(1) << "ServiceStateMachine::setServiceExecutor";
    _serviceExecutor = serviceExecutor;
}

void ServiceStateMachine::setThreadGroupId(size_t id) {
    MONGO_LOG(1) << "ServiceStateMachine::setThreadGroupId. id: " << id;
    _threadGroupId = id;
}

ServiceStateMachine::State ServiceStateMachine::state() {
    return _state.load();
}

void ServiceStateMachine::_terminateAndLogIfError(Status status) {
    if (!status.isOK()) {
        warning(logger::LogComponent::kExecutor) << "Terminating session due to error: " << status;
        terminate();
    }
}

void ServiceStateMachine::_cleanupSession(ThreadGuard guard) {
    MONGO_LOG(1) << "ServiceStateMachine::_cleanupSession";
    _state.store(State::Ended);

    _inMessage.reset();

    // By ignoring the return value of Client::releaseCurrent() we destroy the session.
    // _dbClient is now nullptr and _dbClientPtr is invalid and should never be accessed.
    Client::releaseCurrent();
}

}  // namespace mongo
