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

#include <utility>

#include "mongo/rpc/command_reply_builder.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

    CommandReplyBuilder::CommandReplyBuilder()
        : _message{stdx::make_unique<Message>()}
    {}

    CommandReplyBuilder::CommandReplyBuilder(std::unique_ptr<Message> message)
        : _message{std::move(message)}
    {}

    CommandReplyBuilder& CommandReplyBuilder::setMetadata(BSONObj metadata) {
        invariant(_state == State::kMetadata);
        metadata.appendSelfToBufBuilder(_builder);
        _state = State::kCommandReply;
        return *this;
    }

    CommandReplyBuilder& CommandReplyBuilder::setRawCommandReply(BSONObj commandReply) {
        invariant(_state == State::kCommandReply);
        commandReply.appendSelfToBufBuilder(_builder);
        _state = State::kOutputDocs;
        return *this;
    }

    CommandReplyBuilder& CommandReplyBuilder::addOutputDocs(DocumentRange outputDocs) {
        invariant(_state == State::kOutputDocs);
        auto rangeData = outputDocs.data();
        _builder.appendBuf(rangeData.data(), rangeData.length());
        // leave state as is as we can add as many outputDocs as we want.
        return *this;
    }

    CommandReplyBuilder& CommandReplyBuilder::addOutputDoc(BSONObj outputDoc) {
        invariant(_state == State::kOutputDocs);
        outputDoc.appendSelfToBufBuilder(_builder);
        return *this;
    }

    ReplyBuilderInterface::State CommandReplyBuilder::getState() const {
        return _state;
    }

    Protocol CommandReplyBuilder::getProtocol() const {
        return rpc::Protocol::kOpCommandV1;
    }

    void CommandReplyBuilder::reset() {
        // If we are in State::kMetadata, we are already in the 'start' state, so by
        // immediately returning, we save a heap allocation.
        if (_state == State::kMetadata) {
            return;
        }
        _builder.reset();
        _message = stdx::make_unique<Message>();
        _state = State::kMetadata;
    }

    std::unique_ptr<Message> CommandReplyBuilder::done() {
        invariant(_state == State::kOutputDocs);
        // TODO: we can elide a large copy here by transferring the internal buffer of
        // the BufBuilder to the Message.
        _message->setData(dbCommandReply, _builder.buf(), _builder.len());
        _state = State::kDone;
        return std::move(_message);
    }

    std::size_t CommandReplyBuilder::availableSpaceForOutputDocs() const {
        invariant (State::kDone != _state);
        int intLen = _builder.len();
        invariant(0 <= intLen);
        std::size_t len = static_cast<std::size_t>(intLen);
        std::size_t msgHeaderSz = static_cast<std::size_t>(MsgData::MsgDataHeaderSize);
        invariant(len + msgHeaderSz <= mongo::MaxMessageSizeBytes);
        return mongo::MaxMessageSizeBytes - len - msgHeaderSz;
    }

}  // rpc
}  // mongo
