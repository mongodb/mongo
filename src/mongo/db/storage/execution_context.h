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

#pragma once

#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/auto_clear_ptr.h"

namespace mongo {

/**
 * Execution context for recycling and reusing temporary objects within the lifetime of a database
 * operation. Users of any members should never leave any state behind.
 */
class StorageExecutionContext {
public:
    static const OperationContext::Decoration<StorageExecutionContext> get;

    StorageExecutionContext();

    // No copy and no move
    StorageExecutionContext(const StorageExecutionContext&) = delete;
    StorageExecutionContext(StorageExecutionContext&&) = delete;
    StorageExecutionContext& operator=(const StorageExecutionContext&) = delete;
    StorageExecutionContext& operator=(StorageExecutionContext&&) = delete;

    AutoClearPtr<KeyStringSet> keys() {
        return makeAutoClearPtr(&_keys);
    }
    AutoClearPtr<KeyStringSet> multikeyMetadataKeys() {
        return makeAutoClearPtr(&_multikeyMetadataKeys);
    }
    AutoClearPtr<MultikeyPaths> multikeyPaths() {
        return makeAutoClearPtr(&_multikeyPaths);
    }

private:
    KeyStringSet _keys;
    KeyStringSet _multikeyMetadataKeys;
    MultikeyPaths _multikeyPaths;
};

}  // namespace mongo
