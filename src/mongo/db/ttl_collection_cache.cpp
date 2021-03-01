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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/ttl_collection_cache.h"

#include <algorithm>

#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangTTLCollectionCacheAfterRegisteringInfo);

namespace {
const auto getTTLCollectionCache = ServiceContext::declareDecoration<TTLCollectionCache>();
}

TTLCollectionCache& TTLCollectionCache::get(ServiceContext* ctx) {
    return getTTLCollectionCache(ctx);
}

void TTLCollectionCache::registerTTLInfo(UUID uuid, const Info& info) {
    {
        stdx::lock_guard<Latch> lock(_ttlInfosLock);
        _ttlInfos[uuid].push_back(info);
    }

    if (MONGO_unlikely(hangTTLCollectionCacheAfterRegisteringInfo.shouldFail())) {
        LOGV2(4664000, "Hanging due to hangTTLCollectionCacheAfterRegisteringInfo fail point");
        hangTTLCollectionCacheAfterRegisteringInfo.pauseWhileSet();
    }
}

void TTLCollectionCache::deregisterTTLInfo(UUID uuid, const Info& info) {
    stdx::lock_guard<Latch> lock(_ttlInfosLock);
    auto infoIt = _ttlInfos.find(uuid);
    fassert(5400705, infoIt != _ttlInfos.end());
    auto& [_, infoVec] = *infoIt;

    auto iter = std::find(infoVec.begin(), infoVec.end(), info);
    fassert(40220, iter != infoVec.end());
    infoVec.erase(iter);
    if (infoVec.empty()) {
        _ttlInfos.erase(infoIt);
    }
}

TTLCollectionCache::InfoMap TTLCollectionCache::getTTLInfos() {
    stdx::lock_guard<Latch> lock(_ttlInfosLock);
    return _ttlInfos;
}
};  // namespace mongo
