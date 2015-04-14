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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/rpc/document_range.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

    /**
     * Constructs an OP_COMMAND message.
     */
    class RequestBuilder {
    public:

        /**
         * Constructs an OP_COMMAND in a new buffer.
         */
        RequestBuilder();

        /**
         * Construct an OP_COMMAND in an existing buffer. Ownership of the buffer will be
         * transfered to the RequestBuilder.
         */
        RequestBuilder(std::unique_ptr<Message> message);

        RequestBuilder& setDatabase(StringData database);
        RequestBuilder& setCommandName(StringData commandName);
        RequestBuilder& setMetadata(const BSONObj& metadata);
        RequestBuilder& setCommandArgs(const BSONObj& commandArgs);

        RequestBuilder& addInputDocs(DocumentRange inputDocs);
        RequestBuilder& addInputDoc(const BSONObj& inputDoc);

        /**
         * Writes data then transfers ownership of the message to the caller.
         * The behavior of calling any methods on the object is subsequently
         * undefined.
         */
        std::unique_ptr<Message> done();

    private:
        BufBuilder _builder{};
        std::unique_ptr<Message> _message;

        enum class BuildState {
            kDatabase,
            kCommandName,
            kMetadata,
            kCommandArgs,
            kInputDocs,
            kDone
        };

        BuildState _buildState{BuildState::kDatabase};
    };

}  // rpc
}  // mongo
