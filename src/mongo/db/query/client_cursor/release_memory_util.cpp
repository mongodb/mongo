// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/client_cursor/release_memory_util.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(failReleaseMemoryAfterCursorCheckout);

}  // namespace mongo
