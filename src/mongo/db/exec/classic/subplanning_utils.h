// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

namespace mongo {

class CanonicalQuery;

class SubPlanningUtils {
public:
    static bool canUseSubplanning(const CanonicalQuery& query);
};

}  // namespace mongo
