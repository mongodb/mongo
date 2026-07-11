// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/util/modules.h"

namespace mongo {

class OperationContext;

/**
 * Stores state and statistics related to ingress admission for a given transactional context.
 */
class [[MONGO_MOD_PUBLIC]] IngressAdmissionContext : public AdmissionContext {
public:
    IngressAdmissionContext() = default;

    /**
     * Retrieve the IngressAdmissionContext decoration the provided OperationContext
     */
    static IngressAdmissionContext& get(OperationContext* opCtx);
};

}  // namespace mongo
