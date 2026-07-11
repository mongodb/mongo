// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
namespace mongo {
class CollectionPtr;

/**
 * Context about outside environment when restoring a PlanExecutor.
 *
 * Contains reference to a CollectionPtr owned by an AutoGetCollection lock helper to be used by the
 * RequiresCollectionStage plan stage.
 */
class [[MONGO_MOD_PUBLIC]] RestoreContext {
public:
    enum class RestoreType {
        kExternal,  // Restore on the PlanExecutor by an external call
        kYield      // Internal restore after yield
    };

    RestoreContext() = delete;
    /* implicit */ RestoreContext(const CollectionPtr* coll) : _collection(coll) {}
    /* implicit */ RestoreContext(RestoreType type, const CollectionPtr* coll)
        : _type(type), _collection(coll) {}

    RestoreType type() const {
        return _type;
    }
    const CollectionPtr* collection() const {
        return _collection;
    }

private:
    RestoreType _type = RestoreType::kExternal;
    const CollectionPtr* _collection;
};
}  // namespace mongo
