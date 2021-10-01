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

#include <string>

#include "mongo/base/status_with.h"
#include "mongo/db/service_context.h"

namespace mongo::plan_cache_util {

/**
 * Defines units of planCacheSize parameter.
 */
enum class PlanCacheSizeUnits {
    kPercent,
    kMB,
    kGB,
};

StatusWith<PlanCacheSizeUnits> parseUnitString(const std::string& strUnit);

/**
 * Represents parsed planCacheSize parameter.
 */
struct PlanCacheSizeParameter {
    static StatusWith<PlanCacheSizeParameter> parse(const std::string& str);

    const double size;
    const PlanCacheSizeUnits units;
};

/**
 * Callback called on a change of planCacheSize parameter.
 */
Status onPlanCacheSizeUpdate(const std::string& str);

/**
 * Encapsulates a callback function used to update the PlanCache size when the planCacheSize
 * parameter is updated.
 */
class PlanCacheSizeUpdater {
public:
    virtual ~PlanCacheSizeUpdater() = default;
    virtual void update(ServiceContext* serviceCtx, PlanCacheSizeParameter parameter) = 0;
};

/**
 * Decorated accessor to the PlanCacheSizeUpdater stored in ServiceContext.
 */
extern const Decorable<ServiceContext>::Decoration<std::unique_ptr<PlanCacheSizeUpdater>>
    sbePlanCacheSizeUpdaterDecoration;

}  // namespace mongo::plan_cache_util
