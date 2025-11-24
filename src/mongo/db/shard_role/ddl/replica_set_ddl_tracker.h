/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>

namespace mongo {

class ReplicaSetDDLHook {
public:
    virtual ~ReplicaSetDDLHook() = default;
    virtual StringData getName() const = 0;
    virtual void onBeginDDL(OperationContext* opCtx, const std::vector<NamespaceString>& nss) = 0;
    virtual void onEndDDL(OperationContext* opCtx, const std::vector<NamespaceString>& nss) = 0;
};

class ReplicaSetDDLTracker {
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
    ReplicaSetDDLHook* lookupHookByName(StringData hookName) const;

    /**
     * Scoped object which calls onBeginDDL for all hooks in the constructor and onEndDDL for
     * all hooks in the destructor. Should be created at the beginning of a replica set DDL command.
     */
    class ScopedReplicaSetDDL {
    public:
        ScopedReplicaSetDDL(OperationContext* opCtx,
                            const std::vector<NamespaceString>& namespaces);
        ~ScopedReplicaSetDDL();

    private:
        const ReplicaSetDDLTracker* const _ddlTracker;
        OperationContext* const _opCtx;
        const std::vector<NamespaceString> _namespaces;
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
