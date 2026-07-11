// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/dump_lock_manager_impl.h"

#include "mongo/base/shim.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager.h"
#include "mongo/db/shard_role/transaction_resources.h"
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

        std::lock_guard<Client> lk(*client);
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
