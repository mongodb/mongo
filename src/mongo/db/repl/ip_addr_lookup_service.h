/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <functional>
#include <string>
#include <vector>

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace repl {

/**
 * A background service to maintain a (periodically updated) cache for DNS lookups of
 * replica-set members. The main objective is to avoid expensive DNS lookups while
 * holding replica-set coordinator locks.
 * This class assumes each domain maps to exactly one IP address.
 * Also, the size of the cache does not exceed the number of members in the replica-set.
 * Please carefully consider the above assumptions if decided to reuse this service.
 */
class IPAddrLookupService final {
public:
    using DNSLookupFunc = std::function<std::string(const std::string)>;

    IPAddrLookupService() = default;

    IPAddrLookupService(const IPAddrLookupService&) = delete;

    IPAddrLookupService(IPAddrLookupService&&) noexcept = delete;

    IPAddrLookupService& operator=(const IPAddrLookupService&) = delete;

    IPAddrLookupService& operator=(IPAddrLookupService&&) = delete;

    ~IPAddrLookupService() {
        invariant(_safeToDelete.load());
    }

    // Starts the service and spawns a background thread that periodically updates the cache.
    void init(DNSLookupFunc lookupFunc = [](std::string hostName) -> std::string {
        return hostbyname(hostName.c_str());
    }) noexcept;

    // Stops the service and invalidates the cache.
    void shutdown() noexcept;

    /**
     * Reconfigures the cache by modifying `_cache` (adding and removing entries):
     *     1) Adds every (unique) element in the input vector that is missing in `_cache`.
     *     2) Removes elements from `_cache` that are not present in the input vector.
     *     3) Other elements in `_cache` remain unchanged.
     * This function does not validate the input and could accept IP addresses for host names.
     */
    void reconfigure(std::vector<std::string>) noexcept;

    /**
     * Queries `_cache` to return the IP address for the specified domain name.
     * If the entry is missing or not ready (i.e., pending), it returns boost::none.
     */
    boost::optional<std::string> lookup(const std::string) const noexcept;

    constexpr static auto kIPAddrLookupService = "IPAddrLookupService"_sd;

    /**
     * The refresh rate (5 minutes) is much lower than the default TTL for many DNS caches
     * (e.g., 1 day and 15 minutes for Windows), and high enough to avoid unnecessary network
     * traffic and DNS overhead.
     */
    constexpr static auto kCacheRefreshTimeout = Minutes(5);

private:
    boost::optional<std::vector<std::string>> _getHostsAfterReconfigureOrTimeout() noexcept;

    // Executes DNS lookups in the background and updates `_cache` if necessary.
    void _workerThreadBody() noexcept;

    // Serializes calls to `init()` and `shutdown()` as well as accesses to `_members`.
    // If acquired with other locks in this class, it must always be acquired first.
    // This mutex protects accesses to `condvar`.
    mutable Mutex _mutex = MONGO_MAKE_LATCH(kIPAddrLookupService);

    // `shutdown()` uses the following to signal the background thread to stop.
    AtomicWord<bool> _shouldShutdown{false};

    // Ensures the destructor is never called prior the completion of `shutdown()`.
    AtomicWord<bool> _safeToDelete{true};

    stdx::condition_variable _condVar;

    // Reference to the DNS lookup function.
    DNSLookupFunc _lookup;

    boost::optional<stdx::thread> _thread;

    /**
     * Maintains a sorted list of members in the cache to:
     *     1) Facilitate the reconfiguration process.
     *     2) Avoid blocking lookups while iterating over members.
     */
    std::vector<std::string> _members;

    struct CacheEntry {
        /**
         * All entries are initially in the pending state which indicates a DNS lookup is
         * scheduled but not yet completed. Once `ipAddr` is filled with the proper value,
         * `state` will be updated to ready. If DNS lookup fails, `ipAddr` is set to `boost::none`.
         */
        enum State { Pending, Ready };
        State state = State::Pending;
        boost::optional<std::string> ipAddr;
    };

    // Serializes accesses to `_cache` and its individual elements.
    mutable Mutex _cacheMutex = MONGO_MAKE_LATCH("IPAddrLookupServiceCache");

    // Maintains the mapping between each host name and its resolved IP address.
    stdx::unordered_map<std::string, CacheEntry> _cache;
};

}  // namespace repl
}  // namespace mongo
