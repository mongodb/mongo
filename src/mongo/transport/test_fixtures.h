/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include <functional>
#include <memory>
#include <queue>

#include <boost/filesystem.hpp>

#include "mongo/db/dbmessage.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/net/ssl_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::test {

constexpr auto kLetKernelChoosePort = 0;

template <typename T>
class BlockingQueue {
public:
    void push(T t) {
        stdx::unique_lock lk(_mu);
        _q.push(std::move(t));
        lk.unlock();
        _cv.notify_one();
    }

    T pop() {
        stdx::unique_lock lk(_mu);
        _cv.wait(lk, [&] { return !_q.empty(); });
        T r = std::move(_q.front());
        _q.pop();
        return r;
    }

private:
    mutable stdx::mutex _mu;
    mutable stdx::condition_variable _cv;
    std::queue<T> _q;
};

using unittest::JoinThread;

struct SessionThread {
    struct StopException {};

    explicit SessionThread(std::shared_ptr<transport::Session> s)
        : _session{std::move(s)}, _thread{[this] {
              _run();
          }} {}

    ~SessionThread() {
        if (!_thread.joinable())
            return;
        schedule([](auto&&) { throw StopException{}; });
    }

    void schedule(std::function<void(transport::Session&)> task) {
        _tasks.push(std::move(task));
    }

    std::shared_ptr<transport::Session> session() const {
        return _session;
    }

private:
    void _run() {
        while (true) {
            try {
                LOGV2(6109508, "SessionThread: pop and execute a task");
                _tasks.pop()(*_session);
            } catch (const StopException&) {
                LOGV2(6109509, "SessionThread: stopping");
                return;
            }
        }
    }

    std::shared_ptr<transport::Session> _session;
    BlockingQueue<std::function<void(transport::Session&)>> _tasks;
    JoinThread _thread;  // Appears after the members _run uses.
};

class InlineReactor : public Reactor {
public:
    void run() noexcept override {}
    void stop() override {}

    void drain() override {
        MONGO_UNREACHABLE;
    }

    void schedule(Task t) override {
        t(Status::OK());
    }

    std::unique_ptr<ReactorTimer> makeTimer() override {
        MONGO_UNREACHABLE;
    }

    Date_t now() override {
        MONGO_UNREACHABLE;
    }

    void appendStats(BSONObjBuilder&) const override {
        MONGO_UNREACHABLE;
    }
};

class NoopReactor : public Reactor {
public:
    void run() noexcept override {}
    void stop() override {}

    void drain() override {
        MONGO_UNREACHABLE;
    }

    void schedule(Task) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<ReactorTimer> makeTimer() override {
        MONGO_UNREACHABLE;
    }

    Date_t now() override {
        MONGO_UNREACHABLE;
    }

    void appendStats(BSONObjBuilder&) const override {
        MONGO_UNREACHABLE;
    }
};

class TransportLayerMockWithReactor : public TransportLayerMock {
public:
    using TransportLayerMock::TransportLayerMock;

    ReactorHandle getReactor(WhichReactor) override {
        return _mockReactor;
    }

private:
    ReactorHandle _mockReactor = std::make_unique<NoopReactor>();
};

class MockSessionManager : public SessionManager {
public:
    MockSessionManager() = default;
    explicit MockSessionManager(std::function<void(SessionThread&)> onStartSession)
        : _onStartSession(std::move(onStartSession)) {}

    ~MockSessionManager() override {
        _join();
    }

    void startSession(std::shared_ptr<transport::Session> session) override {
        LOGV2(6109510, "Accepted connection", "remote"_attr = session->remote());
        auto& newSession = [&]() -> SessionThread& {
            auto vec = *_sessions;
            vec->push_back(std::make_unique<SessionThread>(std::move(session)));
            return *vec->back();
        }();
        if (_onStartSession)
            _onStartSession(newSession);
        LOGV2(6109511, "started session");
    }

    void endSessionByClient(Client* client) override {}

    void endAllSessions(Client::TagMask tags) override {
        _join();
    }

    bool shutdown(Milliseconds timeout) override {
        _join();
        return true;
    }

    std::size_t numOpenSessions() const override {
        return _sessions->size();
    }

    void setOnStartSession(std::function<void(SessionThread&)> cb) {
        _onStartSession = std::move(cb);
    }

private:
    void _join() {
        LOGV2(6109513, "Joining all session threads");
        _sessions->clear();
    }

    std::function<void(SessionThread&)> _onStartSession;
    synchronized_value<std::vector<std::unique_ptr<SessionThread>>> _sessions;
};

class ServiceEntryPointUnimplemented : public ServiceEntryPoint {
public:
    ServiceEntryPointUnimplemented() = default;

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request) noexcept override {
        MONGO_UNREACHABLE;
    }
};

class TempCertificatesDir {
public:
    TempCertificatesDir(std::string directoryPrefix) {
        _dir = std::make_unique<unittest::TempDir>(directoryPrefix + "_certs_test");
        boost::filesystem::path directoryPath(_dir->path());
        boost::filesystem::path filePathCA(directoryPath / "ca.pem");
        boost::filesystem::path filePathPEM(directoryPath / "server_pem.pem");
        _filePathCA = filePathCA.string();
        _filePathPEM = filePathPEM.string();
    }

    StringData getCAFile() const {
        return _filePathPEM;
    }

    StringData getPEMKeyFile() const {
        return _filePathCA;
    }

private:
    std::unique_ptr<unittest::TempDir> _dir;
    std::string _filePathCA;
    std::string _filePathPEM;
};

/**
 * Creates a temporary directory and copies the certificates at the provided filepaths into two new
 * files in the temporary directory, which the caller can access through TempCertificatesDir. Allows
 * tests to modify certificate contents mid-test to mimic the actions taken by a user when they call
 * rotateCertificates.
 */
inline std::unique_ptr<TempCertificatesDir> copyCertsToTempDir(std::string caFile,
                                                               std::string pemFile,
                                                               std::string directoryPrefix) {
    auto tempDir = std::make_unique<TempCertificatesDir>(directoryPrefix);

    boost::filesystem::copy_file(caFile, tempDir->getCAFile().toString());
    boost::filesystem::copy_file(pemFile, tempDir->getPEMKeyFile().toString());

    return tempDir;
};

/**
 * RAII type that caches the sslGlobalParams sslCAFile, sslPEMKeyFile, and sslMode on construction,
 * and restores them to the cached values on destruction.
 */
class SSLGlobalParamsGuard {
public:
    SSLGlobalParamsGuard() {
        _sslCAFile = sslGlobalParams.sslCAFile;
        _sslPEMKeyFile = sslGlobalParams.sslPEMKeyFile;
        _sslMode = sslGlobalParams.sslMode.load();
    }

    ~SSLGlobalParamsGuard() {
        sslGlobalParams.sslCAFile = _sslCAFile;
        sslGlobalParams.sslPEMKeyFile = _sslPEMKeyFile;
        sslGlobalParams.sslMode.store(_sslMode);
    }

private:
    std::string _sslCAFile;
    std::string _sslPEMKeyFile;
    int _sslMode;
};

}  // namespace mongo::transport::test

#undef MONGO_LOGV2_DEFAULT_COMPONENT
