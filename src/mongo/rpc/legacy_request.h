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

#include "mongo/base/string_data.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/document_range.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/request_interface.h"

namespace mongo {
class Message;

namespace rpc {

/**
 * An immutable view of an OP_QUERY command request. The underlying bytes are owned
 * by a mongo::Message, which must outlive any LegacyRequest instances created from it.
 *
 */
class LegacyRequest : public RequestInterface {
public:
    /**
     * Construct a Request from a Message. Underlying message MUST outlive the Request.
     * Required fields are parsed eagerly, inputDocs are parsed lazily.
     */
    explicit LegacyRequest(const Message* message);

    ~LegacyRequest() final;

    /**
     * The database that the command is to be executed on.
     */
    StringData getDatabase() const final;

    /**
     * The name of the command to execute.
     */
    StringData getCommandName() const final;

    /**
     * The metadata associated with the command request. This is information that is
     * independent of any specific command, i.e. auditing information.
     */
    const BSONObj& getMetadata() const final;

    /**
     * The arguments to the command - this is passed to the command's run() method.
     */
    const BSONObj& getCommandArgs() const final;

    /**
     * A variable number of BSON documents to pass to the command. It is valid for
     * the returned range to be empty.
     *
     * Example usage:
     *
     * for (auto&& doc : req.getInputDocs()) {
     *    ... do stuff with doc
     * }
     */
    DocumentRange getInputDocs() const final;

    Protocol getProtocol() const final;

private:
    const Message* _message;
    // TODO: metadata will be handled in SERVER-18236
    // for now getMetadata() is a no op
    DbMessage _dbMessage;
    QueryMessage _queryMessage;
    StringData _database;

    BSONObj _upconvertedMetadata;
    BSONObj _upconvertedCommandArgs;
};

}  // namespace rpc
}  // namespace mongo
