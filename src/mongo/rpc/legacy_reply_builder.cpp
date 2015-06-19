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

#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

    LegacyReplyBuilder::LegacyReplyBuilder()
        : LegacyReplyBuilder(stdx::make_unique<Message>())
    {}

    LegacyReplyBuilder::LegacyReplyBuilder(std::unique_ptr<Message> message)
        : _message{std::move(message)} {
        _builder.skip(sizeof(QueryResult::Value));
    }

    LegacyReplyBuilder::~LegacyReplyBuilder() {}

    LegacyReplyBuilder& LegacyReplyBuilder::setMetadata(BSONObj metadata) {
        invariant(_state == State::kMetadata);
        _metadata = std::move(metadata);
        _state = State::kCommandReply;
        return *this;
    }

    LegacyReplyBuilder& LegacyReplyBuilder::setRawCommandReply(BSONObj commandReply) {
        invariant(_state == State::kCommandReply);
        BSONObj downconvertedCommandReply = uassertStatusOK(
            rpc::downconvertReplyMetadata(std::move(commandReply), std::move(_metadata))
        );
        downconvertedCommandReply.appendSelfToBufBuilder(_builder);
        _state = State::kOutputDocs;
        return *this;
    }

    LegacyReplyBuilder& LegacyReplyBuilder::addOutputDocs(DocumentRange outputDocs) {
        invariant(_state == State::kOutputDocs);
        // no op
        return *this;
    }

    LegacyReplyBuilder& LegacyReplyBuilder::addOutputDoc(BSONObj outputDoc) {
        invariant(_state == State::kOutputDocs);
        // no op
        return *this;
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
        if (_state == State::kMetadata) {
            return;
        }
        _builder.reset();
        _metadata = BSONObj();
        _message = stdx::make_unique<Message>();
        _state = State::kMetadata;
    }


    std::unique_ptr<Message> LegacyReplyBuilder::done() {
        invariant(_state == State::kOutputDocs);
        std::unique_ptr<Message> message = stdx::make_unique<Message>();

        QueryResult::View qr = _builder.buf();
        qr.setResultFlagsToOk();
        qr.msgdata().setLen(_builder.len());
        qr.msgdata().setOperation(opReply);
        qr.setCursorId(0);
        qr.setStartingFrom(0);
        qr.setNReturned(1);
        _builder.decouple();

        message->setData(qr.view2ptr(), true);

        _state = State::kDone;
        return std::move(message);
    }

    std::size_t LegacyReplyBuilder::availableSpaceForOutputDocs() const {
        invariant (State::kDone != _state);
        // LegacyReplyBuilder currently does not support addOutputDoc(s)
        return 0u;
    }

}  // namespace rpc
}  // namespace mongo
