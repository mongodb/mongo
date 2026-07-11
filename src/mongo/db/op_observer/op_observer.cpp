// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_observer/op_observer.h"

#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo {
namespace {
const auto getOpObserverTimes = OperationContext::declareDecoration<OpObserver::Times>();
}  // namespace

auto OpObserver::Times::get(OperationContext* const opCtx) -> Times& {
    return getOpObserverTimes(opCtx);
}

OpObserver::ReservedTimes::ReservedTimes(OperationContext* const opCtx)
    : _times(Times::get(opCtx)) {
    // Every time that a `ReservedTimes` scope object is instantiated, we have to track if there was
    // a potentially recursive call. When there was no `OpObserver` chain being executed before this
    // instantiation, we should have an empty `reservedOpTimes` vector.
    if (!_times._recursionDepth++) {
        invariant(_times.reservedOpTimes.empty());
    }

    invariant(_times._recursionDepth > 0);
    invariant(_times._recursionDepth == 1 || !opCtx->writesAreReplicated(),
              str::stream() << "writes are replicated: " << opCtx->writesAreReplicated());
}

OpObserver::ReservedTimes::~ReservedTimes() {
    // Every time the `ReservedTimes` guard goes out of scope, this indicates one fewer level of
    // recursion in the `OpObserver` registered chain.
    if (!--_times._recursionDepth) {
        // When the depth hits 0, the `OpObserver` is considered to have finished, and therefore the
        // `reservedOpTimes` state needs to be reset.
        _times.reservedOpTimes.clear();
    }

    invariant(_times._recursionDepth >= 0);
}

}  // namespace mongo
