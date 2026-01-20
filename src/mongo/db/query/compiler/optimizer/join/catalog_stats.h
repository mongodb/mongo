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
#include "mongo/stdx/unordered_map.h"

namespace mongo::join_ordering {

/**
 * Statisitics for a single collection.
 */
struct CollectionStats {
    // Total number of on-disk bytes currently allocated to data pages that are still in use
    // by this collection's record data, after storage-engine compression. Intended to be derived
    // from the record store's on-disk size metrics (e.g. storageSize - freeStorageSize). This
    // includes both live document data and any unused space on those pages, but excludes fully
    // free/reusable pages and all index storage.
    double allocatedDataPageBytes;

    // Approximate size, in bytes, of a single on-disk data page for this collection after
    // storage-engine compression. The optimizer uses this as the I/O granularity when converting
    // between bytes and page reads in cost estimates. Default to 32KiB if not specified.
    double pageSizeBytes{32 * 1024};
};

/**
 * Statistics extracted from the catalog useful for cost estimation.
 */
struct CatalogStats {
    stdx::unordered_map<NamespaceString, CollectionStats> collStats;
};

}  // namespace mongo::join_ordering
