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

TEST(Message, AckResponse) {
    std::string json = R"({"type":"request","seq":7,"command":"configurationDone","arguments":{}})";
    auto request = Request::fromJSON(json);
    ASSERT_EQ(request->seq, 7);

    auto response = Response::Ack(*request);
    std::string expectedResponse = R"({ "type" : "response", "seq" : 7, "success" : true })";
    ASSERT_EQ(response.getJson(), expectedResponse);
}

TEST(SetBreakpointsRequest, parseRequestAndResponse) {

    std::string json =
        R"({"type":"request","seq":17,"command":"setBreakpoints","arguments":{"source":"jstests/my_test.js","lines":[{"line":5},{"line":82}]}})";
    auto request = std::static_pointer_cast<SetBreakpointsRequest>(Request::fromJSON(json));
    ASSERT_EQ(request->seq, 17);
    ASSERT_EQ(request->source, "jstests/my_test.js");
    ASSERT_EQ(request->lines, std::vector<int>({5, 82}));

    auto response = request->response();
    std::string expectedResponse =
        R"({ "type" : "response", "seq" : 17, "body" : { "breakpoints" : [ { "id" : 1, "verified" : true, "line" : 5, "column" : 0, "source" : { "path" : "jstests/my_test.js" } }, { "id" : 2, "verified" : true, "line" : 82, "column" : 0, "source" : { "path" : "jstests/my_test.js" } } ] } })";
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

TEST(ScopesRequest, parseRequestAndResponse) {

    std::string json =
        R"({"type":"request","seq":3,"command":"scopes","arguments":{"frameId":100}})";
    auto request = std::static_pointer_cast<ScopesRequest>(Request::fromJSON(json));
    ASSERT_EQ(request->seq, 3);
    ASSERT_EQ(request->frameId, 100);

    std::vector<Scope> stubScopes = {
        Scope("Local", 99, false), Scope("Closure", 1, false), Scope("Global", 67, true)};
    auto response = request->response(stubScopes);
    std::string expectedResponse =
        R"({ "type" : "response", "seq" : 3, "body" : { "scopes" : [ { "name" : "Local", "variablesReference" : 99, "expensive" : false }, { "name" : "Closure", "variablesReference" : 1, "expensive" : false }, { "name" : "Global", "variablesReference" : 67, "expensive" : true } ] } })";
    ASSERT_EQ(response.getJson(), expectedResponse);
}

TEST(VariablesRequest, parseRequestAndResponse) {

    std::string json =
        R"({"type":"request","seq":4,"command":"variables","arguments":{"variablesReference":1001}})";
    auto request = std::static_pointer_cast<VariablesRequest>(Request::fromJSON(json));
    ASSERT_EQ(request->seq, 4);
    ASSERT_EQ(request->variablesReference, 1001);

    std::vector<Variable> stubVariables = {Variable("x", "42", "number", 99),
                                           Variable("name", "John", "string", 1),
                                           Variable("isActive", "true", "boolean", 5),
                                           Variable("myObj", "Object {...}", "object", 67)};

    auto response = request->response(stubVariables);
    std::string expectedResponse =
        R"({ "type" : "response", "seq" : 4, "body" : { "variables" : [ { "name" : "x", "value" : "42", "type" : "number", "variablesReference" : 99 }, { "name" : "name", "value" : "John", "type" : "string", "variablesReference" : 1 }, { "name" : "isActive", "value" : "true", "type" : "boolean", "variablesReference" : 5 }, { "name" : "myObj", "value" : "Object {...}", "type" : "object", "variablesReference" : 67 } ] } })";
    ASSERT_EQ(response.getJson(), expectedResponse);
}

TEST(StoppedEvent, parseEvent) {

    auto event = StoppedEvent();
    std::string expectedJson =
        R"({ "type" : "event", "event" : "stopped", "body" : { "reason" : "breakpoint" } })";
    ASSERT_EQ(event.getJson(), expectedJson);
}

TEST(ConfigurationDoneRequest, parseRequestAndResponse) {
    std::string json =
        R"({"type":"request","seq":12,"command":"configurationDone","arguments":{}})";

    auto request = std::static_pointer_cast<ConfigurationDoneRequest>(Request::fromJSON(json));
    ASSERT_EQ(request->seq, 12);

    auto response = request->response();
    std::string expectedResponse = R"({ "type" : "response", "seq" : 12, "success" : true })";
    ASSERT_EQ(response.getJson(), expectedResponse);
}

}  // namespace protocol
}  // namespace debugger
}  // namespace mozjs
}  // namespace mongo
