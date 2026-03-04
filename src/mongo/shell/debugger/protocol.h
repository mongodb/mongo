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

#include "mongo/bson/bsonobj.h"

#include <string>
#include <string_view>

namespace mongo {
namespace mozjs {
namespace debugger {
namespace protocol {


// Forward declarations
class PartialRequest;
class ConfigurationDoneRequest;
class SetBreakpointsRequest;
class ContinueRequest;
class StackTraceRequest;
class ScopesRequest;
class VariablesRequest;
class UnknownRequest;


// Visitor interface
class RequestHandler {
public:
    virtual ~RequestHandler() = default;
    virtual void handleRequest(ConfigurationDoneRequest& req) = 0;
    virtual void handleRequest(SetBreakpointsRequest& req) = 0;
    virtual void handleRequest(ContinueRequest& req) = 0;
    virtual void handleRequest(StackTraceRequest& req) = 0;
    virtual void handleRequest(ScopesRequest& req) = 0;
    virtual void handleRequest(VariablesRequest& req) = 0;
    virtual void handleRequest(UnknownRequest& req) = 0;
};

/**
 * Support for the Debug Adapter Protocol.
 * https://microsoft.github.io/debug-adapter-protocol//specification.html
 *
 * This is incomplete (does not implement the full spec) and supports only those with concrete use
 * cases.
 */

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Base_Protocol_ProtocolMessage
class Message {
public:
    int seq;
    Message(int seq) : seq(seq) {};
};

class PartialRequest {
public:
    int seq;
    std::string command;
    BSONObj arguments;
    PartialRequest(int seq, BSONObj args, std::string command)
        : seq(seq), command(command), arguments(args) {};
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Base_Protocol_Request
class Request : public Message {
public:
    std::string command;
    BSONObj arguments;

    virtual void handleRequest(RequestHandler& visitor) = 0;
    Request(const PartialRequest& partial)
        : Message(partial.seq), command(partial.command), arguments(partial.arguments) {};

    static std::shared_ptr<Request> fromJSON(std::string json);
    virtual ~Request() = default;
};

/**
 * Visitor pattern to use double-dispatch on the matching request type
 */
template <typename Derived>
class VisitableRequest : public Request {
public:
    VisitableRequest(const PartialRequest& partial) : Request(partial) {};
    void handleRequest(RequestHandler& visitor) override {
        visitor.handleRequest(static_cast<Derived&>(*this));
    }
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Base_Protocol_Response
class Response : public Message {
public:
    const BSONObj bson;
    Response(int seq, BSONObj obj) : Message(seq), bson(obj) {}
    std::string getJson() const;

    static Response Ack(Message msg);
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Requests_ConfigurationDone
class ConfigurationDoneRequest : public VisitableRequest<ConfigurationDoneRequest> {
public:
    inline static constexpr std::string_view COMMAND = "configurationDone";
    ConfigurationDoneRequest(const PartialRequest& partial) : VisitableRequest(partial) {};
    Response response();
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Requests_SetBreakpoints
class SetBreakpointsRequest : public VisitableRequest<SetBreakpointsRequest> {
public:
    inline static constexpr std::string_view COMMAND = "setBreakpoints";
    std::string source;
    std::vector<int> lines;

    SetBreakpointsRequest(const PartialRequest& partial);
    Response response();
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Requests_Continue
class ContinueRequest : public VisitableRequest<ContinueRequest> {
public:
    inline static constexpr std::string_view COMMAND = "continue";
    ContinueRequest(const PartialRequest& partial) : VisitableRequest(partial) {};
    Response response();
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Requests_StackTrace
class StackTraceRequest : public VisitableRequest<StackTraceRequest> {
public:
    inline static constexpr std::string_view COMMAND = "stackTrace";
    StackTraceRequest(const PartialRequest& partial) : VisitableRequest(partial) {};
    Response response(std::string script, int line);
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Types_Scope
class Scope {
    std::string name;
    int variablesReference;
    bool expensive;

public:
    Scope(std::string name, int variablesReference, bool expensive)
        : name(name), variablesReference(variablesReference), expensive(expensive) {};
    BSONObj toBSON() const;
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Requests_Scopes
class ScopesRequest : public VisitableRequest<ScopesRequest> {
public:
    inline static constexpr std::string_view COMMAND = "scopes";
    int frameId;

    ScopesRequest(const PartialRequest& partial);
    Response response(std::vector<Scope> scopes);
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Types_Variable
class Variable {
    std::string name;
    std::string value;
    std::string type;
    int variablesReference;

public:
    Variable(std::string name, std::string value, std::string type, int variablesReference)
        : name(name), value(value), type(type), variablesReference(variablesReference) {};
    BSONObj toBSON() const;
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Requests_Variables
class VariablesRequest : public VisitableRequest<VariablesRequest> {
public:
    inline static constexpr std::string_view COMMAND = "variables";
    int variablesReference;

    VariablesRequest(const PartialRequest& partial);
    Response response(std::vector<Variable> variables);
};

// Not technically in the spec, but allows us to use a Null Object pattern on non-matches
class UnknownRequest : public VisitableRequest<UnknownRequest> {
public:
    UnknownRequest(const PartialRequest& partial) : VisitableRequest(partial) {};
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Base_Protocol_Event
class Event {
public:
    virtual std::string getJson() const = 0;
};

// https://microsoft.github.io/debug-adapter-protocol//specification.html#Events_Stopped
class StoppedEvent : public Event {
public:
    std::string getJson() const override;
};

}  // namespace protocol
}  // namespace debugger
}  // namespace mozjs
}  // namespace mongo
