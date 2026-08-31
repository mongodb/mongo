// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/replicated_fast_count/replicated_fast_count_uncommitted_changes.h"
#include "mongo/util/modules.h"

#include <functional>

[[MONGO_MOD_PUBLIC]];
namespace mongo {
/**
 * Function that gets registered as an onCommit callback for a collection the first time we write to
 * it and which ensures that size and count changes to that collection are reflected in fast count
 * metadata upon commit. This should be set when starting up the server.
 */
using FastCountCommitFn = std::function<void(OperationContext*, UncommittedFastCountChangeMap&)>;

void setFastCountCommitFn(FastCountCommitFn fn);
FastCountCommitFn& getFastCountCommitFn();

}  // namespace mongo
