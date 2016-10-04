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

#include <boost/optional.hpp>

namespace mongo {

class Status;
template <typename T>
class StatusWith;
struct HostAndPort;

namespace executor {

struct RemoteCommandResponse;
struct RemoteCommandRequest;

/**
 * An hooking interface for augmenting an implementation of NetworkInterface with domain-specific
 * host validation and post-connection logic.
 */
class NetworkConnectionHook {
public:
    virtual ~NetworkConnectionHook() = default;

    /**
     * Runs optional validation logic on an isMaster reply from a remote host. If a non-OK
     * Status is returned, it will be propagated up to the completion handler for the command
     * that initiated the request that caused this connection to be created. This will
     * be called once for each connection that is created, even if a remote host with the
     * same HostAndPort has already successfully passed validation on a different connection.
     *
     * This method may be called on a different thread from the caller of startCommand that caused
     * this connection to be created.
     *
     * This method must not throw any exceptions or block on network or disk-IO. However, in the
     * event that an exception escapes, the NetworkInterface is responsible for calling
     * std::terminate.
     */
    virtual Status validateHost(const HostAndPort& remoteHost,
                                const RemoteCommandResponse& isMasterReply) = 0;

    /**
     * Generates a command to run on the remote host immediately after connecting to it.
     * If a non-OK StatusWith is returned, it will be propagated up to the completion handler
     * for the command that initiated the request that caused this connection to be created.
     *
     * The command will be run after socket setup, SSL handshake, authentication, and wire
     * protocol detection, but before any commands submitted to the NetworkInterface via
     * startCommand are run. In the case that it isn't necessary to run a command, makeRequest
     * may return boost::none.
     *
     * This method may be called on a different thread from the caller of startCommand that caused
     * this connection to be created.
     *
     * This method must not throw any exceptions or block on network or disk-IO. However, in the
     * event that an exception escapes, the NetworkInterface is responsible for calling
     * std::terminate.
     */
    virtual StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(
        const HostAndPort& remoteHost) = 0;

    /**
     * Handles a remote server's reply to the command generated with makeRequest. If a
     * non-OK Status is returned, it will be propagated up to the completion handler for the
     * command that initiated the request that caused this connection to be created.
     *
     * If the corresponding earlier call to makeRequest for this connection returned
     * boost::none, the NetworkInterface will not call handleReply.
     *
     * This method may be called on a different thread from the caller of startCommand that caused
     * this connection to be created.
     *
     * This method must not throw any exceptions or block on network or disk-IO. However, in the
     * event that an exception escapes, the NetworkInterface is responsible for calling
     * std::terminate.
     */
    virtual Status handleReply(const HostAndPort& remoteHost, RemoteCommandResponse&& response) = 0;

protected:
    NetworkConnectionHook() = default;
};

}  // namespace executor
}  // namespace mongo
