// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <immer/memory_policy.hpp>

namespace mongo::immutable::detail {

// Memory allocations using regular new/delete operators
using HeapPolicy = immer::heap_policy<immer::cpp_heap>;

// Refcounting using atomics for thread safety
using RefcountPolicy = immer::refcount_policy;

// We are not using any features from immer that requires locking. Use 'void' as the lock type which
// would fail compilation if locking was needed anywhere.
using LockPolicy = void;

// No transience policy (this is just used for garbage collection)
using TransiencePolicy = immer::no_transience_policy;

using MemoryPolicy = immer::memory_policy<HeapPolicy,
                                          RefcountPolicy,
                                          LockPolicy,
                                          TransiencePolicy,
                                          /*PreferFewerBiggerObjects*/ true,
                                          /*UseTransientRValues*/ true>;

// Verify that the recommended settings for our memory policy is as expected. We need to investigate
// if any of these fire during a library upgrade.
static_assert(
    std::is_same<immer::get_transience_policy_t<RefcountPolicy>, TransiencePolicy>::value);
static_assert(immer::get_prefer_fewer_bigger_objects_v<HeapPolicy> == true);
static_assert(immer::get_use_transient_rvalues_v<RefcountPolicy> == true);

}  // namespace mongo::immutable::detail
