// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/replanning_required_info.h"

#include "mongo/base/init.h"

#include <string_view>

namespace mongo {
namespace {
constexpr std::string_view kCacheModeFieldName = "cacheMode";
constexpr std::string_view kOldPlanHashFieldName = "oldPlanHash";

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
