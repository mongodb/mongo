// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/util/modules.h"

namespace mongo {

class OperationContext;

/**
 * Stores state and statistics related to MultiPlan admission control for a given transactional
 * context.
 */
class MultiPlanAdmissionContext : public AdmissionContext {
public:
    MultiPlanAdmissionContext() = default;

    static MultiPlanAdmissionContext& get(OperationContext* opCtx);
};
}  // namespace mongo
