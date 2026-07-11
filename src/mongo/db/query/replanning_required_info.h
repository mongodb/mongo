// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/util/modules.h"

#include <memory>


namespace mongo {

/**
 * The error code ErrorCodes::ReplanningRequired is used whenever the query system detects that we
 * should replan this query. This class stores information used in that process.
 */
class [[MONGO_MOD_PUBLIC]] ReplanningRequiredInfo final : public ErrorExtraInfo {
public:
    // Required member of every ErrorExtraInfo.
    static constexpr auto code = ErrorCodes::ReplanningRequired;

    ReplanningRequiredInfo(plan_cache_util::CacheMode cacheMode, size_t oldPlanHash);

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

    plan_cache_util::CacheMode getCacheMode() const;

    size_t getOldPlanHash() const;

private:
    plan_cache_util::CacheMode _cacheMode;
    size_t _oldPlanHash;
};

}  // namespace mongo
