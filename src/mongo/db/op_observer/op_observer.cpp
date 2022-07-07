/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/op_observer/op_observer.h"

#include "mongo/db/operation_context.h"

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
    invariant(_times._recursionDepth == 1 || !opCtx->writesAreReplicated());
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
