// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Constructs a human-readable BSON from the specified LockerInfo structure.
 * The lockerInfo must be sorted.
 */
[[MONGO_MOD_PUBLIC]] void fillLockerInfo(const Locker::LockerInfo& lockerInfo,
                                         BSONObjBuilder& infoBuilder);

}  // namespace mongo
