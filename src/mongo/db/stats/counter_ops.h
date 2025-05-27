/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/platform/atomic_word.h"

#include <cstdint>

namespace mongo {

/**
 * Helper functions to operate on AtomicWord<long long> and int64_t interchangibly.
 */
namespace counter_ops {
static int64_t get(const int64_t& counter) {
    return counter;
}

static int64_t get(const AtomicWord<long long>& counter) {
    return counter.load();
}

static void set(int64_t& counter, int64_t value) {
    counter = value;
}

static void set(int64_t& counter, const AtomicWord<long long>& value) {
    counter = value.load();
}

static void set(AtomicWord<long long>& counter, int64_t value) {
    counter.store(value);
}

static void set(AtomicWord<long long>& counter, const AtomicWord<long long>& value) {
    counter.store(value.load());
}

static void add(int64_t& counter, int64_t value) {
    counter += value;
}

static void add(int64_t& counter, const AtomicWord<long long>& value) {
    counter += value.load();
}

static void add(AtomicWord<long long>& counter, int64_t value) {
    counter.addAndFetch(value);
}

};  // namespace counter_ops
}  // namespace mongo
