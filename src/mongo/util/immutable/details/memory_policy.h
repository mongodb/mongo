/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

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
