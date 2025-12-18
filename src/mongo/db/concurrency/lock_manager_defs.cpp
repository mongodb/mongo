/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/concurrency/lock_manager_defs.h"

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/concurrency/resource_catalog.h"

#include <boost/optional/optional.hpp>

namespace mongo {

// Hardcoded resource IDs.
const ResourceId resourceIdLocalDB = ResourceId(RESOURCE_DATABASE, DatabaseName::kLocal);
const ResourceId resourceIdAdminDB = ResourceId(RESOURCE_DATABASE, DatabaseName::kAdmin);
const ResourceId resourceIdGlobal =
    ResourceId(RESOURCE_GLOBAL, static_cast<uint8_t>(ResourceGlobalId::kGlobal));
const ResourceId resourceIdMultiDocumentTransactionsBarrier = ResourceId(
    RESOURCE_GLOBAL, static_cast<uint8_t>(ResourceGlobalId::kMultiDocumentTransactionsBarrier));
const ResourceId resourceIdReplicationStateTransitionLock = ResourceId(
    RESOURCE_GLOBAL, static_cast<uint8_t>(ResourceGlobalId::kReplicationStateTransitionLock));

std::string toStringForLogging(const ResourceId& rId) {
    StringBuilder ss;
    const auto type = rId.getType();
    ss << "{" << rId._fullHash << ": " << resourceTypeName(type) << ", " << rId.getHashId();
    if (type == RESOURCE_DATABASE || type == RESOURCE_COLLECTION || type == RESOURCE_MUTEX ||
        type == RESOURCE_DDL_DATABASE || type == RESOURCE_DDL_COLLECTION) {
        if (auto resourceName = ResourceCatalog::get().name(rId)) {
            ss << ", " << *resourceName;
        }
    }
    ss << "}";

    return ss.str();
}

std::string ResourceId::toStringForErrorMessage() const {
    StringBuilder ss;
    const auto type = getType();
    ss << "{" << resourceTypeName(type);
    switch (type) {
        case RESOURCE_GLOBAL:
            ss << " : " << getHashId();
            break;
        case RESOURCE_DATABASE:
        case RESOURCE_COLLECTION:
        case RESOURCE_DDL_DATABASE:
        case RESOURCE_DDL_COLLECTION:
        case RESOURCE_MUTEX:
            if (auto resourceName = ResourceCatalog::get().name(*this)) {
                ss << " : " << *resourceName;
            }
            break;
        case ResourceTypesCount:
        case RESOURCE_INVALID:
        case RESOURCE_METADATA:
        case RESOURCE_TENANT:
            break;
    }
    ss << "}";

    return ss.str();
}

void LockRequest::initNew(Locker* locker, LockGrantNotification* notify) {
    this->locker = locker;
    this->notify = notify;

    enqueueAtFront = false;
    compatibleFirst = false;
    partitioned = false;

    status = STATUS_NEW;
    mode = MODE_NONE;

    unlockPending = 0;
    recursiveCount = 1;

    lock = nullptr;
    partitionedLock = nullptr;

    prev = nullptr;
    next = nullptr;
}

}  // namespace mongo
