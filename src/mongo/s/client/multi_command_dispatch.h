/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/s/bson_serializable.h"

namespace mongo {

/**
 * A MultiCommandDispatch is a send/recv operation for multiple commands at once.
 *
 * The commands are first registered with an endpoint and serializable request.  Commands are
 * sent out without waiting for responses, and then responses are read later one-at-a-time.
 *
 * If context must be tracked alongside these requests, it can be associated with the endpoint
 * object.
 */
class MultiCommandDispatch {
public:
    virtual ~MultiCommandDispatch() {}

    /**
     * Adds a command to this multi-command dispatch.  Commands are registered with a
     * ConnectionString endpoint and a BSON request object.
     *
     * Commands are not sent immediately, they are sent on sendAll.
     */
    virtual void addCommand(const ConnectionString& endpoint,
                            StringData dbName,
                            const BSONObj& request) = 0;

    /**
     * Sends all the commands in this dispatch to their endpoints, in undefined order and
     * without waiting for responses.  May block on full send queue (though this should be
     * rare).
     *
     * Any error which occurs during sendAll will be reported on recvAny, *does not throw.*
     */
    virtual void sendAll() = 0;

    /**
     * Returns the number of sent requests that are still waiting to be recv'd.
     */
    virtual int numPending() const = 0;

    /**
     * Blocks until a command response has come back.  Any outstanding command response may be
     * returned with associated endpoint.
     *
     * Returns !OK on send/recv/parse failure, otherwise command-level errors are returned in
     * the response object itself.
     */
    virtual Status recvAny(ConnectionString* endpoint, BSONSerializable* response) = 0;
};
}
