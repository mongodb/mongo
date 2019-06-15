/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class Collection;
class IndexDescriptor;
class OperationContext;

// Indicates which protocol an index build is using.
enum class IndexBuildProtocol {
    /**
     * Refers to the pre-FCV 4.2 index build protocol for building indexes in replica sets.
     * Index builds must complete on the primary before replicating, and are not resumable in
     * any scenario.
     */
    kSinglePhase,
    /**
     * Refers to the FCV 4.2 two-phase index build protocol for building indexes in replica
     * sets. Indexes are built simultaneously on all nodes and are resumable during the draining
     * phase.
     */
    kTwoPhase
};

class CollectionCatalogEntry {
public:
    /**
     * Incremented when breaking changes are made to the index build procedure so that other servers
     * know whether or not to resume or discard unfinished index builds.
     */
    static const int kIndexBuildVersion = 1;

    CollectionCatalogEntry(StringData ns) : _ns(ns) {}
    virtual ~CollectionCatalogEntry() {}

    const NamespaceString& ns() const {
        return _ns;
    }

    void setNs(NamespaceString ns) {
        _ns = std::move(ns);
    }

    // ------- indexes ----------


    virtual RecordStore* getRecordStore() = 0;
    virtual const RecordStore* getRecordStore() const = 0;

private:
    NamespaceString _ns;
};
}
