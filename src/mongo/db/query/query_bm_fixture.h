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

#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/benchmark_util.h"

#include <string>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {

class QueryBenchmarkFixture : public unittest::BenchmarkWithProfiler {
public:
    static const NamespaceString kNss;

    void setUpSharedResources(benchmark::State& state) override;
    void tearDownSharedResources(benchmark::State& state) override;

    void runBenchmark(BSONObj filter, BSONObj projection, benchmark::State& state);

protected:
    const std::vector<BSONObj>& docs() const {
        return _docs;
    }

    char randomLowercaseAlpha() {
        return 'a' + _random.nextInt64(24);
    }

    std::string randomLowercaseAlphaString(size_t size) {
        std::string str;
        str.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            str.push_back(randomLowercaseAlpha());
        }
        return str;
    }

    int64_t randomInt64() {
        return _random.nextInt64();
    }

    virtual BSONObj generateDocument(size_t index, size_t approximateSize) = 0;
    virtual std::vector<BSONObj> getIndexSpecs() const = 0;

    static BSONObj buildIndexSpec(StringData fieldName, bool unique) {
        return BSONObjBuilder{}
            .append("v", IndexConfig::kLatestIndexVersion)
            .append("key", BSON(fieldName << 1))
            .append("name", fieldName + "_1")
            .append("unique", unique)
            .obj();
    }

private:
    void createIndexes(OperationContext* opCtx, std::vector<BSONObj> indexes);

    void populateCollection(size_t size, size_t approximateSize);

    static constexpr int32_t kSeed = 1;
    PseudoRandom _random{kSeed};

    boost::optional<CatalogScopedGlobalServiceContextForTest> _fixture;
    ReadWriteConcernDefaultsLookupMock _lookupMock;
    std::vector<BSONObj> _docs;
};
}  // namespace mongo
