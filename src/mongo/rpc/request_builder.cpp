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

#include "mongo/rpc/request_builder.h"

#include <utility>

#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

    RequestBuilder::RequestBuilder()
        : _message{stdx::make_unique<Message>()}
    {}

    RequestBuilder::RequestBuilder(std::unique_ptr<Message> message)
        : _message{std::move(message)}
    {}

    RequestBuilder& RequestBuilder::setDatabase(StringData database) {
        invariant(_buildState == BuildState::kDatabase);
        _builder.appendStr(database);
        _buildState = BuildState::kCommandName;
        return *this;
    }

    RequestBuilder& RequestBuilder::setCommandName(StringData commandName) {
        invariant(_buildState == BuildState::kCommandName);
        _builder.appendStr(commandName);
        _buildState = BuildState::kMetadata;
        return *this;
    }

    RequestBuilder& RequestBuilder::setMetadata(const BSONObj& metadata) {
        invariant(_buildState == BuildState::kMetadata);
        metadata.appendSelfToBufBuilder(_builder);
        _buildState = BuildState::kCommandArgs;
        return *this;
    }

    RequestBuilder& RequestBuilder::setCommandArgs(const BSONObj& commandArgs) {
        invariant(_buildState == BuildState::kCommandArgs);
        commandArgs.appendSelfToBufBuilder(_builder);
        _buildState = BuildState::kInputDocs;
        return *this;
    }

    RequestBuilder& RequestBuilder::addInputDocs(DocumentRange inputDocs) {
        invariant(_buildState == BuildState::kInputDocs);
        auto rangeData = inputDocs.data();
        _builder.appendBuf(rangeData.data(), rangeData.length());
        return *this;
    }

    RequestBuilder& RequestBuilder::addInputDoc(const BSONObj& inputDoc) {
        invariant(_buildState == BuildState::kInputDocs);
        inputDoc.appendSelfToBufBuilder(_builder);
        return *this;
    }

    std::unique_ptr<Message> RequestBuilder::done() {
        invariant(_buildState == BuildState::kInputDocs);
        // TODO: we can elide a large copy here by transferring the internal buffer of
        // the BufBuilder to the Message.
        _message->setData(dbCommand, _builder.buf(), _builder.len());
        _buildState = BuildState::kDone;
        return std::move(_message);
    }

}  // namespace rpc
}  // namespace mongo
