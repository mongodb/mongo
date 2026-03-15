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

#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"

MONGO_MOD_PUBLIC;

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
