// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/end_of_transaction_parameter.h"

#include "mongo/db/end_of_transaction_parameter_gen.h"
#include "mongo/db/server_feature_flags_gen.h"

namespace mongo {

bool isAllowEndOfTransactionEnabled() {
    return gAllowEndOfTransaction.loadRelaxed();
}

bool isEndOfTransactionWritePathEnabled() {
    return feature_flags::gFeatureFlagEndOfTransactionChangeEventSynthesis.isEnabled() &&
        isAllowEndOfTransactionEnabled();
}

}  // namespace mongo
