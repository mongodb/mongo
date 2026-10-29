// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"

#include "mongo/base/init.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/shard_role/lock_manager/resource_catalog.h"

#include <boost/optional/optional.hpp>

// Note, we need to wrap siphash in an extern C block as it's compiled under C symbol rules and this
// file is compiled under C++ symbol rules. As a result, the linker has to know that the symbol here
// is not a C++ one since otherwise it will fail to find it and cause linking issues.
namespace {
extern "C" {
#include <string_view>

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

namespace {

/**
 * Returns the human-readable name of 'rId', when it has one. A global resource is named after its
 * ResourceGlobalId, while the resources tracked by the ResourceCatalog are named after the
 * namespace, database or label they were registered with.
 */
boost::optional<std::string> resourceName(const ResourceId& rId) {
    switch (rId.getType()) {
        case RESOURCE_GLOBAL:
            // The hash id of a global resource is its ResourceGlobalId.
            return std::string(
                resourceGlobalIdName(static_cast<ResourceGlobalId>(rId.getHashId())));
        case RESOURCE_DATABASE:
        case RESOURCE_COLLECTION:
        case RESOURCE_DDL_DATABASE:
        case RESOURCE_DDL_COLLECTION:
        case RESOURCE_MUTEX:
            return ResourceCatalog::get().name(rId);
        case ResourceTypesCount:
        case RESOURCE_INVALID:
        case RESOURCE_METADATA:
        case RESOURCE_TENANT:
        default:
            return boost::none;
    }
}

}  // namespace

std::string toStringForLogging(const ResourceId& rId) {
    StringBuilder ss;
    ss << "{" << rId._fullHash << ": " << resourceTypeName(rId.getType()) << ", "
       << rId.getHashId();
    if (auto name = resourceName(rId)) {
        ss << ", " << *name;
    }
    ss << "}";

    return ss.str();
}

std::string ResourceId::toStringForErrorMessage() const {
    StringBuilder ss;
    ss << "{" << resourceTypeName(getType());
    if (auto name = resourceName(*this)) {
        ss << " : " << *name;
    }
    ss << "}";

    return ss.str();
}

namespace {
static std::array<std::byte, 16> kHashingSaltForResourceId;

MONGO_INITIALIZER(HashingSaltInitialization)(InitializerContext*) {
    SecureRandom entropy;
    entropy.fill(kHashingSaltForResourceId.data(), kHashingSaltForResourceId.size());
}

}  // namespace

uint64_t hashStringDataForResourceId(std::string_view str, const std::array<std::byte, 16>& salt) {
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
