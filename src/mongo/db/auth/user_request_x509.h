// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/ssl_peer_info.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

#ifdef MONGO_CONFIG_SSL

/**
 * This is the version of UserRequest that is used by X509. It provides
 * a way to store X509 metadata for retrieving roles.
 *
 * When constructing the UserRequestX509, you must use the static function
 * makeUserRequestX509. It will automatically populate the roles from the
 * SSLPeerInfo struct if they exist.
 */
class UserRequestX509 : public UserRequestGeneral {
private:
    struct Passkey {
        Passkey() = default;
    };

public:
    /**
     * Makes a new UserRequestX509. Toggling for re-acquire to true enables
     * a re-fetch of the roles from the certificate.
     */
    static StatusWith<std::unique_ptr<UserRequest>> makeUserRequestX509(
        UserName name,
        boost::optional<std::set<RoleName>> roles,
        std::shared_ptr<const SSLPeerInfo> peerInfo,
        bool forReacquire = true,
        bool insertAuthenticatedMechanism = false);

    UserRequestType getType() const final {
        return UserRequestType::X509;
    }
    std::shared_ptr<const SSLPeerInfo> getPeerInfo() const {
        return _peerInfo;
    }
    std::unique_ptr<UserRequest> clone() const final {
        // Since we are invoking the non-reacquire version of makeUserRequestX509,
        // we can be certain that the uassert below will not throw.
        return uassertStatusOK(makeUserRequestX509(
            getUserName(), getRoles(), getPeerInfo(), false, hasAuthenticatedMechanism()));
    }

    StatusWith<std::unique_ptr<UserRequest>> cloneForReacquire() const final {
        return makeUserRequestX509(
            getUserName(), getRoles(), getPeerInfo(), hasAuthenticatedMechanism());
    }

    UserRequestCacheKey generateUserRequestCacheKey() const final;

    UserRequestX509(Passkey,
                    UserName name,
                    boost::optional<std::set<RoleName>> roles,
                    std::shared_ptr<const SSLPeerInfo> peerInfo)
        : UserRequestGeneral(std::move(name), std::move(roles)), _peerInfo(std::move(peerInfo)) {}

private:
    void _tryAcquireRoles();

    std::shared_ptr<const SSLPeerInfo> _peerInfo;
};

#endif  // MONGO_CONFIG_SSL

}  // namespace mongo
