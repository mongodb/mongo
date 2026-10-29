// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/network_interface.h"


namespace mongo {
namespace executor {

NetworkInterface::NetworkInterface() {}
NetworkInterface::~NetworkInterface() {}

MONGO_FAIL_POINT_DEFINE(networkInterfaceHangCommandsAfterAcquireConn);
MONGO_FAIL_POINT_DEFINE(networkInterfaceDelayCommandsAfterAcquireConn);
MONGO_FAIL_POINT_DEFINE(networkInterfaceCommandsFailedWithErrorCode);
MONGO_FAIL_POINT_DEFINE(networkInterfaceShouldNotKillPendingRequests);

}  // namespace executor
}  // namespace mongo
