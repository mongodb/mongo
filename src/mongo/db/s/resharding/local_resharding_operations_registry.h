/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/s/resharding/resharding_metrics_common.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"

MONGO_MOD_PUBLIC;

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
    boost::optional<Operation> getOperation(const NamespaceString& nss) const;

private:
    mutable ObservableMutex<std::shared_mutex> _mutex;
    stdx::unordered_map<NamespaceString, Operation> _operations;
};
}  // namespace mongo
