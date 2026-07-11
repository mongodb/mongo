// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata/impersonated_client_session.h"

namespace mongo::rpc {

ImpersonatedClientSessionGuard::ImpersonatedClientSessionGuard(
    Client* client, const ImpersonatedClientMetadata& parsedClientMetadata)
    : _client(client) {
    auto session = client->session();
    auto local = session ? session->local() : HostAndPort{};
    const auto& hosts = parsedClientMetadata.getHosts();

    uassert(ErrorCodes::BadValue, "$audit must contain at least one host", hosts.size() > 0);

    auto remote = hosts[0];
    // $audit only carries the (possibly proxy-asserted) remote address; there is no distinct
    // literal peer address to forward across the wire, so directRemote just mirrors remote here.
    auto directRemote = remote;
    std::vector<HostAndPort> intermediates;
    for (size_t i = 1; i < hosts.size(); ++i) {
        // In rare occasions a node will send a request to itself (e.g.
        // checkCatalogConsistencyAcrossShards), adding itself as an intermediate. We check if the
        // host is the same to skip it.
        if (MONGO_unlikely(hosts[i] == local)) {
            continue;
        }
        intermediates.push_back(hosts[i]);
    }

    _oldClientAttrs = AuditClientAttrs::get(client);

    AuditClientAttrs::set(client,
                          AuditClientAttrs(std::move(local),
                                           std::move(remote),
                                           std::move(directRemote),
                                           std::move(intermediates),
                                           true /* isImpersonating */));
}

ImpersonatedClientSessionGuard::~ImpersonatedClientSessionGuard() {
    if (_oldClientAttrs) {
        AuditClientAttrs::set(_client, _oldClientAttrs.value());
    } else {
        AuditClientAttrs::reset(_client);
    }
}

}  // namespace mongo::rpc
