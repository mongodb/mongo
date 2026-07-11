// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/shell/debugger/protocol.h"

#include <string>

namespace mongo {
namespace mozjs {
namespace debugger {

using namespace protocol;

class DebugAdapter : public RequestHandler {

private:
    static void sendMessage(std::string json);
    static void sendMessage(const Response& response);
    static void sendMessage(const Event& event);

public:
    static Status connect();
    static void disconnect();

    static void handleMessagesThread();

    // Wait for the debugger to be configured before continuing execution.
    // Returns true if connected to a DAP client.
    static bool waitForHandshake();

    // Send a StoppedEvent to the client
    static void sendPause();
    static void sendStoppedOnException(std::string text);

    // RequestHandler visitors
    void handleRequest(ConfigurationDoneRequest& request) override;
    void handleRequest(SetBreakpointsRequest& request) override;
    void handleRequest(ContinueRequest& request) override;
    void handleRequest(StackTraceRequest& request) override;
    void handleRequest(ScopesRequest& request) override;
    void handleRequest(VariablesRequest& request) override;
    void handleRequest(EvaluateRequest& request) override;
    void handleRequest(SetVariableRequest& request) override;
    void handleRequest(UnknownRequest& request) override;
};


}  // namespace debugger
}  // namespace mozjs
}  // namespace mongo
