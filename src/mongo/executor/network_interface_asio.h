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
#include <atomic>
#include <boost/optional.hpp>
#include <system_error>
#include <thread>
#include <unordered_map>

#include "mongo/client/connection_pool.h"
#include "mongo/client/remote_command_runner.h"
#include "mongo/executor/network_interface.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace executor {

/**
 * Implementation of the replication system's network interface using Christopher
 * Kohlhoff's ASIO library instead of existing MongoDB networking primitives.
 */
class NetworkInterfaceASIO final : public NetworkInterface {
public:
    NetworkInterfaceASIO();
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

    bool inShutdown() const;

private:
    enum class State { kReady, kRunning, kShutdown };

    /**
     * AsyncConnection encapsulates the per-connection state we maintain.
     */
    class AsyncConnection {
    public:
        AsyncConnection(
            ConnectionPool::ConnectionPtr&& booststrapConn,
            asio::ip::tcp::socket&& sock);

        asio::ip::tcp::socket* sock();

    private:
        ConnectionPool::ConnectionPtr _bootstrapConn;
        asio::ip::tcp::socket _sock;
    };

    /**
     * Helper object to manage individual network operations.
     */
    class AsyncOp {
    public:
        AsyncOp(const TaskExecutor::CallbackHandle& cbHandle,
                const RemoteCommandRequest& request,
                const RemoteCommandCompletionFn& onFinish,
                Date_t now,
                int id);

        std::string toString() const;

        void cancel();
        bool canceled() const;

        const TaskExecutor::CallbackHandle& cbHandle() const;

        void complete(Date_t now);

        AsyncConnection* connection();

        void connect(ConnectionPool* const pool, asio::io_service* service, Date_t now);
        bool connected() const;

        void finish(const TaskExecutor::ResponseStatus& status);

        MSGHEADER::Value* header();

        const RemoteCommandRequest& request() const;

        void setOutput(const BSONObj& bson);

        Date_t start() const;

        Message* toSend();
        Message* toRecv();

    private:
        enum class OpState {
            kReady,
            kConnectionAcquired,
            kConnectionVerified,
            kConnected,
            kCompleted
        };

        // Information describing an in-flight command.
        TaskExecutor::CallbackHandle _cbHandle;
        RemoteCommandRequest _request;
        RemoteCommandCompletionFn _onFinish;

        /**
         * The connection state used to service this request. We wrap it in an optional
         * as it is instantiated at some point after the AsyncOp is created.
         */
        boost::optional<AsyncConnection> _connection;

        const Date_t _start;

        OpState _state;
        AtomicUInt64 _canceled;

        Message _toSend;
        Message _toRecv;
        MSGHEADER::Value _header;

        BSONObj _output;

        const int _id;
    };

    void _messageFromRequest(const RemoteCommandRequest& request,
                             Message* toSend,
                             bool useOpCommand = false);

    void _asyncSendSimpleMessage(AsyncOp* op, const asio::const_buffer& buf);

    void _completedWriteCallback(AsyncOp* op);
    void _networkErrorCallback(AsyncOp* op, const std::error_code& ec);
    void _completeOperation(AsyncOp* op);
    void _keepAlive(AsyncOp* op);
    void _recvMessageHeader(AsyncOp* op);
    void _recvMessageBody(AsyncOp* op);
    void _receiveResponse(AsyncOp* op);
    void _signalWorkAvailable_inlock();

    asio::io_service _io_service;
    stdx::thread _serviceRunner;

    std::atomic<State> _state;

    stdx::mutex _inProgressMutex;
    std::unordered_map<AsyncOp*, std::unique_ptr<AsyncOp>> _inProgress;

    stdx::mutex _executorMutex;
    bool _isExecutorRunnable;
    stdx::condition_variable _isExecutorRunnableCondition;

    std::unique_ptr<ConnectionPool> _connPool;

    AtomicUInt64 _numOps;
};

}  // namespace executor
}  // namespace mongo
