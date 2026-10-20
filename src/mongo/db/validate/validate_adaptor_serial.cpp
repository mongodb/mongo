// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_adaptor_serial.h"

namespace mongo {
namespace collection_validation {

ValidateAdaptorSerial::ValidateAdaptorSerial(OperationContext* opCtx,
                                             collection_validation::ValidateState& validateState)
    : ValidateAdaptor(opCtx, validateState) {}

void ValidateAdaptorSerial::traverseRecordStore(OperationContext* opCtx,
                                                ValidateResults& validateResults,
                                                ValidationVersion validationVersion) {
    traverseRecordStoreSerial(opCtx, validateResults, validationVersion);
}

}  // namespace collection_validation
}  // namespace mongo
