// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Mongot (search) configuration options
 */
struct [[MONGO_MOD_PUBLIC]] MongotParams {
    static Status onSetHost(const std::string&);
    static Status onValidateHost(std::string_view str, const boost::optional<TenantId>&);

    MongotParams();

    bool enabled = false;
    std::string host;
    bool skipAuthToMongot = false;
    bool useGRPC = false;

    Atomic<int> minConnections;
    Atomic<int> maxConnections;
    transport::ConnectSSLMode sslMode;
};

[[MONGO_MOD_PUBLIC]] extern MongotParams globalMongotParams;

}  // namespace mongo
