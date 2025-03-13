/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
