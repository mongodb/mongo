// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/validate/validate_adaptor.h"

#include <memory>

namespace mongo {

namespace collection_validation {

class ValidateAdaptorFactory {
public:
    [[nodiscard]] static std::unique_ptr<ValidateAdaptor> create(
        OperationContext* opCtx, collection_validation::ValidateState& validateState);
};

}  // namespace collection_validation
}  // namespace mongo
