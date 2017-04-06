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

#include "mongo/rpc/command_reply_builder.h"

#include <utility>

#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

CommandReplyBuilder::CommandReplyBuilder() : CommandReplyBuilder(Message{}) {}

CommandReplyBuilder::CommandReplyBuilder(Message&& message) : _message{std::move(message)} {
    _builder.skip(mongo::MsgData::MsgDataHeaderSize);
}

CommandReplyBuilder& CommandReplyBuilder::setRawCommandReply(const BSONObj& commandReply) {
    invariant(_state == State::kCommandReply);
    commandReply.appendSelfToBufBuilder(_builder);
    _state = State::kMetadata;
    return *this;
}

BSONObjBuilder CommandReplyBuilder::getInPlaceReplyBuilder(std::size_t reserveBytes) {
    invariant(_state == State::kCommandReply);
    // Eagerly allocate reserveBytes bytes.
    _builder.reserveBytes(reserveBytes);
    // Claim our reservation immediately so we can actually write data to it.
    _builder.claimReservedBytes(reserveBytes);
    _state = State::kMetadata;
    return BSONObjBuilder(_builder);
}

CommandReplyBuilder& CommandReplyBuilder::setMetadata(const BSONObj& metadata) {
    invariant(_state == State::kMetadata);
    // OP_COMMAND is only used when communicating with 3.4 nodes and they serialize their metadata
    // fields differently. We do all up- and down-conversion here so that the rest of the code only
    // has to deal with the current format.
    BSONObjBuilder bob(_builder);
    for (auto elem : metadata) {
        if (elem.fieldNameStringData() == "$configServerState") {
            bob.appendAs(elem, "configsvr");
        } else {
            bob.append(elem);
        }
    }
    _state = State::kOutputDocs;
    return *this;
}

Protocol CommandReplyBuilder::getProtocol() const {
    return rpc::Protocol::kOpCommandV1;
}

void CommandReplyBuilder::reset() {
    // If we are in State::kCommandReply, we are already in the 'start' state, so by
    // immediately returning, we save a heap allocation.
    if (_state == State::kCommandReply) {
        return;
    }
    _builder.reset();
    _builder.skip(mongo::MsgData::MsgDataHeaderSize);
    _message.reset();
    _state = State::kCommandReply;
}

Message CommandReplyBuilder::done() {
    invariant(_state == State::kOutputDocs);
    MsgData::View msg = _builder.buf();
    msg.setLen(_builder.len());
    msg.setOperation(dbCommandReply);
    _message.setData(_builder.release());
    _state = State::kDone;
    return std::move(_message);
}

}  // rpc
}  // mongo
