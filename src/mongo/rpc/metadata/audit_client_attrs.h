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

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"

namespace mongo::rpc {

class AuditClientAttrs {
public:
    AuditClientAttrs(HostAndPort local, HostAndPort remote, std::vector<HostAndPort> proxies)
        : _local(std::move(local)), _remote(std::move(remote)), _proxies(std::move(proxies)) {}

    AuditClientAttrs(HostAndPort local, HostAndPort remote)
        : AuditClientAttrs(std::move(local), std::move(remote), {}) {}

    static boost::optional<AuditClientAttrs> get(Client* client);
    static void set(Client* client, AuditClientAttrs clientAttrs);

    const HostAndPort& getLocal() const {
        return _local;
    }

    const HostAndPort& getRemote() const {
        return _remote;
    }

    const std::vector<HostAndPort>& getProxiedEndpoints() const {
        return _proxies;
    }

private:
    HostAndPort _local;
    HostAndPort _remote;
    std::vector<HostAndPort> _proxies;
};

}  // namespace mongo::rpc
