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

#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/shard_role/lock_manager/resource_catalog.h"

#include <boost/optional/optional.hpp>

// Note, we need to wrap siphash in an extern C block as it's compiled under C symbol rules and this
// file is compiled under C++ symbol rules. As a result, the linker has to know that the symbol here
// is not a C++ one since otherwise it will fail to find it and cause linking issues.
namespace {
extern "C" {
#include <siphash.h>
}
}  // namespace

namespace mongo {

// Hardcoded resource IDs.
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

namespace {
static const std::array<std::byte, 16> kHashingSaltForResourceId = [] {
    SecureRandom entropy;
    std::array<std::byte, 16> result;
    entropy.fill(result.data(), result.size());
    return result;
}();

}  // namespace

uint64_t hashStringDataForResourceId(StringData str, const std::array<std::byte, 16>& salt) {
    // We salt the hash with a given random value to generate randomness in ResourceId selection on
    // every restart. This aids in testing for detecting lock ordering issues.
    uint8_t result[8];
    (void)siphash(str.data(), str.size(), salt.data(), result, sizeof(result));
    return ConstDataView(reinterpret_cast<char*>(result)).read<uint64_t>();
}

ResourceId::ResourceId(ResourceType type, const NamespaceString& nss)
    : _fullHash(fullHash(
          type,
          hashStringDataForResourceId(nss.toStringForResourceId(), kHashingSaltForResourceId))) {
    verifyNoResourceMutex(type);
}

ResourceId::ResourceId(ResourceType type, const DatabaseName& dbName)
    : _fullHash(fullHash(
          type,
          hashStringDataForResourceId(dbName.toStringForResourceId(), kHashingSaltForResourceId))) {
    verifyNoResourceMutex(type);
}

ResourceId::ResourceId(ResourceType type, const TenantId& tenantId)
    : _fullHash{fullHash(
          type, hashStringDataForResourceId(tenantId.toString(), kHashingSaltForResourceId))} {
    verifyNoResourceMutex(type);
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
