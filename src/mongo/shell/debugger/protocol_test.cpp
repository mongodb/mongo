/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/shell/debugger/protocol.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace mozjs {
namespace debugger {
namespace protocol {

TEST(Request, fromJSON) {

    std::string json =
        R"({"type":"request","seq":17,"command":"setBreakpoints","arguments":{"source":"","lines":[]}})";
    auto req = Request::fromJSON(json);
    ASSERT_EQ(req->seq, 17);
    ASSERT_EQ(req->command, "setBreakpoints");
}

TEST(SetBreakpointsRequest, parseRequestAndResponse) {

    std::string json =
        R"({"type":"request","seq":17,"command":"setBreakpoints","arguments":{"source":"jstests/my_test.js","lines":[{"line":5},{"line":6}]}})";
    auto request = std::static_pointer_cast<SetBreakpointsRequest>(Request::fromJSON(json));
    ASSERT_EQ(request->seq, 17);
    ASSERT_EQ(request->source, "jstests/my_test.js");
    ASSERT_EQ(request->lines, std::vector<int>({5, 6}));

    auto response = request->response();
    std::string expectedResponse =
        R"({ "type" : "response", "seq" : 17, "body" : { "breakpoints" : [ { "id" : "1", "verified" : true, "line" : "12", "column" : "0" } ] } })";
    ASSERT_EQ(response.getJson(), expectedResponse);
}


TEST(StackTraceRequest, parseRequestAndResponse) {

    std::string json = R"({"type":"request","seq":17,"command":"stackTrace","arguments":{}})";
    auto request = std::static_pointer_cast<StackTraceRequest>(Request::fromJSON(json));
    ASSERT_EQ(request->seq, 17);

    auto response = request->response("myScript", 32);
    std::string expectedResponse =
        R"({ "type" : "response", "seq" : 17, "body" : { "stackFrames" : [ { "id" : 1, "name" : "myScript", "source" : { "path" : "myScript" }, "line" : 32, "column" : 0 } ] } })";
    ASSERT_EQ(response.getJson(), expectedResponse);
}

TEST(ContinueRequest, parseRequestAndResponse) {

    std::string json = R"({"type":"request","seq":17,"command":"continue","arguments":{}})";
    auto request = std::static_pointer_cast<ContinueRequest>(Request::fromJSON(json));
    ASSERT_EQ(request->seq, 17);

    auto response = request->response();
    std::string expectedResponse =
        R"({ "type" : "response", "seq" : 17, "command" : "continue", "success" : true })";
    ASSERT_EQ(response.getJson(), expectedResponse);
}

TEST(StoppedEvent, parseEvent) {

    auto event = StoppedEvent();
    std::string expectedJson =
        R"({ "type" : "event", "event" : "stopped", "body" : { "reason" : "breakpoint" } })";
    ASSERT_EQ(event.getJson(), expectedJson);
}

}  // namespace protocol
}  // namespace debugger
}  // namespace mozjs
}  // namespace mongo
