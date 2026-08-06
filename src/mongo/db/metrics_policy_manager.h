// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/metrics_category_gen.h"
#include "mongo/db/metrics_filtering_util.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class OperationContext;
using PathMatcherNode = metrics_filtering_util::PathMatcherNode;

/**
 * This class manages metrics filtering policies.
 */
class [[MONGO_MOD_OPEN]] MetricsPolicyManager {
public:
    virtual ~MetricsPolicyManager() = default;

    /**
     * Returns the MetricsPolicyManager from a ServiceContext. The manager is guaranteed to be
     * set during ServiceContext initialization.
     */
    static MetricsPolicyManager& get(ServiceContext* svcCtx);
    static MetricsPolicyManager& get(OperationContext* opCtx);

    /**
     * Sets the MetricsPolicyManager on a ServiceContext.
     */
    static void set(ServiceContext* svcCtx, std::unique_ptr<MetricsPolicyManager>&& manager);

    /**
     * Returns whether filtering of the given MetricsCategoryEnum is required for the given client.
     * For categories that support forced filtering, the value of forceFiltered is ignored if
     * metrics filtering is not enabled or supported. For categories that do not, forceFiltered is
     * expected to always be false.
     */
    virtual bool requiresFiltering(OperationContext* opCtx,
                                   MetricsCategoryEnum category,
                                   bool forcedFiltered) const = 0;

    /**
     * Returns a reference to the allowlist paths for the given MetricsCategoryEnum filtering.
     * Throws IllegalOperation if metrics should never be filtered.
     */
    virtual const std::vector<std::string>& getAllowlistPaths(
        MetricsCategoryEnum category) const = 0;

    /**
     * Returns a reference to the path matcher for the given MetricsCategoryEnum filtering. Throws
     * IllegalOperation if metrics should never be filtered.
     */
    virtual const PathMatcherNode& getAllowlistMatcher(MetricsCategoryEnum category) const = 0;

protected:
    /**
     * Throws IllegalOperation with the message that it is illegal to get an allowlist as
     * metrics should never be filtered.
     */
    [[noreturn]] static void _throwNoAllowlistError();
};

}  // namespace mongo
