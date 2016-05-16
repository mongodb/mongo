/**
 *    Copyright (C) 2016 MongoDB Inc.
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
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace transport {

class TransportLayer;

/**
 * This type contains data needed to associate Messages with connections
 * (on the transport side) and Messages with Client objects (on the database side).
 */
class Session {
    MONGO_DISALLOW_COPYING(Session);

public:
    /**
     * Type to indicate the internal id for this session.
     */
    using SessionId = uint64_t;

    /**
     * Construct a new session.
     */
    Session(HostAndPort remote, HostAndPort local, TransportLayer* tl);

    /**
     * Destroys a session, calling end() for this session in its TransportLayer.
     */
    ~Session();

    /**
     * Move constructor and assignment operator.
     */
    Session(Session&& other);
    Session& operator=(Session&& other);

    /**
     * Return the id for this session.
     */
    SessionId id() const;

    /**
     * Return the remote host for this session.
     */
    const HostAndPort& remote() const;

    /**
     * Return the local host information for this session.
     */
    const HostAndPort& local() const;

private:
    SessionId _id;

    HostAndPort _remote;
    HostAndPort _local;

    TransportLayer* _tl;
};

}  // namespace transport
}  // namespace mongo
