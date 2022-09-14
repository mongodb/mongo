/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/concurrency/lock_manager_defs.h"

namespace mongo {
class ResourceCatalog {
public:
    static ResourceCatalog& get(ServiceContext* scvCtx);

    void add(ResourceId id, const NamespaceString& ns);
    void add(ResourceId id, const DatabaseName& dbName);

    void remove(ResourceId id, const NamespaceString& ns);
    void remove(ResourceId id, const DatabaseName& dbName);

    void clear();

    /**
     * Returns the name of a resource by its id. If the id is not found or it maps to multiple
     * resources, returns boost::none.
     */
    boost::optional<std::string> name(ResourceId id) const;

private:
    void _add(ResourceId id, std::string name);

    void _remove(ResourceId id, const std::string& name);

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ResourceCatalog");
    stdx::unordered_map<ResourceId, StringSet> _resources;
};
}  // namespace mongo
