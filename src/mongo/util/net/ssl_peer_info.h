// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/session.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/ssl_types.h"

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {
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

    // Similar to the above constructor, but allows explicitly setting _isTLS to false for the case
    // where we want to populate SSLPeerInfo from proxy protocol TLVs on a non-TLS connection. In
    // this case, we can still populate the SNI and role information from the proxy protocol header.
    SSLPeerInfo(bool isTLS,
                SSLX509Name subjectName,
                boost::optional<std::string> sniName = {},
                stdx::unordered_set<RoleName> roles = {})
        : _isTLS(isTLS),
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

    static std::shared_ptr<const SSLPeerInfo>& forSession(
        const std::shared_ptr<transport::Session>& session);
    static std::shared_ptr<const SSLPeerInfo> forSession(
        const std::shared_ptr<const transport::Session>& session);

    const boost::optional<std::string>& getClusterMembership() const {
        return _clusterMembership;
    }

    void setClusterMembership(boost::optional<std::string> clusterMembership) {
        _clusterMembership = std::move(clusterMembership);
    }

    void appendPeerInfoToVector(std::vector<std::string>& elements) const;

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
