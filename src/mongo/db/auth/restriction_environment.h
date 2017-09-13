/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/client.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/util/net/sockaddr.h"

namespace mongo {

// A RestrictionEnvironment stores all information about an incoming client which could be used to
// verify whether it should be able to authenticate as a user, or be granted a role.
// It must be constructed and attached to a Client object while a server is accepting a connection.
// Clients created by internal server operations may not have a RestrictionEnvironment. Clients
// which attempt to perform authentication or authorization must have a RestrictionEnvironment.
class RestrictionEnvironment {
public:
    RestrictionEnvironment(SockAddr clientSource, SockAddr serverAddress)
        : clientSource(std::move(clientSource)), serverAddress(std::move(serverAddress)) {}

    // Retrieve a RestrictionEnvironment from a Client.
    static const RestrictionEnvironment& get(const Client& client);
    static void get(Client&&) = delete;

    // Set a RestrictionEnvironment on a transport Session
    static void set(const transport::SessionHandle& session,
                    std::unique_ptr<RestrictionEnvironment> environment);

    // Returns the source address of the client.
    // This value is useful for filering clients by their address, or network block. Note that
    // clients on some networks can spoof this address.
    const SockAddr& getClientSource() const& {
        return clientSource;
    }
    void getClientSource() && = delete;

    // Returns the listening address which the server accepted a client on.
    // A server may be bound to multiple network interfaces, each with their own IP address.
    const SockAddr& getServerAddress() const& {
        return serverAddress;
    }
    void getServerAddress() && = delete;

private:
    SockAddr clientSource;
    SockAddr serverAddress;
};

}  // namespace mongo
