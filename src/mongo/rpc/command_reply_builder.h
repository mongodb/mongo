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

#pragma once

#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/document_range.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

    /**
     * Constructs an OP_COMMANDREPLY message.
     */
    class CommandReplyBuilder : public ReplyBuilderInterface {
    public:

        /**
         * Constructs an OP_COMMANDREPLY in a new buffer.
         */
        CommandReplyBuilder();

        /*
         * Constructs an OP_COMMANDREPLY in an existing buffer. Ownership of the buffer
         * will be transfered to the CommandReplyBuilder.
         */
        CommandReplyBuilder(std::unique_ptr<Message> message);

        CommandReplyBuilder& setMetadata(BSONObj metadata) final;
        CommandReplyBuilder& setRawCommandReply(BSONObj commandReply) final;

        CommandReplyBuilder& addOutputDocs(DocumentRange outputDocs) final;
        CommandReplyBuilder& addOutputDoc(BSONObj outputDoc) final;

        State getState() const final;

        /**
         * Writes data then transfers ownership of the message to the caller.
         * The behavior of calling any methods on the object is subsequently
         * undefined.
         */
        std::unique_ptr<Message> done() final;

    private:
        // Default values are all empty.
        BufBuilder _builder{};
        std::unique_ptr<Message> _message;

        State _state{State::kMetadata};
    };

}  // namespace rpc
}  // namespace mongo
