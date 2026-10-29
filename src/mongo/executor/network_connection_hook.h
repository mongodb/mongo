// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

class Status;
template <typename T>
class StatusWith;
struct HostAndPort;

namespace executor {

struct RemoteCommandResponse;

/**
 * An hooking interface for augmenting an implementation of NetworkInterface with domain-specific
 * host validation and post-connection logic.
 */
class [[MONGO_MOD_OPEN]] NetworkConnectionHook {
public:
    virtual ~NetworkConnectionHook() = default;

    /**
     * Optionally augments the "hello" request sent while initializing the wire protocol.
     *
     * By default this will just return the cmdObj passed in unaltered.
     */
    virtual BSONObj augmentHelloRequest(const HostAndPort& remoteHost, BSONObj cmdObj) {
        return cmdObj;
    }

    /**
     * Runs optional validation logic on an "hello" reply from a remote host. If a non-OK
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
                                const BSONObj& helloRequest,
                                const RemoteCommandResponse& helloReply) = 0;

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
