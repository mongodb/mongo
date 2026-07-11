// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"
#include "mongo/db/operation_context.h"

#include <memory>

namespace mongo::exec::agg {

/**
 * Produces a LocalLookupEligibility, consulted once when building the SingleDocumentLookupExecutor
 * chain.
 */
class LocalLookupEligibilityFactoryInterface {
public:
    virtual ~LocalLookupEligibilityFactoryInterface() = default;

    virtual std::unique_ptr<LocalLookupEligibility> makeLocalLookupEligibility(
        OperationContext* opCtx) const = 0;
};

}  // namespace mongo::exec::agg
