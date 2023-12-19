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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/db/dump_lock_manager.h"

#include "mongo/base/shim.h"
#include "mongo/db/concurrency/lock_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

template <typename T>
std::string formatHex(T&& x) {
    return format(FMT_STRING("{:#x}"), x);
}

std::string formatPtr(const void* x) {
    return formatHex(reinterpret_cast<uintptr_t>(x));
}

void dumpLockManagerImpl() {
    auto service = getGlobalServiceContext();
    auto lockManager = LockManager::get(service);

    BSONArrayBuilder locks;
    lockManager->getLockInfoArray(LockManager::getLockToClientMap(service), true, nullptr, &locks);
    LOGV2_OPTIONS(20521,
                  logv2::LogTruncation::Disabled,
                  "lock manager dump",
                  "addr"_attr = formatPtr(lockManager),
                  "locks"_attr = locks.arr());
}

auto dumpLockManagerRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(dumpLockManager, dumpLockManagerImpl);
}  // namespace
}  // namespace mongo
