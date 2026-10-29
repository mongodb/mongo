// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/sockaddr.h"

#include <memory>
#include <string_view>
#include <utility>

namespace mongo {

inline Status validateClientSourceAuthenticationRestrictionMode(std::string_view mode,
                                                                const boost::optional<TenantId>&) {
    if (mode != "origin" && mode != "peer") {
        return Status(ErrorCodes::BadValue,
                      "Unable to set value for clientSourceAuthenticationRestrictionMode. Valid "
                      "options are  \"origin\" or \"peer\".");
    }
    return Status::OK();
}

// A RestrictionEnvironment stores all information about an incoming client which could be used to
// verify whether it should be able to authenticate as a user, or be granted a role.
// It must be constructed and attached to a Client object while a server is accepting a connection.
// Clients created by internal server operations may not have a RestrictionEnvironment. Clients
// which attempt to perform authentication or authorization must have a RestrictionEnvironment.
class [[MONGO_MOD_PUBLIC]] RestrictionEnvironment {
public:
    RestrictionEnvironment() = default;
    RestrictionEnvironment(SockAddr clientSource, SockAddr serverAddress)
        : clientSource(std::move(clientSource)), serverAddress(std::move(serverAddress)) {}

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
