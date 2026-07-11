/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/memory_tracking/memory_usage_limit.h"

#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"

namespace mongo {
namespace {

int64_t readGlobalKnobValue(const QueryKnob<long long>& knob) {
    return std::get<long long>(QueryKnobRegistry::instance().entry(knob.id).readGlobal());
}

}  // namespace

int64_t MemoryUsageLimit::_resolveAndLatch(OperationContext* opCtx) const {
    const auto& knob = std::get<QueryKnob<long long>>(_slot);
    if (MONGO_unlikely(opCtx == nullptr)) {
        // No operation to resolve against. This only happens outside client operations, on
        // long-lived detached ExpressionContexts (e.g. collection validators). Read the knob's
        // global value without latching: latching here would pin the value for the lifetime of
        // the owning object.
        return readGlobalKnobValue(knob);
    }
    // Return the resolved local rather than re-reading the variant, so this cold path performs no
    // further discriminant checks either.
    int64_t value = QueryKnobConfiguration::get(opCtx).get(knob);
    _slot = value;
    return value;
}

}  // namespace mongo
