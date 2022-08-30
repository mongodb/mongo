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

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/query/plan_cache_size_parameter.h"
#include "mongo/db/service_context.h"

namespace mongo::plan_cache_util {

/**
 * Callback called on a change of planCacheSize parameter.
 */
Status onPlanCacheSizeUpdate(const std::string& str);

/**
 * Callback called on validation of planCacheSize parameter.
 */
Status validatePlanCacheSize(const std::string& str, const boost::optional<TenantId>&);

/**
 * Clears the SBE plan cache. Used to implement 'clearSbeCacheOnParameterChange()' below.
 */
Status clearSbeCacheOnParameterChangeHelper();

/**
 * Hook to delete all SBE plan cache entries when query-related setParameter values are updated at
 * runtime.
 */
constexpr inline auto clearSbeCacheOnParameterChange = [](auto&&) {
    return clearSbeCacheOnParameterChangeHelper();
};

/**
 *  An interface used to modify the SBE plan cache when query setParameters are modified. This is
 *  done via an interface decorating the 'ServiceContext' in order to avoid a link-time dependency
 *  of the query knobs library on the SBE plan cache code.
 */
class OnParamChangeUpdater {
public:
    virtual ~OnParamChangeUpdater() = default;

    /**
     * Resizes the SBE plan cache decorating 'serviceCtx' to the new size given by 'parameter'. If
     * the new cache size is smaller than the old, cache entries are evicted in order to ensure the
     * cache fits within the new size bound.
     */
    virtual void updateCacheSize(ServiceContext* serviceCtx, PlanCacheSizeParameter parameter) = 0;

    /**
     * Deletes all plans from the SBE plan cache decorating 'serviceCtx'.
     */
    virtual void clearCache(ServiceContext* serviceCtx) = 0;
};

/**
 * Decorated accessor to the 'OnParamChangeUpdater' stored in 'ServiceContext'.
 */
extern const Decorable<ServiceContext>::Decoration<std::unique_ptr<OnParamChangeUpdater>>
    sbePlanCacheOnParamChangeUpdater;

}  // namespace mongo::plan_cache_util
