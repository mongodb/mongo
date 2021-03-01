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

#include "mongo/stdx/variant.h"
#include <string>
#include <vector>

#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/uuid.h"

/**
 * Caches the set of collections containing a TTL index.
 * This class is thread safe.
 */
namespace mongo {

class TTLCollectionCache {
public:
    static TTLCollectionCache& get(ServiceContext* ctx);

    // Specifies that a collection is clustered by _id and is TTL.
    class ClusteredId : public stdx::monostate {};
    // Names an index that is TTL.
    using IndexName = std::string;

    // Specifies how a collection should expire data with TTL.
    using Info = stdx::variant<ClusteredId, IndexName>;

    // Caller is responsible for ensuring no duplicates are registered.
    void registerTTLInfo(UUID uuid, const Info& info);
    void deregisterTTLInfo(UUID uuid, const Info& info);

    using InfoMap = stdx::unordered_map<UUID, std::vector<Info>, UUID::Hash>;
    InfoMap getTTLInfos();

private:
    Mutex _ttlInfosLock = MONGO_MAKE_LATCH("TTLCollectionCache::_ttlInfosLock");
    InfoMap _ttlInfos;
};
}  // namespace mongo
