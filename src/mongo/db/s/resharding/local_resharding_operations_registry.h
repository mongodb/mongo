// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/resharding_metrics_common.h"
#include "mongo/platform/atomic.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
class LocalReshardingOperationsRegistry {
public:
    using Role = ReshardingMetricsCommon::Role;
    struct Operation {
        CommonReshardingMetadata metadata;
        stdx::unordered_set<Role> roles;
    };

    static LocalReshardingOperationsRegistry& get();

    void registerOperation(Role role, const CommonReshardingMetadata& metadata);
    void unregisterOperation(Role role, const CommonReshardingMetadata& metadata);
    void clearOperationsForRole(Role role);
    boost::optional<Operation> getOperation(const NamespaceString& nss) const;
    boost::optional<CommonReshardingMetadata> getDonorMetadata(const NamespaceString& nss) const;

    void resyncFromDisk(OperationContext* opCtx, std::string_view reason);

    void reportForServerStatus(BSONObjBuilder* bob) const;

private:
    using UuidToOperation = stdx::unordered_map<UUID, Operation>;

    mutable ObservableMutex<std::shared_mutex> _mutex;
    stdx::unordered_map<NamespaceString, UuidToOperation> _namespaceToOperations;

    Atomic<int64_t> _registrationCount{0};
    Atomic<int64_t> _unregistrationCount{0};
    Atomic<int64_t> _resyncCount{0};
    Atomic<int64_t> _currentOperationCount{0};
};

namespace resharding {
/**
 * Throws ReshardCollectionInProgress if the registry contains an entry for the given namespace.
 */
void throwIfReshardingInProgress(const NamespaceString& nss);
}  // namespace resharding

}  // namespace mongo
