/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/rpc/legacy_reply_builder.h"

#include <iterator>

#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace rpc {

LegacyReplyBuilder::LegacyReplyBuilder() : LegacyReplyBuilder(Message()) {}

LegacyReplyBuilder::LegacyReplyBuilder(Message&& message) : _message{std::move(message)} {
    _builder.skip(sizeof(QueryResult::Value));
}

LegacyReplyBuilder::~LegacyReplyBuilder() {}

LegacyReplyBuilder& LegacyReplyBuilder::setCommandReply(Status nonOKStatus,
                                                        const BSONObj& extraErrorInfo) {
    invariant(_state == State::kCommandReply);
    if (nonOKStatus == ErrorCodes::SendStaleConfig) {
        _staleConfigError = true;
        // Need to use the special $err format for SendStaleConfig errors to be backwards
        // compatible.
        BSONObjBuilder err;
        // $err must be the first field in object.
        err.append("$err", nonOKStatus.reason());
        err.append("code", nonOKStatus.code());
        err.appendElements(extraErrorInfo);
        setRawCommandReply(err.done());
    } else {
        // All other errors proceed through the normal path, which also handles state transitions.
        ReplyBuilderInterface::setCommandReply(std::move(nonOKStatus), extraErrorInfo);
    }
    return *this;
}

LegacyReplyBuilder& LegacyReplyBuilder::setRawCommandReply(const BSONObj& commandReply) {
    invariant(_state == State::kCommandReply);
    commandReply.appendSelfToBufBuilder(_builder);
    _state = State::kMetadata;
    return *this;
}

BufBuilder& LegacyReplyBuilder::getInPlaceReplyBuilder(std::size_t reserveBytes) {
    invariant(_state == State::kCommandReply);
    // Eagerly allocate reserveBytes bytes.
    _builder.reserveBytes(reserveBytes);
    // Claim our reservation immediately so we can actually write data to it.
    _builder.claimReservedBytes(reserveBytes);
    _state = State::kMetadata;
    return _builder;
}

LegacyReplyBuilder& LegacyReplyBuilder::setMetadata(const BSONObj& metadata) {
    invariant(_state == State::kMetadata);
    // HACK: the only thing we need to downconvert is ShardingMetadata, which can go at the end of
    // the object. So we do that in place to avoid copying the command reply.
    auto shardingMetadata = rpc::ShardingMetadata::readFromMetadata(metadata);
    invariant(shardingMetadata.isOK() || shardingMetadata.getStatus() == ErrorCodes::NoSuchKey);

    if (shardingMetadata.isOK()) {
        // Write the sharding metadata in to the end of the object. The third parameter is needed
        // because we already have skipped some bytes for the message header.
        BSONObjBuilder resumedBuilder(
            BSONObjBuilder::ResumeBuildingTag(), _builder, sizeof(QueryResult::Value));
        shardingMetadata.getValue().writeToMetadata(&resumedBuilder);
    }
    _state = State::kOutputDocs;
    return *this;
}

Status LegacyReplyBuilder::addOutputDocs(DocumentRange docs) {
    invariant(_state == State::kOutputDocs);
    // no op
    return Status::OK();
}

Status LegacyReplyBuilder::addOutputDoc(const BSONObj& bson) {
    invariant(_state == State::kOutputDocs);
    // no op
    return Status::OK();
}

ReplyBuilderInterface::State LegacyReplyBuilder::getState() const {
    return _state;
}

Protocol LegacyReplyBuilder::getProtocol() const {
    return rpc::Protocol::kOpQuery;
}

void LegacyReplyBuilder::reset() {
    // If we are in State::kMetadata, we are already in the 'start' state, so by
    // immediately returning, we save a heap allocation.
    if (_state == State::kCommandReply) {
        return;
    }
    _builder.reset();
    _builder.skip(sizeof(QueryResult::Value));
    _message.reset();
    _state = State::kCommandReply;
    _staleConfigError = false;
}


Message LegacyReplyBuilder::done() {
    invariant(_state == State::kOutputDocs);

    QueryResult::View qr = _builder.buf();

    if (_staleConfigError) {
        // For compatibility with legacy mongos, we need to set this result flag on SendStaleConfig
        qr.setResultFlags(ResultFlag_ErrSet | ResultFlag_ShardConfigStale);
    } else {
        qr.setResultFlagsToOk();
    }

    qr.msgdata().setLen(_builder.len());
    qr.msgdata().setOperation(opReply);
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(1);

    _message.setData(_builder.release());

    _state = State::kDone;
    return std::move(_message);
}

}  // namespace rpc
}  // namespace mongo
