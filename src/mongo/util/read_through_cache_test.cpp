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

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/read_through_cache.h"

namespace mongo {
namespace {

struct CachedValue {
    const int counter;
};

class Cache : public ReadThroughCache<std::string, CachedValue> {
public:
    Cache(size_t size, LookupFn lookupFn)
        : ReadThroughCache(size, _mutex), _lookupFn(std::move(lookupFn)) {}

private:
    boost::optional<CachedValue> lookup(OperationContext* opCtx, const std::string& key) override {
        return _lookupFn(opCtx, key);
    }

    Mutex _mutex = MONGO_MAKE_LATCH("ReadThroughCacheTest::Cache");

    LookupFn _lookupFn;
};

class ReadThroughCacheTest : public ServiceContextTest {
protected:
    const ServiceContext::UniqueOperationContext _opCtxHolder{makeOperationContext()};
    OperationContext* _opCtx{_opCtxHolder.get()};
};

TEST_F(ReadThroughCacheTest, FetchInvalidateAndRefetch) {
    int countLookups = 0;
    Cache cache(1, [&](OperationContext*, const std::string& key) {
        ASSERT_EQ("TestKey", key);
        countLookups++;

        return CachedValue{100 * countLookups};
    });

    for (int i = 1; i <= 3; i++) {
        auto value = cache.acquire(_opCtx, "TestKey");
        ASSERT(value);
        ASSERT_EQ(100 * i, value->counter);
        ASSERT_EQ(i, countLookups);

        ASSERT(cache.acquire(_opCtx, "TestKey"));
        ASSERT_EQ(i, countLookups);

        cache.invalidate("TestKey");
    }
}

TEST_F(ReadThroughCacheTest, CacheSizeZero) {
    int countLookups = 0;
    Cache cache(0, [&](OperationContext*, const std::string& key) {
        ASSERT_EQ("TestKey", key);
        countLookups++;

        return CachedValue{100 * countLookups};
    });

    for (int i = 1; i <= 3; i++) {
        auto value = cache.acquire(_opCtx, "TestKey");
        ASSERT(value);
        ASSERT_EQ(100 * i, value->counter);
        ASSERT_EQ(i, countLookups);
    }
}

}  // namespace
}  // namespace mongo
