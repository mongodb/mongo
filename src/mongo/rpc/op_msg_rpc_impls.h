/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/reply_interface.h"

namespace mongo {
namespace rpc {

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
    OpMsgBuilder::DocSequenceBuilder getDocSequenceBuilder(StringData name) override {
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
