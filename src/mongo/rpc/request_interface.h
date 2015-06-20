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

#include "mongo/base/disallow_copying.h"
#include "mongo/rpc/protocol.h"

namespace mongo {
class BSONObj;
class Message;
class StringData;

namespace rpc {
class DocumentRange;

/**
 * An immutable view of an RPC message.
 */
class RequestInterface {
    MONGO_DISALLOW_COPYING(RequestInterface);

public:
    virtual ~RequestInterface() = default;

    /**
     * Gets the database that the command is to be executed on.
     */
    virtual StringData getDatabase() const = 0;

    /**
     * Gets the name of the command to execute.
     */
    virtual StringData getCommandName() const = 0;

    /**
     * Gets the metadata associated with the command request. This is information that is
     * independent of any specific command, i.e. auditing information. See metadata.h for
     * further details.
     */
    virtual const BSONObj& getMetadata() const = 0;

    /**
     * Gets the arguments to the command - this is passed to the command's run() method.
     */
    virtual const BSONObj& getCommandArgs() const = 0;

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
    virtual DocumentRange getInputDocs() const = 0;

    /**
     * Gets the RPC protocol used to deserialize this message. This should only be used for
     * asserts, and not for runtime behavior changes, which should be handled with polymorphism.
     */
    virtual Protocol getProtocol() const = 0;

protected:
    RequestInterface() = default;
};

}  // namespace rpc
}  // namespace mongo
