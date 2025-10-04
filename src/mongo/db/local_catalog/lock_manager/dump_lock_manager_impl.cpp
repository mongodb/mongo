/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/lock_manager/dump_lock_manager_impl.h"

#include "mongo/base/shim.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/lock_manager/dump_lock_manager.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

template <typename T>
std::string formatHex(T&& x) {
    return fmt::format("{:#x}", x);
}

std::string formatPtr(const void* x) {
    return formatHex(reinterpret_cast<uintptr_t>(x));
}

void dumpLockManagerImpl() {
    auto service = getGlobalServiceContext();
    auto lockManager = LockManager::get(service);

    BSONArrayBuilder locks;
    lockManager->getLockInfoArray(getLockerIdToClientMap(service), true, nullptr, &locks);
    LOGV2_OPTIONS(20521,
                  logv2::LogTruncation::Disabled,
                  "lock manager dump",
                  "addr"_attr = formatPtr(lockManager),
                  "locks"_attr = locks.arr());
}

auto dumpLockManagerRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(dumpLockManager, dumpLockManagerImpl);

}  // namespace

std::map<LockerId, BSONObj> getLockerIdToClientMap(ServiceContext* serviceContext) {
    std::map<LockerId, BSONObj> lockToClientMap;

    for (ServiceContext::LockedClientsCursor cursor(serviceContext);
         Client* client = cursor.next();) {
        invariant(client);

        stdx::lock_guard<Client> lk(*client);
        const OperationContext* clientOpCtx = client->getOperationContext();

        // Operation context specific information
        if (clientOpCtx) {
            BSONObjBuilder infoBuilder;
            // The client information
            client->reportState(infoBuilder);

            infoBuilder.append("opid", static_cast<int>(clientOpCtx->getOpID()));
            LockerId lockerId = shard_role_details::getLocker(clientOpCtx)->getId();
            lockToClientMap.insert({lockerId, infoBuilder.obj()});
        }
    }

    return lockToClientMap;
}

}  // namespace mongo
