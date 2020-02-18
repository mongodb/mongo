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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include <algorithm>

#include "mongo/db/repl/ip_addr_lookup_service.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace repl {

void IPAddrLookupService::init(DNSLookupFunc lookupFunc) noexcept {
    invariant(hasGlobalServiceContext());

    stdx::lock_guard<Latch> lk(_mutex);
    if (_thread) {
        return;
    }

    _safeToDelete.store(false);
    _lookup = std::move(lookupFunc);

    invariant(!_thread);
    _thread = stdx::thread([this]() { this->_workerThreadBody(); });
    invariant(_thread);
}

void IPAddrLookupService::shutdown() noexcept {
    stdx::unique_lock<Mutex> lk(_mutex);
    if (!_thread) {
        lk.unlock();
        while (!_safeToDelete.load()) {
            // Another `shutdown()` is in progress, wait for it to return.
        }
        return;
    }

    _shouldShutdown.store(true);
    _condVar.notify_one();

    auto thread = std::exchange(_thread, boost::none);

    {
        // Invalidate the cache as it won't get updated anymore.
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        _cache.clear();
    }

    lk.unlock();

    invariant(thread);
    thread->join();
    _safeToDelete.store(true);
}

void IPAddrLookupService::reconfigure(std::vector<std::string> newMembers) noexcept {
    std::sort(newMembers.begin(), newMembers.end());
    auto last = std::unique(newMembers.begin(), newMembers.end());
    newMembers.erase(last, newMembers.end());

    stdx::lock_guard<Latch> lk(_mutex);

    // Must add hosts that are present in `newMembers`, but missing in `_members`.
    std::vector<std::string> toAdd;
    std::set_difference(newMembers.begin(),
                        newMembers.end(),
                        _members.begin(),
                        _members.end(),
                        std::inserter(toAdd, toAdd.begin()));

    // Must remove hosts that are present in `_members`, but missing in `newMembers`.
    std::vector<std::string> toRemove;
    std::set_difference(_members.begin(),
                        _members.end(),
                        newMembers.begin(),
                        newMembers.end(),
                        std::inserter(toRemove, toRemove.begin()));

    _members = std::move(newMembers);

    // Update the cache based on the `toRemove` and `toAdd` lists
    stdx::lock_guard<Latch> cacheLock(_cacheMutex);
    for (auto host : toRemove) {
        _cache.erase(host);
    }

    if (toAdd.empty())
        return;
    for (auto host : toAdd) {
        _cache.emplace(host, CacheEntry());
    }
    _condVar.notify_one();
}

boost::optional<std::string> IPAddrLookupService::lookup(const std::string hostName) const
    noexcept {
    stdx::lock_guard<Latch> lk(_cacheMutex);
    auto it = _cache.find(hostName);
    if (it != _cache.end() && it->second.state == CacheEntry::State::Ready) {
        return it->second.ipAddr;
    }
    return boost::none;
}

boost::optional<std::vector<std::string>>
IPAddrLookupService::_getHostsAfterReconfigureOrTimeout() noexcept {
    Date_t timeout = Date_t::now() + kCacheRefreshTimeout;
    auto opCtx = Client::getCurrent()->makeOperationContext();

    try {
        stdx::unique_lock<Mutex> lk(_mutex);
        opCtx->waitForConditionOrInterruptUntil(_condVar, lk, timeout, [&] {
            if (_shouldShutdown.load())
                return true;

            // A reconfigure might have occurred before
            for (auto hostName : _members) {
                stdx::lock_guard<Latch> cacheLock(_cacheMutex);
                if (_cache[hostName].state == CacheEntry::State::Pending) {
                    return true;
                }
            }

            return false;
        });

        if (!_shouldShutdown.load()) {
            return std::vector(_members);
        }
    } catch (DBException& e) {
        LOGV2_WARNING(4530405,
                      "Interrupted while waiting for reconfigure or timeout: {exception}",
                      "exception"_attr = e.toString());
    } catch (...) {
        LOGV2_WARNING(4530406,
                      "Interrupted while waiting for reconfigure or timeout due to an error");
    }

    return boost::none;
}

void IPAddrLookupService::_workerThreadBody() noexcept {

    ON_BLOCK_EXIT([] {
        LOGV2_INFO(4530401,
                   "Stopped {addrLookupService}",
                   "addrLookupService"_attr = kIPAddrLookupService);
    });

    Client::initThread(kIPAddrLookupService);
    LOGV2_INFO(
        4530402, "Started {addrLookupService}", "addrLookupService"_attr = kIPAddrLookupService);

    auto updateCache = [&](std::vector<std::string>& hosts) {
        std::vector<boost::optional<std::string>> resolvedIPs(hosts.size());
        for (size_t i = 0; i < hosts.size(); i++) {
            try {
                auto ip = _lookup(hosts[i]);
                if (!ip.empty())
                    resolvedIPs[i] = ip;
            } catch (DBException& e) {
                LOGV2_WARNING(
                    4530403, "DNS Lookup failed: {exception}", "exception"_attr = e.toString());
            } catch (...) {
                LOGV2_WARNING(4530404, "DNS Lookup failed due to an error.");
            }
        }

        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        for (size_t i = 0; i < hosts.size(); i++) {
            auto it = _cache.find(hosts[i]);
            // `reconfigure()` could modify `_cache` in the meantime.
            if (it != _cache.end()) {
                it->second.state = CacheEntry::State::Ready;
                it->second.ipAddr = resolvedIPs[i];
            }
        }
    };

    while (!_shouldShutdown.load()) {
        if (auto hostsToLookup = _getHostsAfterReconfigureOrTimeout()) {
            updateCache(*hostsToLookup);
        }
    }
}

}  // namespace repl
}  // namespace mongo
