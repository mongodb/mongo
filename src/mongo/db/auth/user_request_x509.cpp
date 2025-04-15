/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/auth/user_request_x509.h"

#include "mongo/util/net/ssl_peer_info.h"

namespace mongo {

#ifdef MONGO_CONFIG_SSL

StatusWith<std::unique_ptr<UserRequest>> UserRequestX509::makeUserRequestX509(
    UserName name,
    boost::optional<std::set<RoleName>> roles,
    std::shared_ptr<const SSLPeerInfo> peerInfo,
    bool forReacquire) {
    auto request =
        std::make_unique<UserRequestX509>(std::move(name), std::move(roles), std::move(peerInfo));

    if (!forReacquire) {
        return std::unique_ptr<UserRequest>(std::move(request));
    }

    request->_tryAcquireRoles();
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
