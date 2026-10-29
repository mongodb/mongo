// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

namespace mongo {
namespace rpc {

class [[MONGO_MOD_PUBLIC]] LegacyReplyBuilder final : public ReplyBuilderInterface {
public:
    static const char kCursorTag[];
    static const char kFirstBatchTag[];

    LegacyReplyBuilder();
    LegacyReplyBuilder(Message&&);
    ~LegacyReplyBuilder() final;

    // Override of setCommandReply specifically used to handle StaleConfig errors
    LegacyReplyBuilder& setCommandReply(Status nonOKStatus, BSONObj extraErrorInfo) final;
    LegacyReplyBuilder& setRawCommandReply(const BSONObj& commandReply) final;

    BSONObjBuilder getBodyBuilder() final;

    void reset() final;

    Message done() final;

    Protocol getProtocol() const final;

    void reserveBytes(std::size_t bytes) final;

private:
    BufBuilder _builder;
    std::size_t _bodyOffset = 0;
    Message _message;
    bool _haveCommandReply = false;
};

}  // namespace rpc
}  // namespace mongo
