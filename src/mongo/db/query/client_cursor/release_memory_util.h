// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

namespace mongo {

// Enabling this failpoint will cause a releaseMemory to fail with a specified exception after it
// has checked out a cursor.
extern FailPoint failReleaseMemoryAfterCursorCheckout;

}  // namespace mongo
