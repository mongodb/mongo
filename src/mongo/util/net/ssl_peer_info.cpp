// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/net/ssl_peer_info.h"

namespace mongo {
namespace {
const auto peerInfoForSession =
    transport::Session::declareDecoration<std::shared_ptr<const SSLPeerInfo>>();
}

std::shared_ptr<const SSLPeerInfo>& SSLPeerInfo::forSession(
    const std::shared_ptr<transport::Session>& session) {
    return peerInfoForSession(session.get());
}

std::shared_ptr<const SSLPeerInfo> SSLPeerInfo::forSession(
    const std::shared_ptr<const transport::Session>& session) {
    return peerInfoForSession(session.get());
}

void SSLPeerInfo::appendPeerInfoToVector(std::vector<std::string>& elements) const {
    elements.push_back(_subjectName.toString());
    if (_sniName) {
        elements.push_back(*_sniName);
    }
    if (_clusterMembership) {
        elements.push_back(*_clusterMembership);
    }
}

}  // namespace mongo
