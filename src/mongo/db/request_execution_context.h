// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>

[[MONGO_MOD_PUBLIC]];

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
class [[MONGO_MOD_OPEN]] RequestExecutionContext {
public:
    RequestExecutionContext() = delete;
    RequestExecutionContext(const RequestExecutionContext&) = delete;
    RequestExecutionContext& operator=(const RequestExecutionContext&) = delete;
    RequestExecutionContext(RequestExecutionContext&&) = delete;
    RequestExecutionContext& operator=(RequestExecutionContext&&) = delete;

    RequestExecutionContext(OperationContext* opCtx, Message message, Date_t started)
        : _opCtx(opCtx),
          _message(std::move(message)),
          _dbmsg(std::make_unique<DbMessage>(_message.get())),
          _started(started) {}

    auto getOpCtx() const {
        dassert(_isOnClientThread());
        return _opCtx;
    }

    Date_t getStarted() const {
        return _started;
    }

    const Message& getMessage() const {
        dassert(_isOnClientThread() && _message);
        return _message.get();
    }

    DbMessage& getDbMessage() const {
        dassert(_isOnClientThread() && _dbmsg);
        return *_dbmsg.get();
    }

    void setRequest(OpMsgRequest request) {
        dassert(_isOnClientThread() && !_request);
        _request = std::move(request);
    }
    const OpMsgRequest& getRequest() const {
        dassert(_isOnClientThread() && _request);
        return _request.get();
    }

    bool hasOtelSpan() const {
        dassert(_isOnClientThread());
        return _otelSpan.has_value();
    }
    void setOtelSpan(otel::traces::Span otelSpan) {
        dassert(_isOnClientThread() && !_otelSpan);
        _otelSpan = std::move(otelSpan);
    }
    const otel::traces::Span& getOtelSpan() const {
        dassert(_isOnClientThread() && _otelSpan);
        return *_otelSpan;
    }
    otel::traces::Span& getOtelSpan() {
        dassert(_isOnClientThread() && _otelSpan);
        return *_otelSpan;
    }

    void setCommand(Command* command) {
        dassert(_isOnClientThread() && !_command);
        _command = command;
    }
    Command* getCommand() const {
        dassert(_isOnClientThread());
        return _command;
    }

    void setReplyBuilder(std::unique_ptr<rpc::ReplyBuilderInterface> replyBuilder) {
        dassert(_isOnClientThread() && !_replyBuilder);
        _replyBuilder = std::move(replyBuilder);
    }
    auto getReplyBuilder() const {
        dassert(_isOnClientThread() && _replyBuilder);
        return _replyBuilder.get();
    }

private:
    bool _isOnClientThread() const {
        return _opCtx != nullptr && Client::getCurrent() == _opCtx->getClient();
    }

    OperationContext* const _opCtx;
    boost::optional<Message> _message;
    std::unique_ptr<DbMessage> _dbmsg;
    const Date_t _started;
    boost::optional<OpMsgRequest> _request;
    Command* _command = nullptr;
    std::unique_ptr<rpc::ReplyBuilderInterface> _replyBuilder;
    boost::optional<otel::traces::Span> _otelSpan;
};

}  // namespace mongo
