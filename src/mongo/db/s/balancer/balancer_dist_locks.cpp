/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/balancer_dist_locks.h"
#include "mongo/db/operation_context.h"

namespace mongo {

BalancerDistLocks::~BalancerDistLocks() {
    invariant(_distLocksByCollection.empty(),
              "Attempting to destroy the keychain while still holding distributed locks");
}

Status BalancerDistLocks::acquireFor(OperationContext* opCtx, const NamespaceString& nss) {
    auto it = _distLocksByCollection.find(nss);
    if (it != _distLocksByCollection.end()) {
        ++it->second.references;
        return Status::OK();
    } else {
        boost::optional<DistLockManager::ScopedLock> scopedLock;
        try {
            scopedLock.emplace(DistLockManager::get(opCtx)->lockDirectLocally(
                opCtx,
                nss.ns(),
                "moveRange" /* reason */,
                DistLockManager::kSingleLockAttemptTimeout));

            const std::string whyMessage(str::stream()
                                         << "Migrating chunk(s) in collection " << nss.ns());
            uassertStatusOK(DistLockManager::get(opCtx)->lockDirect(
                opCtx, nss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout));
        } catch (const DBException& ex) {
            return ex.toStatus(str::stream() << "Could not acquire collection lock for " << nss.ns()
                                             << " to migrate chunks");
        }
        ReferenceCountedLock refCountedLock(std::move(*scopedLock));
        _distLocksByCollection.insert(std::make_pair(nss, std::move(refCountedLock)));
    }
    return Status::OK();
}

void BalancerDistLocks::releaseFor(OperationContext* opCtx, const NamespaceString& nss) {
    auto it = _distLocksByCollection.find(nss);
    if (it == _distLocksByCollection.end()) {
        return;
    } else if (it->second.references == 1) {
        DistLockManager::get(opCtx)->unlock(opCtx, nss.ns());
        _distLocksByCollection.erase(it);
    } else {
        --it->second.references;
    }
}


}  // namespace mongo
