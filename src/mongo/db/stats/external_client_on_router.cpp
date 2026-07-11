// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/external_client_on_router.h"

namespace mongo {
namespace {
const auto externalClientOnRouter = OperationContext::declareDecoration<bool>();
}

bool& isExternalClientOnRouter(OperationContext* opCtx) {
    return externalClientOnRouter(opCtx);
}

}  // namespace mongo
