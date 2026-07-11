// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/read_preference.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/decorable.h"

namespace mongo {

const OperationContext::Decoration<ReadPreferenceSetting> ReadPreferenceSetting::get =
    OperationContext::declareDecoration<ReadPreferenceSetting>();

}  // namespace mongo
