// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_adaptor_factory.h"

#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/validate_adaptor_parallel.h"
#include "mongo/db/validate/validate_adaptor_serial.h"
#include "mongo/db/validate/validate_state.h"

namespace mongo {
namespace collection_validation {

std::unique_ptr<ValidateAdaptor> ValidateAdaptorFactory::create(
    OperationContext* opCtx, collection_validation::ValidateState& validateState) {

    const auto& targetRecordsPerSlice = validateState.getTargetRecordsPerRecordStoreSlice();
    if (isRecordStoreSlicingEnabled(targetRecordsPerSlice)) {
        return std::make_unique<ValidateAdaptorParallel>(
            opCtx, validateState, targetRecordsPerSlice);
    }
    return std::make_unique<ValidateAdaptorSerial>(opCtx, validateState);
}

}  // namespace collection_validation
}  // namespace mongo
