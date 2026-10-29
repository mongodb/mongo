// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/metadata/audit_attrs_gen.h"
#include "mongo/rpc/metadata/audit_metadata_gen.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional.hpp>

namespace mongo::rpc {

class [[MONGO_MOD_PUBLIC]] AuditClientAttrs : public rpc::AuditClientAttrsBase {
public:
    AuditClientAttrs(HostAndPort local,
                     HostAndPort remote,
                     HostAndPort directRemote,
                     std::vector<HostAndPort> proxies = {},
                     bool isImpersonating = false)
        : AuditClientAttrsBase(
              std::move(local), std::move(remote), std::move(proxies), isImpersonating) {
        setDirectRemote(std::move(directRemote));
    }

    explicit AuditClientAttrs(const BSONObj& obj);

    /**
     * Returns the literal, directly-connected TCP peer address. 'directRemote' is optional on the
     * wire for backwards-compatibility, so when it is absent (e.g. forwarded by a peer running an
     * older binary) we fall back to 'remote', which is the best address available.
     */
    HostAndPort getDirectRemote() const {
        if (const auto& directRemote = AuditClientAttrsBase::getDirectRemote()) {
            return *directRemote;
        }
        return getRemote();
    }

    static boost::optional<AuditClientAttrs> get(Client* client);
    static void set(Client* client, AuditClientAttrs clientAttrs);
    static void resetToPeerClient(Client* client);
    static void reset(Client* client);

    ImpersonatedClientMetadata generateClientMetadataObj();
};

}  // namespace mongo::rpc
