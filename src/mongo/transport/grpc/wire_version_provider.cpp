// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/wire_version_provider.h"

#include "mongo/db/wire_version.h"

namespace mongo::transport::grpc {

int WireVersionProvider::getClusterMaxWireVersion() const {
    return WireSpec::getWireSpec(getGlobalServiceContext())
        .getIncomingExternalClient()
        .maxWireVersion;
}

}  // namespace mongo::transport::grpc
