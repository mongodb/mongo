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

#include "mongo/db/query/replanning_required_info.h"

#include "mongo/base/init.h"

namespace mongo {
namespace {
constexpr StringData kCacheModeFieldName = "cacheMode";
constexpr StringData kOldPlanHashFieldName = "oldPlanHash";

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ReplanningRequiredInfo);

}  // namespace

ReplanningRequiredInfo::ReplanningRequiredInfo(plan_cache_util::CacheMode cacheMode,
                                               size_t oldPlanHash)
    : _cacheMode(cacheMode), _oldPlanHash(oldPlanHash) {}

void ReplanningRequiredInfo::serialize(BSONObjBuilder* bob) const {
    bob->append(kCacheModeFieldName, _cacheMode == plan_cache_util::CacheMode::AlwaysCache);

    // The hash is of type size_t, which is unsigned and BSON cannot store unsigned values. Storing
    // it as a string in the BSON allows us to convert it back to a size_t during parsing without
    // information loss. Use hex format to be in line with other hashes.
    bob->append(kOldPlanHashFieldName, fmt::format("{:X}", _oldPlanHash));
}

std::shared_ptr<const ErrorExtraInfo> ReplanningRequiredInfo::parse(const BSONObj& obj) {
    const auto& cacheModeElem = obj.getField(kCacheModeFieldName);
    uassert(8746600,
            fmt::format("ReplanningRequiredInfo was missing '{}' field", kCacheModeFieldName),
            !cacheModeElem.eoo());

    const auto& oldPlanHashElem = obj.getField(kOldPlanHashFieldName);
    uassert(8746602,
            fmt::format("ReplanningRequiredInfo was missing '{}' field", kOldPlanHashFieldName),
            !oldPlanHashElem.eoo());

    const auto& hashStr = oldPlanHashElem.String();
    size_t oldPlanHash;
    auto fromCharsRes = std::from_chars(
        hashStr.data(), hashStr.data() + hashStr.size(), oldPlanHash, 16 /* base */);
    uassert(8746603,
            fmt::format("Failed to parse ReplanningRequiredInfo '{}' field", kOldPlanHashFieldName),
            fromCharsRes.ec == std::errc{});

    return std::make_shared<ReplanningRequiredInfo>(
        plan_cache_util::ConditionalClassicPlanCacheWriter::alwaysOrNeverCacheMode(
            cacheModeElem.Bool()),
        oldPlanHash);
}

plan_cache_util::CacheMode ReplanningRequiredInfo::getCacheMode() const {
    return _cacheMode;
}

size_t ReplanningRequiredInfo::getOldPlanHash() const {
    return _oldPlanHash;
}

}  // namespace mongo
