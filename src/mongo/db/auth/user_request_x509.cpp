// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/user_request_x509.h"

#include "mongo/client/authenticate.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/net/ssl_peer_info.h"

namespace mongo {

#ifdef MONGO_CONFIG_SSL

StatusWith<std::unique_ptr<UserRequest>> UserRequestX509::makeUserRequestX509(
    UserName name,
    boost::optional<std::set<RoleName>> roles,
    std::shared_ptr<const SSLPeerInfo> peerInfo,
    bool forReacquire,
    bool insertAuthenticatedMechanism) {
    auto request = std::make_unique<UserRequestX509>(
        Passkey{}, std::move(name), std::move(roles), std::move(peerInfo));

    if (!forReacquire) {
        return std::unique_ptr<UserRequest>(std::move(request));
    }

    request->_tryAcquireRoles();
    if (insertAuthenticatedMechanism) {
        request->setAuthenticatedMechanism(std::string{auth::kMechanismMongoX509});
    }
    return std::unique_ptr<UserRequest>(std::move(request));
}

UserRequest::UserRequestCacheKey UserRequestX509::generateUserRequestCacheKey() const {
    uassert(10355702, "SSLPeerInfo is not set when generating user cache key", _peerInfo);
    auto hashElements = getUserNameAndRolesVector(getUserName(), getRoles());
    _peerInfo->appendPeerInfoToVector(hashElements);
    return UserRequestCacheKey(getUserName(), hashElements);
}

void UserRequestX509::_tryAcquireRoles() {
    if (!_peerInfo) {
        return;
    }

    const auto& peerRoles = _peerInfo->roles();
    if (peerRoles.empty()) {
        return;
    }

    std::set<RoleName> requestRoles;
    std::copy(
        peerRoles.begin(), peerRoles.end(), std::inserter(requestRoles, requestRoles.begin()));

    setRoles(std::move(requestRoles));
}

#endif  // MONGO_CONFIG_SSL

}  // namespace mongo
