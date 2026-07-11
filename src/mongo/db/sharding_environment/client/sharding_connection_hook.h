// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class DBClientBase;

/**
 * Intercepts creation of sharded connections and transparently performs the internal
 * authentication on them.
 */
class [[MONGO_MOD_PUBLIC]] ShardingConnectionHook : public DBConnectionHook {
public:
    ShardingConnectionHook(std::unique_ptr<rpc::EgressMetadataHook> egressHook);

    void onCreate(DBClientBase* conn) override;
    void onRelease(DBClientBase* conn) override;

private:
    // Use the implementation of the metadata readers and writers in ShardingEgressMetadataHook,
    // since that is the hook for Network Interface ASIO and this hook is to be deprecated.
    std::unique_ptr<rpc::EgressMetadataHook> _egressHook;
};

}  // namespace mongo
