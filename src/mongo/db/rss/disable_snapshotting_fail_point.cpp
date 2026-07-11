// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/rss/disable_snapshotting_fail_point.h"

#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(disableSnapshotting);

}  // namespace mongo
