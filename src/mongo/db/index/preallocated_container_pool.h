// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A pool of preallocated objects that could be reused within the lifetime of a database operation.
 * Users of any members should never leave any state behind.
 */
class PreallocatedContainerPool {
private:
    struct Clear {
        void operator()(auto* c) const {
            c->clear();
        }
    };

public:
    /** Non-owning smart pointer that calls clear() on destruction. */
    template <typename C>
    using ClearingPtr = std::unique_ptr<C, Clear>;

    static const OperationContext::Decoration<PreallocatedContainerPool> get;

    ClearingPtr<KeyStringSet> keys() {
        return _makeClearingPtr(&_keys);
    }
    ClearingPtr<KeyStringSet> multikeyMetadataKeys() {
        return _makeClearingPtr(&_multikeyMetadataKeys);
    }
    ClearingPtr<MultikeyPaths> multikeyPaths() {
        return _makeClearingPtr(&_multikeyPaths);
    }

private:
    template <typename C>
    static ClearingPtr<C> _makeClearingPtr(C* c) {
        // It should always be cleared when it is accessed
        dassert(c);
        dassert(c->empty());
        return ClearingPtr<C>{c};
    }

    KeyStringSet _keys;
    KeyStringSet _multikeyMetadataKeys;
    MultikeyPaths _multikeyPaths;
};

}  // namespace mongo
