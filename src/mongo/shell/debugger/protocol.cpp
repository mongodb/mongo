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

#include "mongo/shell/debugger/protocol.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"

namespace mongo {
namespace mozjs {
namespace debugger {
namespace protocol {


Message::Message(int seq) : seq(seq) {}

/**
 * Request
 */

Request::Request(const PartialRequest& partial)
    : Message(partial.seq), command(partial.command), arguments(partial.arguments) {}

PartialRequest::PartialRequest(int seq, BSONObj args, std::string command)
    : seq(seq), command(command), arguments(args) {}

std::shared_ptr<Request> Request::fromJSON(std::string line) {
    BSONObj obj = fromjson(line);

    auto seq = obj.getIntField("seq");
    auto command = std::string(toStdStringViewForInterop(obj.getStringField("command")));
    auto args = obj.getObjectField("arguments").getOwned();

    auto partial = PartialRequest(seq, args, command);

    if (command == SetBreakpointsRequest::COMMAND) {
        return std::make_shared<SetBreakpointsRequest>(partial);
    }
    if (command == ContinueRequest::COMMAND) {
        return std::make_shared<ContinueRequest>(partial);
    }
    if (command == StackTraceRequest::COMMAND) {
        return std::make_shared<StackTraceRequest>(partial);
    }
    if (command == ScopesRequest::COMMAND) {
        return std::make_shared<ScopesRequest>(partial);
    }
    if (command == VariablesRequest::COMMAND) {
        return std::make_shared<VariablesRequest>(partial);
    }
    if (command == ConfigurationDoneRequest::COMMAND) {
        return std::make_shared<ConfigurationDoneRequest>(partial);
    }

    // Null Object pattern
    return std::make_shared<UnknownRequest>(partial);
}

/**
 * ConfigurationDoneRequest
 */

ConfigurationDoneRequest::ConfigurationDoneRequest(const PartialRequest& partial)
    : VisitableRequest(partial) {}

Response ConfigurationDoneRequest::response() {
    return Response::Ack(*this);
}

/**
 * SetBreakpointsRequest
 */

SetBreakpointsRequest::SetBreakpointsRequest(const PartialRequest& partial)
    : VisitableRequest(partial) {
    // Get source from arguments
    source = std::string(toStdStringViewForInterop(arguments.getStringField("source")));

    // Get lines array from arguments and extract line numbers
    std::vector<BSONElement> linesArr = arguments.getField("lines").Array();
    for (const auto& lineElem : linesArr) {
        BSONObj lineObj = lineElem.Obj();
        int lineNum = lineObj.getIntField("line");
        lines.push_back(lineNum);
    }
}

Response SetBreakpointsRequest::response() {
    BSONObjBuilder responseBuilder;
    responseBuilder.append("type", "response");
    responseBuilder.append("seq", seq);

    BSONObjBuilder bodyBuilder;
    BSONArrayBuilder breakpointsArr;
    // Create a breakpoint response for each requested line
    for (size_t i = 0; i < lines.size(); ++i) {
        BSONObjBuilder bpBuilder;
        bpBuilder.append("id", static_cast<int>(i + 1));
        bpBuilder.append("verified", true);
        bpBuilder.append("line", lines[i]);
        bpBuilder.append("column", 0);
        breakpointsArr.append(bpBuilder.obj());
    }
    bodyBuilder.append("breakpoints", breakpointsArr.arr());

    responseBuilder.append("body", bodyBuilder.obj());

    Response response(seq, responseBuilder.obj().getOwned());
    return response;
}

/**
 * ContinueRequest
 */

ContinueRequest::ContinueRequest(const PartialRequest& partial) : VisitableRequest(partial) {}

Response ContinueRequest::response() {
    BSONObjBuilder responseBuilder;
    responseBuilder.append("type", "response");
    responseBuilder.append("seq", seq);
    responseBuilder.append("command", "continue");
    responseBuilder.append("success", true);

    Response response(seq, responseBuilder.obj().getOwned());
    return response;
}

/**
 * StackTraceRequest
 */

StackTraceRequest::StackTraceRequest(const PartialRequest& partial) : VisitableRequest(partial) {}

Response StackTraceRequest::response(std::string script, int line) {
    BSONObjBuilder responseBuilder;
    responseBuilder.append("type", "response");
    responseBuilder.append("seq", seq);

    BSONObjBuilder bodyBuilder;
    BSONArrayBuilder stackFramesArr;

    BSONObjBuilder frameBuilder;
    frameBuilder.append("id", 1);
    frameBuilder.append("name", script);

    BSONObjBuilder sourceBuilder;
    sourceBuilder.append("path", script);
    frameBuilder.append("source", sourceBuilder.obj());

    frameBuilder.append("line", line);
    frameBuilder.append("column", 0);

    stackFramesArr.append(frameBuilder.obj());
    bodyBuilder.append("stackFrames", stackFramesArr.arr());

    responseBuilder.append("body", bodyBuilder.obj());

    Response response(seq, responseBuilder.obj().getOwned());
    return response;
}


/**
 * ScopesRequest
 */

ScopesRequest::ScopesRequest(const PartialRequest& partial) : VisitableRequest(partial) {
    frameId = arguments.getIntField("frameId");
}

Response ScopesRequest::response(std::vector<Scope> scopes) {
    BSONObjBuilder responseBuilder;
    responseBuilder.append("type", "response");
    responseBuilder.append("seq", seq);

    BSONObjBuilder bodyBuilder;
    BSONArrayBuilder scopesArray;

    for (const auto& scope : scopes) {
        scopesArray.append(scope.toBSON());
    }

    bodyBuilder.append("scopes", scopesArray.arr());
    responseBuilder.append("body", bodyBuilder.obj());

    Response response(seq, responseBuilder.obj().getOwned());
    return response;
}

Scope::Scope(std::string name, int variablesReference, bool expensive)
    : name(name), variablesReference(variablesReference), expensive(expensive) {};

BSONObj Scope::toBSON() const {
    BSONObjBuilder obj;
    obj.append("name", name);
    obj.append("variablesReference", variablesReference);
    obj.append("expensive", expensive);
    return obj.obj();
}


/**
 * VariablesRequest
 */

VariablesRequest::VariablesRequest(const PartialRequest& partial) : VisitableRequest(partial) {
    variablesReference = arguments.getIntField("variablesReference");
}

Response VariablesRequest::response(std::vector<Variable> variables) {
    BSONObjBuilder responseBuilder;
    responseBuilder.append("type", "response");
    responseBuilder.append("seq", seq);

    BSONObjBuilder bodyBuilder;
    BSONArrayBuilder variablesArray;

    for (const auto& variable : variables) {
        variablesArray.append(variable.toBSON());
    }

    bodyBuilder.append("variables", variablesArray.arr());
    responseBuilder.append("body", bodyBuilder.obj());

    Response response(seq, responseBuilder.obj().getOwned());
    return response;
}

Variable::Variable(std::string name, std::string value, std::string type, int variablesReference)
    : name(name), value(value), type(type), variablesReference(variablesReference) {};

BSONObj Variable::toBSON() const {
    BSONObjBuilder obj;
    obj.append("name", name);
    obj.append("value", value);
    obj.append("type", type);
    obj.append("variablesReference", variablesReference);
    return obj.obj();
}

/**
 * UnknownRequest
 */
UnknownRequest::UnknownRequest(const PartialRequest& partial) : VisitableRequest(partial) {}

/**
 * StoppedEvent
 */

std::string StoppedEvent::getJson() const {
    BSONObjBuilder eventBuilder;
    eventBuilder.append("type", "event");
    eventBuilder.append("event", "stopped");

    BSONObjBuilder bodyBuilder;
    bodyBuilder.append("reason", "breakpoint");

    eventBuilder.append("body", bodyBuilder.obj());

    return eventBuilder.obj().jsonString(LegacyStrict);
}

/**
 * Response
 */

Response::Response(int seq, BSONObj obj) : Message(seq), bson(obj) {}

std::string Response::getJson() const {
    return bson.jsonString(LegacyStrict);
}

Response Response::Ack(Message msg) {
    BSONObjBuilder builder;
    builder.append("type", "response");
    builder.append("seq", msg.seq);
    builder.append("success", true);
    Response response(msg.seq, builder.obj());
    return response;
}

}  // namespace protocol
}  // namespace debugger
}  // namespace mozjs
}  // namespace mongo
