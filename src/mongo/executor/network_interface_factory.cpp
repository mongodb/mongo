/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/executor/network_interface_factory.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/rpc/metadata/metadata_hook.h"

namespace mongo {
namespace executor {

std::unique_ptr<NetworkInterface> makeNetworkInterface(std::string instanceName) {
    return makeNetworkInterface(std::move(instanceName), nullptr, nullptr);
}

std::unique_ptr<NetworkInterface> makeNetworkInterface(
    std::string instanceName,
    std::unique_ptr<NetworkConnectionHook> hook,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook,
    ConnectionPool::Options connPoolOptions) {

    if (!connPoolOptions.egressTagCloserManager && hasGlobalServiceContext()) {
        connPoolOptions.egressTagCloserManager =
            &EgressTagCloserManager::get(getGlobalServiceContext());
    }

    auto svcCtx = hasGlobalServiceContext() ? getGlobalServiceContext() : nullptr;
    return std::make_unique<NetworkInterfaceTL>(
        instanceName, connPoolOptions, svcCtx, std::move(hook), std::move(metadataHook));
}

}  // namespace executor
}  // namespace mongo
