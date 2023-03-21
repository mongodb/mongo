/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/transport/session.h"
#include "mongo/util/net/ssl_types.h"

namespace mongo {
/**
 * Contains information extracted from the peer certificate which is consumed by subsystems
 * outside of the networking stack.
 */
class SSLPeerInfo {
public:
    explicit SSLPeerInfo(SSLX509Name subjectName,
                         boost::optional<std::string> sniName = {},
                         stdx::unordered_set<RoleName> roles = {})
        : _isTLS(true),
          _subjectName(std::move(subjectName)),
          _sniName(std::move(sniName)),
          _roles(std::move(roles)) {}
    SSLPeerInfo() = default;

    explicit SSLPeerInfo(boost::optional<std::string> sniName)
        : _isTLS(true), _sniName(std::move(sniName)) {}

    bool isTLS() const {
        return _isTLS;
    }

    const SSLX509Name& subjectName() const {
        return _subjectName;
    }

    const boost::optional<std::string>& sniName() const {
        return _sniName;
    }

    const stdx::unordered_set<RoleName>& roles() const {
        return _roles;
    }

    static SSLPeerInfo& forSession(const std::shared_ptr<transport::Session>& session);
    static const SSLPeerInfo& forSession(const std::shared_ptr<const transport::Session>& session);

    const boost::optional<std::string>& getClusterMembership() const {
        return _clusterMembership;
    }

    void setClusterMembership(boost::optional<std::string> clusterMembership) {
        _clusterMembership = std::move(clusterMembership);
    }

private:
    /**
     * This flag is used to indicate if the underlying socket is using TLS or not. A default
     * constructor of SSLPeerInfo indicates that TLS is not being used, and the other
     * constructors set its value to true.
     */
    bool _isTLS = false;
    SSLX509Name _subjectName;
    boost::optional<std::string> _sniName;
    stdx::unordered_set<RoleName> _roles;
    boost::optional<std::string> _clusterMembership;
};
}  // namespace mongo
