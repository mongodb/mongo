/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/shell/debugger/adapter.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/shell/debugger/debugger.h"
#include "mongo/stdx/condition_variable.h"

#include <mutex>

#include <boost/filesystem.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace mongo {
namespace mozjs {
namespace debugger {

constexpr int INVALID_SOCKET_VALUE = -1;

// Network state
int _clientSocket = INVALID_SOCKET_VALUE;
AtomicWord<bool> _running{false};
std::unique_ptr<std::thread> _messageThread;

// Configuration state
std::mutex _configMutex;
stdx::condition_variable _configCV;
static AtomicWord<bool> _configured{false};


/**
 * Path helpers
 *
 * VSCode uses absolute path.
 * The shell uses relative path.
 */

// Assume that the user is working from their mongo repo root.
// It would be more elegant to use something like ModuleLoader::resolveBaseUrl in the future.
const boost::filesystem::path MONGO_ROOT = boost::filesystem::current_path();

std::string abs2rel(std::string absPath) {
    boost::filesystem::path absolute(absPath);
    return boost::filesystem::relative(absolute, MONGO_ROOT).string();
}

std::string rel2abs(std::string relPath) {
    boost::filesystem::path relative(relPath);
    return boost::filesystem::absolute(relative, MONGO_ROOT).string();
}

/**
 * DebugAdapter
 */

void DebugAdapter::handleRequest(ConfigurationDoneRequest& request) {
    std::lock_guard<std::mutex> lock(_configMutex);
    _configured.store(true);
    _configCV.notify_all();

    sendMessage(request.response());
}

void DebugAdapter::handleRequest(SetBreakpointsRequest& request) {
    request.source = abs2rel(request.source);
    DebuggerGlobal::setBreakpoints(request);
    request.source = rel2abs(request.source);
    sendMessage(request.response());
}

void DebugAdapter::handleRequest(ContinueRequest& request) {
    DebuggerGlobal::unpause();
    sendMessage(request.response());
}

void DebugAdapter::handleRequest(StackTraceRequest& request) {
    auto frames = DebuggerGlobal::getStackFrames();

    for (auto& frame : frames) {
        frame.source = rel2abs(frame.source);
        // some built-in scripts have absolute paths as their "name", so convert them to relative
        // for the client
        frame.name = abs2rel(frame.name);
    }
    auto response = request.response(frames);
    sendMessage(response);
}

void DebugAdapter::handleRequest(ScopesRequest& request) {
    auto scopes = DebuggerGlobal::getScopes(request.frameId);
    auto response = request.response(scopes);
    sendMessage(response);
}

void DebugAdapter::handleRequest(VariablesRequest& request) {
    auto variables = DebuggerGlobal::getVariables(request.variablesReference);
    auto response = request.response(variables);
    sendMessage(response);
}

void DebugAdapter::handleRequest(EvaluateRequest& request) {
    std::string result = DebuggerGlobal::evaluate(request);
    auto response = request.response(result);
    sendMessage(response);
}

void DebugAdapter::handleRequest(SetVariableRequest& request) {
    std::string value = DebuggerGlobal::setVariable(request);
    auto response = request.response(value);
    sendMessage(response);
}

void DebugAdapter::handleRequest(UnknownRequest& request) {
    std::cerr << "Unknown request for command '" << request.command << "'" << std::endl;
}

bool DebugAdapter::waitForHandshake() {
    // The DAP should already be running, which is probing the port for when to attach to a new
    // shell. The shell can come up and start running tests before the DAP gets to relay any pending
    // breakpoints. We need to wait for the DAP to sync up, and then continue.

    std::unique_lock<std::mutex> lock(_configMutex);
    return _configCV.wait_for(
        lock, std::chrono::milliseconds(100), [] { return _configured.load(); });
}

void DebugAdapter::sendPause() {
    auto e = StoppedEvent::Breakpoint();
    sendMessage(e);
}

void DebugAdapter::sendStoppedOnException(std::string text) {
    auto e = StoppedEvent::Exception(text);
    sendMessage(e);
}

void DebugAdapter::sendMessage(std::string json) {
    if (_clientSocket < 0)
        return;
    json += '\n';
    send(_clientSocket, json.c_str(), json.length(), 0);
}

void DebugAdapter::sendMessage(const Response& response) {
    sendMessage(response.getJson());
}

void DebugAdapter::sendMessage(const Event& event) {
    sendMessage(event.getJson());
}

void DebugAdapter::handleMessagesThread() {
    char buffer[4096];
    std::string msgBuffer;
    DebugAdapter adapter;

    while (_running.load() && _clientSocket >= 0) {
        auto n = recv(_clientSocket, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;  // Connection closed or error
        }

        msgBuffer.append(buffer, n);

        // Process complete messages (newline-delimited JSON)
        size_t pos;
        while ((pos = msgBuffer.find('\n')) != std::string::npos) {
            std::string line = msgBuffer.substr(0, pos);
            msgBuffer.erase(0, pos + 1);

            if (!line.empty()) {
                // Parse and handle message
                try {
                    auto req = Request::fromJSON(line);
                    req->handleRequest(adapter);  // double-dispatch
                } catch (const std::exception& e) {
                    std::cout << "[ERROR] Failed to parse JSON: " << e.what() << "\nLine: " << line
                              << std::endl;
                }
            }
        }
    }

    DebugAdapter::disconnect();
}

Status DebugAdapter::connect() {
#ifdef _WIN32
    // Initialize Winsock on Windows
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to initialize Winsock");
    }
#endif

    // Create CLIENT socket (connecting to debug adapter)
    _clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (_clientSocket < 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to create client socket");
    }

    // Set up address to connect to debug adapter client
    int port = 9229;
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(_clientSocket, (sockaddr*)&addr, sizeof(addr)) == 0) {
        std::cout << "Shell connected to debug adapter client on port " << port << std::endl;
        _running.store(true);
        _messageThread = std::make_unique<std::thread>(handleMessagesThread);
    }

    return Status::OK();
}

void DebugAdapter::disconnect() {
    if (_clientSocket >= 0) {
#ifdef _WIN32
        closesocket(_clientSocket);
        WSACleanup();
#else
        close(_clientSocket);
#endif
    }
    _clientSocket = INVALID_SOCKET_VALUE;
}

}  // namespace debugger
}  // namespace mozjs
}  // namespace mongo
