// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/legacy_reply_builder.h"

#include "mongo/db/dbmessage.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo {
namespace rpc {

LegacyReplyBuilder::LegacyReplyBuilder() : LegacyReplyBuilder(Message()) {}

LegacyReplyBuilder::LegacyReplyBuilder(Message&& message) : _message{std::move(message)} {
    _builder.skip(sizeof(QueryResult::Value));
}

LegacyReplyBuilder::~LegacyReplyBuilder() {}

LegacyReplyBuilder& LegacyReplyBuilder::setCommandReply(Status nonOKStatus,
                                                        BSONObj extraErrorInfo) {
    invariant(!_haveCommandReply);
    ReplyBuilderInterface::setCommandReply(std::move(nonOKStatus), std::move(extraErrorInfo));
    return *this;
}

LegacyReplyBuilder& LegacyReplyBuilder::setRawCommandReply(const BSONObj& commandReply) {
    invariant(!_haveCommandReply);
    _bodyOffset = _builder.len();
    commandReply.appendSelfToBufBuilder(_builder);
    _haveCommandReply = true;
    return *this;
}

BSONObjBuilder LegacyReplyBuilder::getBodyBuilder() {
    if (!_haveCommandReply) {
        auto bob = BSONObjBuilder(_builder);
        _bodyOffset = bob.offset();
        _haveCommandReply = true;
        return bob;
    }

    invariant(_bodyOffset);
    return BSONObjBuilder(BSONObjBuilder::ResumeBuildingTag{}, _builder, _bodyOffset);
}


Protocol LegacyReplyBuilder::getProtocol() const {
    return rpc::Protocol::kOpQuery;
}

void LegacyReplyBuilder::reserveBytes(const std::size_t bytes) {
    _builder.reserveBytes(bytes);
    _builder.claimReservedBytes(bytes);
}

void LegacyReplyBuilder::reset() {
    // If we are in State::kMetadata, we are already in the 'start' state, so by
    // immediately returning, we save a heap allocation.
    if (!_haveCommandReply) {
        return;
    }
    _builder.reset();
    _builder.skip(sizeof(QueryResult::Value));
    _message.reset();
    _haveCommandReply = false;
    _bodyOffset = 0;
}


Message LegacyReplyBuilder::done() {
    invariant(_haveCommandReply);

    QueryResult::View qr = _builder.buf();
    qr.msgdata().setLen(_builder.len());
    qr.msgdata().setOperation(opReply);
    qr.setResultFlagsToOk();
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(1);

    _message.setData(_builder.release());

    return std::move(_message);
}

}  // namespace rpc
}  // namespace mongo
