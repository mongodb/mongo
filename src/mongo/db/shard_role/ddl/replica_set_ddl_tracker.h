// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string_view>

namespace mongo {

class [[MONGO_MOD_PRIVATE]] ReplicaSetDDLHook {
public:
    virtual ~ReplicaSetDDLHook() = default;
    virtual std::string_view getName() const = 0;
    virtual void onBeginDDL(OperationContext* opCtx, const std::vector<NamespaceString>& nss) = 0;
    virtual void onEndDDL(OperationContext* opCtx, const std::vector<NamespaceString>& nss) = 0;
};

struct [[MONGO_MOD_PUBLIC]] ReplicaSetDDLOptions {
    // If true, acquire DDL locks in X mode for all affected namespaces.
    // DDL locks are only acquired in replica sets (not shard direct commands). In sharded
    // clusters, DDL coordinators are responsible for acquiring the DDL lock in the DB primary.
    bool acquireDDLLocks = false;
};

class [[MONGO_MOD_PUBLIC]] ReplicaSetDDLTracker {
public:
    ReplicaSetDDLTracker(const ReplicaSetDDLTracker&) = delete;
    ReplicaSetDDLTracker& operator=(const ReplicaSetDDLTracker&) = delete;
    ReplicaSetDDLTracker() = default;

    static void create(ServiceContext* serviceContext);
    static ReplicaSetDDLTracker* get(ServiceContext* opCtx);

    void registerHook(std::unique_ptr<ReplicaSetDDLHook> hook);

    /**
     * Looks up a registered hook by the hook's name.  Calling it with a non-registered
     * service name is a programmer error as all services should be known statically and registered
     * at startup. Since all services live for the lifetime of the mongod process (unlike their
     * Instance objects), there's no concern about the returned pointer becoming invalid.
     */
    ReplicaSetDDLHook* lookupHookByName(std::string_view hookName) const;

    /**
     * Scoped object which calls onBeginDDL for all hooks in the constructor and onEndDDL for
     * all hooks in the destructor. Should be created at the beginning of a replica set DDL command.
     */
    class ScopedReplicaSetDDL {
    public:
        ScopedReplicaSetDDL(OperationContext* opCtx,
                            const std::vector<NamespaceString>& namespaces,
                            std::string_view ddlName = "",
                            const ReplicaSetDDLOptions& options = {});
        ~ScopedReplicaSetDDL();

    private:
        void acquireDDLLocks(OperationContext* opCtx, std::string_view reason);

        const ReplicaSetDDLTracker* const _ddlTracker;
        OperationContext* const _opCtx;
        const std::vector<NamespaceString> _namespaces;
        std::vector<DDLLockManager::ScopedBaseDDLLock> _ddlLocks;
    };

    /**
     * Calls onBeginDDL and onEndDDL for all registered hooks. The scoped helper above should be
     * preferred over direct calls to these functions.
     */
    void onBeginDDL(OperationContext* opCtx, const std::vector<NamespaceString>& namespaces) const;
    void onEndDDL(OperationContext* opCtx, const std::vector<NamespaceString>& namespaces) const;

private:
    StringMap<std::unique_ptr<ReplicaSetDDLHook>> _ddlHooksByName;
};

}  // namespace mongo
