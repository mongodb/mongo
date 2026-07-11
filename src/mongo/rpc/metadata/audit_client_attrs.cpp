// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata/audit_client_attrs.h"

namespace mongo::rpc {
namespace {

const auto getAuditClientAttrs =
    Client::declareDecoration<synchronized_value<boost::optional<AuditClientAttrs>>>();

}  // namespace

AuditClientAttrs::AuditClientAttrs(const BSONObj& obj) {
    AuditClientAttrsBase::parseProtected(obj);
}

boost::optional<AuditClientAttrs> AuditClientAttrs::get(Client* client) {
    return getAuditClientAttrs(client).get();
}

void AuditClientAttrs::set(Client* client, AuditClientAttrs clientAttrs) {
    *getAuditClientAttrs(client) = std::move(clientAttrs);
}

void AuditClientAttrs::resetToPeerClient(Client* client) {
    auto session = client->session();
    if (!session) {
        reset(client);
        return;
    }

    auto local = session->local();
    auto remote = session->getSourceRemoteEndpoint();
    auto directRemote = session->remote();
    std::vector<HostAndPort> proxies;
    if (auto proxyEndpoint = session->getProxiedDstEndpoint()) {
        proxies.push_back(*proxyEndpoint);
    }

    *getAuditClientAttrs(client) = rpc::AuditClientAttrs(std::move(local),
                                                         std::move(remote),
                                                         std::move(directRemote),
                                                         std::move(proxies),
                                                         false /* isImpersonating */);
}

void AuditClientAttrs::reset(Client* client) {
    *getAuditClientAttrs(client) = boost::none;
}

ImpersonatedClientMetadata AuditClientAttrs::generateClientMetadataObj() {
    ImpersonatedClientMetadata clientMetadata;

    std::vector<HostAndPort> hosts;
    hosts.push_back(getRemote());
    hosts.push_back(getLocal());

    if (const auto& intermediates = getProxies(); !intermediates.empty()) {
        for (const auto& address : intermediates) {
            hosts.push_back(address);
        }
    }

    clientMetadata.setHosts(std::move(hosts));

    return clientMetadata;
}

}  // namespace mongo::rpc
