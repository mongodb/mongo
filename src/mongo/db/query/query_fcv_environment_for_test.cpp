// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_fcv_environment_for_test.h"

#include "mongo/db/server_options.h"
#include "mongo/util/version/releases.h"

namespace mongo {

void QueryFCVEnvironmentForTest::setUp() {
    // (Generic FCV reference): Must be initialized to allow FCV checks.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
}

}  // namespace mongo
