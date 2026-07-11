// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_committer.h"

namespace mongo {

namespace {
// Function to be registered as a callback for committing fast count changes.
FastCountCommitFn gFastCountCommitFn;
}  // namespace

void setFastCountCommitFn(FastCountCommitFn fn) {
    gFastCountCommitFn = std::move(fn);
}

FastCountCommitFn& getFastCountCommitFn() {
    return gFastCountCommitFn;
}
}  // namespace mongo
