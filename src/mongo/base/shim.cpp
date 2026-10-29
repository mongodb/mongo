// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/shim.h"

namespace mongo {

WeakFunctionRegistry::BasicSlot::~BasicSlot() = default;

WeakFunctionRegistry& globalWeakFunctionRegistry() {
    static auto& p = *new WeakFunctionRegistry();
    return p;
}

}  // namespace mongo
