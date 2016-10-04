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

#include "mongo/base/disallow_copying.h"
#include "mongo/rpc/protocol.h"

namespace mongo {
class Message;
class BSONObj;
class StringData;

namespace rpc {
class DocumentRange;

/**
 * Constructs an RPC request.
 */
class RequestBuilderInterface {
    MONGO_DISALLOW_COPYING(RequestBuilderInterface);

public:
    /**
     * Request builders must have their fields set in order as they are immediately written into
     * the underlying message buffer. This enum represents the next field that can be written
     * into the builder. Note that when the builder is in state 'kInputDocs', multiple input
     * docs can be added. After the builder's done() method is called it is in state 'kDone',
     * and no further methods can be called.
     */
    enum class State { kDatabase, kCommandName, kMetadata, kCommandArgs, kInputDocs, kDone };

    virtual ~RequestBuilderInterface() = default;

    /**
     * Sets the database that the command will be executed against.
     */
    virtual RequestBuilderInterface& setDatabase(StringData database) = 0;

    /**
     * Sets the name of the command to execute.
     */
    virtual RequestBuilderInterface& setCommandName(StringData commandName) = 0;

    /**
     * Sets the metadata associated with this command request - see metadata.h for details.
     */
    virtual RequestBuilderInterface& setMetadata(BSONObj metadata) = 0;

    /**
     * Sets the arguments to pass to the command.
     */
    virtual RequestBuilderInterface& setCommandArgs(BSONObj commandArgs) = 0;

    /**
     * Add a range of input documents to the request. This method can be called multiple times
     * before calling done().
     */
    virtual RequestBuilderInterface& addInputDocs(DocumentRange inputDocs) = 0;

    /**
     * Add a single output document to the request. This method can be called multiple times
     * before calling done().
     */
    virtual RequestBuilderInterface& addInputDoc(BSONObj inputDoc) = 0;

    /**
     * Get the state of the builder. This method is intended to enable debug or invariant
     * checks that the builder is in the correct state.
     */
    virtual State getState() const = 0;

    /**
     * Gets the protocol used to serialize this request. This should only be used for asserts,
     * and not for runtime behavior changes, which should be handled with polymorphism.
     */
    virtual Protocol getProtocol() const = 0;

    /**
     * Writes data then transfers ownership of the message to the caller.
     * The behavior of calling any methods on the object is subsequently
     * undefined.
     */
    virtual Message done() = 0;

protected:
    RequestBuilderInterface() = default;
};

}  // namespace rpc
}  // namespace mongo
