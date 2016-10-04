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

#include "mongo/rpc/legacy_request_builder.h"

#include <tuple>
#include <utility>

#include "mongo/db/namespace_string.h"
#include "mongo/rpc/metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

LegacyRequestBuilder::LegacyRequestBuilder() : LegacyRequestBuilder(Message()) {}

LegacyRequestBuilder::~LegacyRequestBuilder() {}

LegacyRequestBuilder::LegacyRequestBuilder(Message&& message) : _message{std::move(message)} {
    _builder.skip(mongo::MsgData::MsgDataHeaderSize);
}

LegacyRequestBuilder& LegacyRequestBuilder::setDatabase(StringData database) {
    invariant(_state == State::kDatabase);
    _ns = NamespaceString(database, "$cmd").toString();
    _state = State::kCommandName;
    return *this;
}

LegacyRequestBuilder& LegacyRequestBuilder::setCommandName(StringData commandName) {
    invariant(_state == State::kCommandName);
    // no op, as commandName is the first element of commandArgs
    _state = State::kCommandArgs;
    return *this;
}

LegacyRequestBuilder& LegacyRequestBuilder::setCommandArgs(BSONObj commandArgs) {
    invariant(_state == State::kCommandArgs);
    _commandArgs = std::move(commandArgs);
    _state = State::kMetadata;
    return *this;
}

LegacyRequestBuilder& LegacyRequestBuilder::setMetadata(BSONObj metadata) {
    invariant(_state == State::kMetadata);
    BSONObj legacyCommandArgs;
    int queryOptions;

    std::tie(legacyCommandArgs, queryOptions) = uassertStatusOK(
        rpc::downconvertRequestMetadata(std::move(_commandArgs), std::move(metadata)));

    _builder.appendNum(queryOptions);  // queryOptions
    _builder.appendStr(_ns);
    _builder.appendNum(0);  // nToSkip
    _builder.appendNum(1);  // nToReturn

    legacyCommandArgs.appendSelfToBufBuilder(_builder);
    _state = State::kInputDocs;
    return *this;
}

LegacyRequestBuilder& LegacyRequestBuilder::addInputDocs(DocumentRange inputDocs) {
    invariant(_state == State::kInputDocs);
    // no op
    return *this;
}

LegacyRequestBuilder& LegacyRequestBuilder::addInputDoc(BSONObj inputDoc) {
    invariant(_state == State::kInputDocs);
    // no op
    return *this;
}

RequestBuilderInterface::State LegacyRequestBuilder::getState() const {
    return _state;
}

Protocol LegacyRequestBuilder::getProtocol() const {
    return rpc::Protocol::kOpQuery;
}

Message LegacyRequestBuilder::done() {
    invariant(_state == State::kInputDocs);
    MsgData::View msg = _builder.buf();
    msg.setLen(_builder.len());
    msg.setOperation(dbQuery);
    _message.setData(_builder.release());
    _state = State::kDone;
    return std::move(_message);
}

}  // namespace rpc
}  // namespace mongo
