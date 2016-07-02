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

#include "mongo/rpc/command_request_builder.h"

#include <utility>

#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

CommandRequestBuilder::CommandRequestBuilder() : CommandRequestBuilder(Message()) {}

CommandRequestBuilder::~CommandRequestBuilder() {}

CommandRequestBuilder::CommandRequestBuilder(Message&& message) : _message{std::move(message)} {
    _builder.skip(mongo::MsgData::MsgDataHeaderSize);  // Leave room for message header.
}

CommandRequestBuilder& CommandRequestBuilder::setDatabase(StringData database) {
    invariant(_state == State::kDatabase);
    _builder.appendStr(database);
    _state = State::kCommandName;
    return *this;
}

CommandRequestBuilder& CommandRequestBuilder::setCommandName(StringData commandName) {
    invariant(_state == State::kCommandName);
    _builder.appendStr(commandName);
    _state = State::kCommandArgs;
    return *this;
}

CommandRequestBuilder& CommandRequestBuilder::setCommandArgs(BSONObj commandArgs) {
    invariant(_state == State::kCommandArgs);
    commandArgs.appendSelfToBufBuilder(_builder);
    _state = State::kMetadata;
    return *this;
}

CommandRequestBuilder& CommandRequestBuilder::setMetadata(BSONObj metadata) {
    invariant(_state == State::kMetadata);
    metadata.appendSelfToBufBuilder(_builder);
    _state = State::kInputDocs;
    return *this;
}

CommandRequestBuilder& CommandRequestBuilder::addInputDocs(DocumentRange inputDocs) {
    invariant(_state == State::kInputDocs);
    auto rangeData = inputDocs.data();
    _builder.appendBuf(rangeData.data(), rangeData.length());
    return *this;
}

CommandRequestBuilder& CommandRequestBuilder::addInputDoc(BSONObj inputDoc) {
    invariant(_state == State::kInputDocs);
    inputDoc.appendSelfToBufBuilder(_builder);
    return *this;
}

RequestBuilderInterface::State CommandRequestBuilder::getState() const {
    return _state;
}

Protocol CommandRequestBuilder::getProtocol() const {
    return rpc::Protocol::kOpCommandV1;
}

Message CommandRequestBuilder::done() {
    invariant(_state == State::kInputDocs);
    MsgData::View msg = _builder.buf();
    msg.setLen(_builder.len());
    msg.setOperation(dbCommand);
    _message.setData(_builder.release());  // transfer ownership to Message.
    _state = State::kDone;
    return std::move(_message);
}

}  // namespace rpc
}  // namespace mongo
