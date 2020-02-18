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

#include "mongo/db/repl/ip_addr_lookup_service.h"

#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {
namespace {

class IPAddrLookupServiceTest : public unittest::Test {
public:
    IPAddrLookupServiceTest() {
        decltype(_dnsMock) dnsMock({{"localhost", "127.0.0.1"},
                                    {"mongodb.com", "52.21.89.200"},
                                    {"university.mongodb.com", "54.175.147.155"}});
        _dnsMock = std::move(dnsMock);
        _counter.store(0);
        setGlobalServiceContext(ServiceContext::make());
        _service.init([&](std::string hostName) -> std::string { return this->lookup(hostName); });
    }

    ~IPAddrLookupServiceTest() {
        _service.shutdown();
    }

    std::string lookup(std::string hostName) {
        stdx::lock_guard<Latch> lk(_mutex);
        auto it = _dnsMock.find(hostName);
        invariant(it != _dnsMock.end());
        _counter.fetchAndAdd(1);
        return it->second;
    }

    void reconfigureAndWaitForLookups(std::vector<std::string>& hosts) {
        const auto counterAfterLookups = _counter.load() + hosts.size();
        _service.reconfigure(hosts);
        while (_counter.load() < counterAfterLookups) {
            sleepFor(Milliseconds{10});
        }
    }

    IPAddrLookupService& getService() {
        return _service;
    }

    void updateMockDNS(std::string domain, std::string ip) {
        stdx::lock_guard<Latch> lk(_mutex);
        invariant(_dnsMock.find(domain) != _dnsMock.end());
        _dnsMock[domain] = ip;
    }

    void verifyCachedIP(std::string hostName, std::string expectedAddr) {
        auto ip = _service.lookup(hostName);
        ASSERT(ip);
        ASSERT_EQ(*ip, expectedAddr);
    }

private:
    IPAddrLookupService _service;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("IPAddrLookupServiceTest");

    stdx::unordered_map<std::string, std::string> _dnsMock;

    AtomicWord<size_t> _counter{0};
};

TEST_F(IPAddrLookupServiceTest, LookupRandomDomains) {
    ASSERT(!getService().lookup("xyz.com"));
    ASSERT(!getService().lookup("abc.com"));
}

TEST_F(IPAddrLookupServiceTest, ReconfigureWithEmptySet) {
    getService().reconfigure({});
}

TEST_F(IPAddrLookupServiceTest, ReconfigureAndLookup) {
    std::vector<std::string> domains({"mongodb.com", "localhost"});
    reconfigureAndWaitForLookups(domains);

    for (auto hostName : domains) {
        verifyCachedIP(hostName, lookup(hostName));
    }

    // Add a new host
    domains.push_back("university.mongodb.com");
    reconfigureAndWaitForLookups(domains);

    for (auto hostName : domains) {
        verifyCachedIP(hostName, lookup(hostName));
    }

    // Remove all hosts
    getService().reconfigure({});

    for (auto hostName : domains) {
        ASSERT(!getService().lookup(hostName));
    }
}

TEST_F(IPAddrLookupServiceTest, CacheTimeout) {
    std::vector<std::string> domains({"mongodb.com"});
    reconfigureAndWaitForLookups(domains);

    verifyCachedIP(domains.front(), lookup(domains.front()));

    updateMockDNS(domains.back(), "127.0.0.1");
    ASSERT_EQ(lookup(domains.back()), "127.0.0.1");

    // Simulates a timeout by forcing the worker thread to wake-up
    domains.push_back("university.mongodb.com");
    reconfigureAndWaitForLookups(domains);

    verifyCachedIP(domains.front(), "127.0.0.1");
}

}  // namespace
}  // namespace repl
}  // namespace mongo
