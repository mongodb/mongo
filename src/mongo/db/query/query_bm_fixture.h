// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/benchmark_util.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
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

    static BSONObj buildIndexSpec(std::string_view fieldName, bool unique) {
        return BSONObjBuilder{}
            .append("v", IndexConfig::kLatestIndexVersion)
            .append("key", BSON(fieldName << 1))
            .append("name", fmt::format("{}_1", fieldName))
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
