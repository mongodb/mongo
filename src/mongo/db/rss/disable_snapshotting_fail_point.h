// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

namespace mongo {

[[MONGO_MOD_PUBLIC]] extern FailPoint disableSnapshotting;

}  // namespace mongo
