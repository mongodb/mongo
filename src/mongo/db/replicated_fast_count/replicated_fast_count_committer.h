// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <boost/container/flat_map.hpp>
#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo {
/**
 * Function that gets registered as an onCommit callback for a collection the first time we write to
 * it and which ensures that size and count changes to that collection are reflected in fast count
 * metadata upon commit. This should be set when starting up the server.
 */
using FastCountCommitFn = std::function<void(
    OperationContext*, const boost::container::flat_map<UUID, CollectionSizeCount>&)>;

void setFastCountCommitFn(FastCountCommitFn fn);
FastCountCommitFn& getFastCountCommitFn();

}  // namespace mongo
