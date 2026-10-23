// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/end_of_transaction_parameter.h"

#include "mongo/db/end_of_transaction_parameter_gen.h"

namespace mongo {

bool isAllowEndOfTransactionEnabled() {
    return gAllowEndOfTransaction.loadRelaxed();
}

}  // namespace mongo
