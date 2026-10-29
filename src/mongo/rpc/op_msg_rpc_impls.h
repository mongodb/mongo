// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] rpc {

class OpMsgReply final : public rpc::ReplyInterface {
public:
    explicit OpMsgReply(const Message* message) : _msg(OpMsg::parseOwned(*message)) {}
    explicit OpMsgReply(OpMsg msg) : _msg(std::move(msg)) {}
    const BSONObj& getCommandReply() const override {
        return _msg.body;
    }
    rpc::Protocol getProtocol() const override {
        return rpc::Protocol::kOpMsg;
    }

private:
    OpMsg _msg;
};

class OpMsgReplyBuilder final : public rpc::ReplyBuilderInterface {
public:
    ReplyBuilderInterface& setRawCommandReply(const BSONObj& reply) override {
        _builder.beginBody().appendElements(reply);
        return *this;
    }
    BSONObjBuilder getBodyBuilder() override {
        if (!_builder.isBuildingBody()) {
            return _builder.beginBody();
        }
        return _builder.resumeBody();
    }
    OpMsgBuilder::DocSequenceBuilder getDocSequenceBuilder(std::string_view name) override {
        return _builder.beginDocSequence(name);
    }
    rpc::Protocol getProtocol() const override {
        return rpc::Protocol::kOpMsg;
    }
    void reset() override {
        _builder.reset();
    }
    Message done() override {
        return _builder.finish();
    }
    void reserveBytes(const std::size_t bytes) override {
        _builder.reserveBytes(bytes);
    }
    BSONObj releaseBody() {
        return _builder.releaseBody();
    }

private:
    OpMsgBuilder _builder;
};

}  // namespace rpc
}  // namespace mongo
