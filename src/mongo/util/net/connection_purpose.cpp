// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/util/net/connection_purpose.h"

#include "mongo/db/repl/repl_set_config.h"

namespace mongo {
namespace {
const auto getPeerIdForSession = transport::Session::declareDecoration<boost::optional<int64_t>>();
const auto getPurposeForSession = transport::Session::declareDecoration<ConnectionPurpose>();
}  // namespace

const otel::metrics::AttributeDefinition<int64_t>& replicationIdAttribute() {
    static StaticImmortal def = [] {
        otel::metrics::AttributeDefinition<int64_t> def{.name = "peer_id"};
        def.values.reserve(repl::ReplSetConfig::kMaxMembers);
        for (size_t i = 0; i < repl::ReplSetConfig::kMaxMembers; i++) {
            def.values.push_back(i);
        }
        return def;
    }();
    return *def;
}

boost::optional<int64_t>& getReplicationId(transport::Session* session) {
    return getPeerIdForSession(session);
}

ConnectionPurpose& getConnectionPurpose(transport::Session* session) {
    return getPurposeForSession(session);
}
}  // namespace mongo
