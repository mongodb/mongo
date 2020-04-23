/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <absl/hash/hash.h>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/key_extractors.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <functional>

#include "mongo/logv2/log_severity.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/time_support.h"

namespace mongo::logv2 {

namespace bmi = boost::multi_index;

/**
 * Callable object yielding a variable LogSeverity depending on key and on timing.
 *
 * `bump(k)` returns `normal` severity on the
 * first call for a given `k`. This object will then internally mark `k` as being
 * suppressed for 1 sec, such that calls to `bump(k)` will return the less severe
 * `quiet` severity for key `k` until the suppression expires.
 */
template <typename K, typename H = absl::Hash<K>, typename Eq = std::equal_to<K>>
class KeyedSeveritySuppressor {
public:
    using key_type = K;
    using hasher = H;
    using key_eq = Eq;

    /**
     * @param period The duration of the the suppression.
     * @param normal The debug level to use most of the time
     * @param quiet  The debug level to use at most once per period
     */
    KeyedSeveritySuppressor(Milliseconds period, LogSeverity normal, LogSeverity quiet)
        : _period{period}, _normal{normal}, _quiet{quiet} {}

private:
    struct Suppression {
        key_type key;
        Date_t expire;
    };

    /**
     * Indexed two ways:
     *     [0]: like a queue of Suppression with arrivals at the back.
     *     [1]: like a hashset of Suppression, keyed by their `key` member.
     */
    using Suppressions = bmi::multi_index_container<
        Suppression,
        bmi::indexed_by<bmi::sequenced<>,
                        bmi::hashed_unique<bmi::member<Suppression, key_type, &Suppression::key>,
                                           hasher,
                                           key_eq>>>;

public:
    /**
     * If there is an unexpired suppression on `k`, returns the quiet severity.
     * Otherwise, creates a fresh suppression on `k` and returns the normal severity.
     * Reaps expired suppressions.
     */
    LogSeverity operator()(const key_type& k) {
        auto now = Date_t::now();
        auto lg = stdx::lock_guard(_mutex);
        auto& queue = _suppressions.template get<0>();    // view as a queue
        auto& hashset = _suppressions.template get<1>();  // view as a hashset
        for (; !queue.empty() && queue.front().expire <= now;)
            queue.pop_front();
        if (!hashset.insert({k, {now + _period}}).second)
            return _quiet;  // There was an active suppression on `k`
        return _normal;
    }

private:
    Milliseconds _period;
    LogSeverity _normal;
    LogSeverity _quiet;
    Mutex _mutex;
    Suppressions _suppressions;
};

class SeveritySuppressor {
public:
    SeveritySuppressor(Milliseconds period, LogSeverity normal, LogSeverity quiet)
        : _period{period}, _normal{normal}, _quiet{quiet} {}

    LogSeverity operator()() {
        auto now = Date_t::now();
        auto lg = stdx::lock_guard(_mutex);
        if (_expire <= now) {
            _expire = now + Seconds{1};
            return _quiet;
        }
        return _normal;
    }

private:
    Milliseconds _period;
    LogSeverity _normal;
    LogSeverity _quiet;
    Mutex _mutex;
    Date_t _expire;
};

}  // namespace mongo::logv2
