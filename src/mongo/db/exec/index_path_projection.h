// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * TODO SERVER-113178: Determine whether this class can be reworked to remove local catalog
 * dependency on query module.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] IndexPathProjection {
public:
    IndexPathProjection(std::unique_ptr<projection_executor::ProjectionExecutor> projExec)
        : _exec(std::move(projExec)), _exhaustivePaths(_exec->extractExhaustivePaths()) {
        tassert(7241740, "index path projection requires a Projection Executor", _exec);
    }

    projection_executor::ProjectionExecutor* exec() const {
        return _exec.get();
    }

    const boost::optional<std::set<FieldRef>>& exhaustivePaths() const {
        return _exhaustivePaths;
    }

private:
    // Guaranteed to be non-null.
    std::unique_ptr<projection_executor::ProjectionExecutor> _exec;
    // Store this here to avoid having to recompute it repeatedly, which is expensive.
    boost::optional<std::set<FieldRef>> _exhaustivePaths;
};

using WildcardProjection = IndexPathProjection;

}  // namespace mongo
