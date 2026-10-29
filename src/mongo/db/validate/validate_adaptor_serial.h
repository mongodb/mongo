// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/validate/validate_adaptor.h"


namespace mongo {
namespace collection_validation {

class ValidateAdaptorSerial final : public ValidateAdaptor {
public:
    ValidateAdaptorSerial(OperationContext* opCtx,
                          collection_validation::ValidateState& validateState);

    void traverseRecordStore(OperationContext* opCtx,
                             ValidateResults& validateResults,
                             ValidationVersion validationVersion) final;
};

}  // namespace collection_validation
}  // namespace mongo
