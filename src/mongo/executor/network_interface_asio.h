/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#pragma once

#include <asio.hpp>

#include <boost/optional.hpp>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>

#include "mongo/base/status.h"
#include "mongo/base/system_error.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/protocol.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace executor {

class AsyncStreamFactoryInterface;
class AsyncStreamInterface;

/**
 * Implementation of the replication system's network interface using Christopher
 * Kohlhoff's ASIO library instead of existing MongoDB networking primitives.
 */
class NetworkInterfaceASIO final : public NetworkInterface {
public:
    NetworkInterfaceASIO(std::unique_ptr<AsyncStreamFactoryInterface> streamFactory,
                         std::unique_ptr<NetworkConnectionHook> networkConnectionHook);
    NetworkInterfaceASIO(std::unique_ptr<AsyncStreamFactoryInterface> streamFactory);
    std::string getDiagnosticString() override;
    std::string getHostName() override;
    void startup() override;
    void shutdown() override;
    void waitForWork() override;
    void waitForWorkUntil(Date_t when) override;
    void signalWorkAvailable() override;
    Date_t now() override;
    void startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                      const RemoteCommandRequest& request,
                      const RemoteCommandCompletionFn& onFinish) override;
    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) override;
    void setAlarm(Date_t when, const stdx::function<void()>& action) override;

    bool inShutdown() const;

private:
    using ResponseStatus = TaskExecutor::ResponseStatus;
    using NetworkInterface::RemoteCommandCompletionFn;
    using NetworkOpHandler = stdx::function<void(std::error_code, size_t)>;

    enum class State { kReady, kRunning, kShutdown };

    /**
     * AsyncConnection encapsulates the per-connection state we maintain.
     */
    class AsyncConnection {
    public:
        AsyncConnection(std::unique_ptr<AsyncStreamInterface>, rpc::ProtocolSet serverProtocols);

        AsyncStreamInterface& stream();

        rpc::ProtocolSet serverProtocols() const;
        rpc::ProtocolSet clientProtocols() const;
        void setServerProtocols(rpc::ProtocolSet protocols);

// Explicit move construction and assignment to support MSVC
#if defined(_MSC_VER) && _MSC_VER < 1900
        AsyncConnection(AsyncConnection&&);
        AsyncConnection& operator=(AsyncConnection&&);
#else
        AsyncConnection(AsyncConnection&&) = default;
        AsyncConnection& operator=(AsyncConnection&&) = default;
#endif

    private:
        std::unique_ptr<AsyncStreamInterface> _stream;

        rpc::ProtocolSet _serverProtocols;
        rpc::ProtocolSet _clientProtocols{rpc::supports::kAll};
    };

    /**
     * AsyncCommand holds state for a currently running or soon-to-be-run command.
     */
    class AsyncCommand {
    public:
        AsyncCommand(AsyncConnection* conn, Message&& command, Date_t now);

        NetworkInterfaceASIO::AsyncConnection& conn();

        Message& toSend();
        Message& toRecv();
        MSGHEADER::Value& header();

        ResponseStatus response(rpc::Protocol protocol, Date_t now);

    private:
        NetworkInterfaceASIO::AsyncConnection* const _conn;

        Message _toSend;
        Message _toRecv;

        // TODO: Investigate efficiency of storing header separately.
        MSGHEADER::Value _header;

        const Date_t _start;
    };

    /**
     * Helper object to manage individual network operations.
     */
    class AsyncOp {
    public:
        AsyncOp(const TaskExecutor::CallbackHandle& cbHandle,
                const RemoteCommandRequest& request,
                const RemoteCommandCompletionFn& onFinish,
                Date_t now);

        void cancel();
        bool canceled() const;

        const TaskExecutor::CallbackHandle& cbHandle() const;

        AsyncConnection& connection();

        void setConnection(AsyncConnection&& conn);

        // AsyncOp may run multiple commands over its lifetime (for example, an ismaster
        // command, the command provided to the NetworkInterface via startCommand(), etc.)
        // Calling beginCommand() resets internal state to prepare to run newCommand.
        AsyncCommand& beginCommand(const RemoteCommandRequest& request,
                                   rpc::Protocol protocol,
                                   Date_t now);
        AsyncCommand& beginCommand(Message&& newCommand, Date_t now);
        AsyncCommand& command();

        void finish(const TaskExecutor::ResponseStatus& status);

        const RemoteCommandRequest& request() const;

        Date_t start() const;

        rpc::Protocol operationProtocol() const;

        void setOperationProtocol(rpc::Protocol proto);

    private:
        // Information describing a task enqueued on the NetworkInterface
        // via a call to startCommand().
        TaskExecutor::CallbackHandle _cbHandle;
        RemoteCommandRequest _request;
        RemoteCommandCompletionFn _onFinish;

        /**
         * The connection state used to service this request. We wrap it in an optional
         * as it is instantiated at some point after the AsyncOp is created.
         */
        boost::optional<AsyncConnection> _connection;

        /**
         * The RPC protocol used for this operation. We wrap it in an optional as it
         * is not known until we obtain a connection.
         */
        boost::optional<rpc::Protocol> _operationProtocol;

        const Date_t _start;

        AtomicUInt64 _canceled;

        /**
         * An AsyncOp may run 0, 1, or multiple commands over its lifetime.
         * AsyncOp only holds at most a single AsyncCommand object at a time,
         * representing its current running or next-to-be-run command, if there is one.
         */
        boost::optional<AsyncCommand> _command;
    };

    void _startCommand(AsyncOp* op);

    /**
     * Wraps a completion handler in pre-condition checks.
     * When we resume after an asynchronous call, we may find the following:
     *    - the AsyncOp has been canceled in the interim (via cancelCommand())
     *    - the asynchronous call has returned a non-OK error code
     * Should both conditions be present, we handle cancelation over errors. States use
     * _validateAndRun() to perform these checks before advancing the state machine.
     */
    template <typename Handler>
    void _validateAndRun(AsyncOp* op, std::error_code ec, Handler&& handler) {
        if (op->canceled())
            return _completeOperation(op,
                                      Status(ErrorCodes::CallbackCanceled, "Callback canceled"));
        if (ec)
            return _networkErrorCallback(op, ec);

        handler();
    }

    // Connection
    void _connect(AsyncOp* op);

    // setup plaintext TCP socket
    void _setupSocket(AsyncOp* op, asio::ip::tcp::resolver::iterator endpoints);

    void _runIsMaster(AsyncOp* op);
    void _runConnectionHook(AsyncOp* op);
    void _authenticate(AsyncOp* op);

    // Communication state machine
    void _beginCommunication(AsyncOp* op);
    void _completedOpCallback(AsyncOp* op);
    void _networkErrorCallback(AsyncOp* op, const std::error_code& ec);
    void _completeOperation(AsyncOp* op, const TaskExecutor::ResponseStatus& resp);

    void _signalWorkAvailable_inlock();

    void _asyncRunCommand(AsyncCommand* cmd, NetworkOpHandler handler);

    asio::io_service _io_service;
    stdx::thread _serviceRunner;

    std::unique_ptr<NetworkConnectionHook> _hook;

    asio::ip::tcp::resolver _resolver;

    std::atomic<State> _state;

    std::unique_ptr<AsyncStreamFactoryInterface> _streamFactory;

    stdx::mutex _inProgressMutex;
    std::unordered_map<AsyncOp*, std::unique_ptr<AsyncOp>> _inProgress;

    stdx::mutex _executorMutex;
    bool _isExecutorRunnable;
    stdx::condition_variable _isExecutorRunnableCondition;
};

}  // namespace executor
}  // namespace mongo
