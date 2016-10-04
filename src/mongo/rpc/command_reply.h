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

#include "mongo/db/jsobj.h"
#include "mongo/rpc/document_range.h"
#include "mongo/rpc/reply_interface.h"

namespace mongo {
class Message;

namespace rpc {

/**
 * An immutable view of an OP_COMMANDREPLY message. The underlying bytes are owned
 * by a mongo::Message, which must outlive any Reply instances created from it.
 *
 * TODO: _metadata and _commandReply are owned by the underlying message. When
 * we implement a BSONObjView or similar, we should use that here to make these semantics
 * explicit.
 * See SERVER-16730 for additional details.
 */
class CommandReply : public ReplyInterface {
public:
    /**
     * Construct a Reply from a Message.
     * The underlying message MUST outlive the Reply.
     * Required fields are parsed eagerly, outputDocs are parsed lazily.
     *
     * The underlying Message also handles the wire-protocol header.
     */
    explicit CommandReply(const Message* message);

    /**
     * Accessor for the metadata object. Metadata is generally used for information
     * that is independent of any specific command, e.g. auditing information.
     */
    const BSONObj& getMetadata() const final;

    /**
     * The result of executing the command.
     */
    const BSONObj& getCommandReply() const final;

    /**
     * A variable number of BSON documents returned by the command. It is valid for the
     * returned range to be empty.
     *
     * Example usage:
     *
     * for (auto&& doc : reply.getOutputDocs()) {
     *    ... do stuff with doc
     * }
     */
    DocumentRange getOutputDocs() const final;

    Protocol getProtocol() const final;

    friend bool operator==(const CommandReply& lhs, const CommandReply& rhs);
    friend bool operator!=(const CommandReply& lhs, const CommandReply& rhs);

private:
    const Message* _message;

    BSONObj _metadata;
    BSONObj _commandReply;
    DocumentRange _outputDocs;
};

}  // namespace rpc
}  // namespace mongo
