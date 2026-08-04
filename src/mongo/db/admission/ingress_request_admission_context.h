// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/util/modules.h"

namespace mongo {

class OperationContext;

/**
 * Stores state and statistics related to ingress request rate limiter (IRRL) admission for a
 * given transactional context.
 */
class [[MONGO_MOD_PUBLIC]] IngressRequestAdmissionContext : public AdmissionContext {
public:
    IngressRequestAdmissionContext() = default;

    /**
     * Retrieve the IngressRequestAdmissionContext decoration the provided OperationContext.
     */
    static IngressRequestAdmissionContext& get(OperationContext* opCtx);
};

}  // namespace mongo
