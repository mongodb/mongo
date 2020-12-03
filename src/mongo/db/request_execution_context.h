/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <memory>

#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class Command;

/**
 * Captures the execution context for a command to allow safe, shared and asynchronous accesses.
 * This class owns all objects that participate in command execution (e.g., `request`). The only
 * exceptions are `opCtx` and `command`. The `opCtx` remains valid so long as its corresponding
 * client is attached to the executor thread. In case of `command`, it is a global, static
 * construct and is safe to be accessed through raw pointers.
 * Any access from a client thread that does not own the `opCtx`, or after the `opCtx` is
 * released is strictly forbidden.
 */
class RequestExecutionContext {
public:
    RequestExecutionContext() = delete;
    RequestExecutionContext(const RequestExecutionContext&) = delete;
    RequestExecutionContext& operator=(const RequestExecutionContext&) = delete;
    RequestExecutionContext(RequestExecutionContext&&) = delete;
    RequestExecutionContext& operator=(RequestExecutionContext&&) = delete;

    RequestExecutionContext(OperationContext* opCtx, Message message)
        : _opCtx(opCtx),
          _message(std::move(message)),
          _dbmsg(std::make_unique<DbMessage>(_message.get())) {}

    auto getOpCtx() const {
        invariant(_isOnClientThread());
        return _opCtx;
    }

    const Message& getMessage() const {
        invariant(_isOnClientThread() && _message);
        return _message.get();
    }

    DbMessage& getDbMessage() const {
        invariant(_isOnClientThread() && _dbmsg);
        return *_dbmsg.get();
    }

    void setRequest(OpMsgRequest request) {
        invariant(_isOnClientThread() && !_request);
        _request = std::move(request);
    }
    const OpMsgRequest& getRequest() const {
        invariant(_isOnClientThread() && _request);
        return _request.get();
    }

    void setCommand(Command* command) {
        invariant(_isOnClientThread() && !_command);
        _command = command;
    }
    Command* getCommand() const {
        invariant(_isOnClientThread());
        return _command;
    }

    void setReplyBuilder(std::unique_ptr<rpc::ReplyBuilderInterface> replyBuilder) {
        invariant(_isOnClientThread() && !_replyBuilder);
        _replyBuilder = std::move(replyBuilder);
    }
    auto getReplyBuilder() const {
        invariant(_isOnClientThread() && _replyBuilder);
        return _replyBuilder.get();
    }

    void setResponse(DbResponse response) {
        invariant(_isOnClientThread());
        _response = std::move(response);
    }
    DbResponse& getResponse() {
        invariant(_isOnClientThread());
        return _response;
    }

private:
    bool _isOnClientThread() const {
        return _opCtx != nullptr && Client::getCurrent() == _opCtx->getClient();
    }

    OperationContext* const _opCtx;
    boost::optional<Message> _message;
    std::unique_ptr<DbMessage> _dbmsg;
    boost::optional<OpMsgRequest> _request;
    Command* _command = nullptr;
    std::unique_ptr<rpc::ReplyBuilderInterface> _replyBuilder;
    DbResponse _response;
};

}  // namespace mongo
