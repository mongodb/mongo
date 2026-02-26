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

public:
    static Status connect();
    static void disconnect();
    static void sendMessage(std::string json);
    static void sendMessage(const Response& response);
    static void sendMessage(const Event& event);

    static void handleMessagesThread();

    // Wait for the debugger to be configured before continuing execution
    static void waitForHandshake();

    static void sendPause();

    // visitors
    void handleRequest(SetBreakpointsRequest& request) override;
    void handleRequest(ContinueRequest& request) override;
    void handleRequest(StackTraceRequest& request) override;
    void handleRequest(UnknownRequest& request) override;
};


}  // namespace debugger
}  // namespace mozjs
}  // namespace mongo
