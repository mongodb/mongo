// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/util.h"
#include "mongo/transport/grpc/wire_version_provider.h"
#include "mongo/util/modules.h"

namespace mongo::transport::grpc {

/**
 * A WireVersionProvider whose clusterMaxWireVersion can be manually set to arbitrary values.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] MockWireVersionProvider : public WireVersionProvider {
public:
    MockWireVersionProvider() = default;

    int getClusterMaxWireVersion() const override {
        return _clusterMaxWireVersion.load();
    }

    void setClusterMaxWireVersion(int newVersion) {
        _clusterMaxWireVersion.store(newVersion);
    }

private:
    Atomic<int> _clusterMaxWireVersion{util::constants::kMinimumWireVersion};
};

}  // namespace mongo::transport::grpc
