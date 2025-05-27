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

#pragma once


#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
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
public:
    // We define this function as a friend so that makeUserRequestX509
    // can use it.
    friend std::unique_ptr<UserRequestX509> std::make_unique<UserRequestX509>(
        mongo::UserName&& name,
        boost::optional<std::set<mongo::RoleName>>&& roles,
        std::shared_ptr<const mongo::SSLPeerInfo>&& peerInfo);

    /**
     * Makes a new UserRequestX509. Toggling for re-acquire to true enables
     * a re-fetch of the roles from the certificate.
     */
    static StatusWith<std::unique_ptr<UserRequest>> makeUserRequestX509(
        UserName name,
        boost::optional<std::set<RoleName>> roles,
        std::shared_ptr<const SSLPeerInfo> peerInfo,
        bool forReacquire = true);

    UserRequestType getType() const final {
        return UserRequestType::X509;
    }
    std::shared_ptr<const SSLPeerInfo> getPeerInfo() const {
        return _peerInfo;
    }
    std::unique_ptr<UserRequest> clone() const final {
        // Since we are invoking the non-reacquire version of makeUserRequestX509,
        // we can be certain that the uassert below will not throw.
        return uassertStatusOK(
            makeUserRequestX509(getUserName(), getRoles(), getPeerInfo(), false));
    }

    StatusWith<std::unique_ptr<UserRequest>> cloneForReacquire() const final {
        return makeUserRequestX509(getUserName(), getRoles(), getPeerInfo());
    }

    UserRequestCacheKey generateUserRequestCacheKey() const final;

protected:
    UserRequestX509(UserName name,
                    boost::optional<std::set<RoleName>> roles,
                    std::shared_ptr<const SSLPeerInfo> peerInfo)
        : UserRequestGeneral(std::move(name), std::move(roles)), _peerInfo(std::move(peerInfo)) {}

private:
    void _tryAcquireRoles();

    std::shared_ptr<const SSLPeerInfo> _peerInfo;
};

#endif  // MONGO_CONFIG_SSL

}  // namespace mongo
