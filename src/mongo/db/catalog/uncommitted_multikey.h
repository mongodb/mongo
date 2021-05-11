/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include <map>

namespace mongo {
class Collection;
class UncommittedMultikey {
public:
    /**
     * Wrapper class for the resources used by UncommittedMultikey
     * Store uncommitted multikey updates as a decoration on the OperationContext. We can use the
     * raw Collection pointer as a key as there cannot be any concurrent MODE_X writer that clones
     * the Collection into a new instance.
     */
    using MultikeyMap = std::map<const Collection*, BSONCollectionCatalogEntry::MetaData>;

    std::shared_ptr<MultikeyMap> releaseResources() {
        return std::move(_resourcesPtr);
    }

    void receiveResources(std::shared_ptr<MultikeyMap> resources) {
        _resourcesPtr = std::move(resources);
    }

    static UncommittedMultikey& get(OperationContext* opCtx);

    std::shared_ptr<MultikeyMap>& resources() {
        return _resourcesPtr;
    }

private:
    std::shared_ptr<MultikeyMap> _resourcesPtr;
};
}  // namespace mongo
