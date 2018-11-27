
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/util/string_map.h"
#include "mongo/util/unordered_fast_key_table.h"

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <absl/strings/string_view.h>
#include <algorithm>
#include <benchmark/benchmark.h>
#include <memory>
#include <random>
#include <unordered_map>


namespace mongo {
namespace {

constexpr uint32_t kMaxContainerSize = 1000000;
// Two fixed seeds, what they are doesn't matter much, they should just generate distict ranges.
constexpr uint32_t kDefaultSeed = 34862;
constexpr uint32_t kOtherSeed = 76453;

template <typename K_L, typename K_S>
struct UnorderedFastKeyTableAbslTraits {
    static uint32_t hash(K_L key) {
        return static_cast<uint32_t>(absl::Hash<K_L>{}(key));
    }

    static bool equals(K_L a, K_L b) {
        return a == b;
    }

    static K_S toStorage(K_L lookup) {
        return static_cast<K_S>(lookup);
    }

    static K_L toLookup(K_S const& storage) {
        return static_cast<K_L>(storage);
    }

    class HashedKey {
    public:
        explicit HashedKey(K_L key)
            : _key(std::move(key)), _hash(UnorderedFastKeyTableAbslTraits<K_L, K_S>::hash(_key)) {}

        HashedKey(K_L key, uint32_t hash) : _key(std::move(key)), _hash(hash) {}

        K_L key() const {
            return _key;
        }

        uint32_t hash() const {
            return _hash;
        }

    private:
        K_L _key;
        uint32_t _hash;
    };
};

using StdUnorderedInt = std::unordered_map<uint32_t, bool>;        // NOLINT
using StdUnorderedString = std::unordered_map<std::string, bool>;  // NOLINT

using MongoUnorderedFastKeyTableInt =
    UnorderedFastKeyTable<uint32_t,
                          uint32_t,
                          bool,
                          UnorderedFastKeyTableAbslTraits<uint32_t, uint32_t>>;
using MongoUnorderedFastKeyTableString =
    UnorderedFastKeyTable<absl::string_view,
                          std::string,
                          bool,
                          UnorderedFastKeyTableAbslTraits<absl::string_view, std::string>>;

using AbslFlatHashMapInt = absl::flat_hash_map<uint32_t, bool>;
using AbslFlatHashMapString = absl::flat_hash_map<std::string, bool>;

using AbslNodeHashMapInt = absl::node_hash_map<uint32_t, bool>;
using AbslNodeHashMapString = absl::node_hash_map<std::string, bool>;

template <typename>
struct IsUnorderedFastKeyTable : std::false_type {};

template <typename K_L, typename K_S, typename V, typename Traits>
struct IsUnorderedFastKeyTable<UnorderedFastKeyTable<K_L, K_S, V, Traits>> : std::true_type {};

template <typename>
struct IsAbslHashMap : std::false_type {};

template <typename K, typename V, typename Hash, typename Eq, typename Allocator>
struct IsAbslHashMap<absl::flat_hash_map<K, V, Hash, Eq, Allocator>> : std::true_type {};

template <typename K, typename V, typename Hash, typename Eq, typename Allocator>
struct IsAbslHashMap<absl::node_hash_map<K, V, Hash, Eq, Allocator>> : std::true_type {};

// Template conditional magic, I want to switch out the key type if storage is std::string AND
// the container is absl.
template <typename Container>
struct LookupType {
    using type = std::conditional_t<
        std::is_same<typename Container::key_type, std::string>::value,
        std::conditional_t<IsAbslHashMap<Container>::value, absl::string_view, std::string>,
        typename Container::key_type>;
};

template <class T>
std::enable_if_t<!IsUnorderedFastKeyTable<T>::value, float> getLoadFactor(const T& container) {
    return container.load_factor();
}

template <class T>
std::enable_if_t<IsUnorderedFastKeyTable<T>::value, float> getLoadFactor(const T& container) {
    return container.empty() ? 0.0f : (float)container.size() / container.capacity();
}

class BaseGenerator {
public:
    template <typename K>
    K generate();

private:
    virtual uint32_t generateInteger() = 0;

    StringData generateStringData(uint32_t i) {
        if (!_mem.get()) {
            // Use a very large buffer to store string keys contiguously so fetching the key memory
            // doesn't interfere with the actual test.
            // We create strings from 32bit integers, so they will have a maximum length of 10
            _mem = std::make_unique<char[]>(kMaxContainerSize * 10);
            _current = _mem.get();
        }
        sprintf(_current, "%u", i);
        StringData sd(_current);
        _current += sd.size();
        return sd;
    }

    std::unique_ptr<char[]> _mem{nullptr};
    char* _current{nullptr};
};

template <>
uint32_t BaseGenerator::generate<uint32_t>() {
    return generateInteger();
}

template <>
StringData BaseGenerator::generate<StringData>() {
    return generateStringData(generate<uint32_t>());
}

template <>
absl::string_view BaseGenerator::generate<absl::string_view>() {
    StringData sd = generateStringData(generate<uint32_t>());
    return absl::string_view(sd.rawData(), sd.size());
}

template <>
std::string BaseGenerator::generate<std::string>() {
    return generate<StringData>().toString();
}

class Sequence : public BaseGenerator {
private:
    uint32_t generateInteger() override {
        return ++_state;
    }

    uint32_t _state{0};
};

template <uint32_t Seed>
class UniformDistribution : public BaseGenerator {
public:
    UniformDistribution() : _gen(Seed) {}

private:
    uint32_t generateInteger() override {
        return _dist(_gen);
    }

    std::uniform_int_distribution<uint32_t> _dist;
    std::mt19937 _gen;
};

template <class Container, class LookupKey, class StorageGenerator, class LookupGenerator>
void LookupTest(benchmark::State& state) {
    Container container;
    StorageGenerator storage_gen;

    const int num = state.range(0) + 1;
    for (int i = num - 1; i; --i) {
        container[storage_gen.template generate<LookupKey>()];
    }

    std::vector<LookupKey> lookup_keys;
    LookupGenerator lookup_gen;
    for (int i = num; i; --i) {
        lookup_keys.push_back(lookup_gen.template generate<LookupKey>());
    }
    // Make sure we don't do the lookup in the same order as insert.
    std::shuffle(lookup_keys.begin(),
                 lookup_keys.end(),
                 std::default_random_engine(kDefaultSeed + kOtherSeed));

    int i = 0;
    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(container.find(lookup_keys[i++]));
        if (i == num) {
            i = 0;
        }
    }

    state.counters["size"] = state.range(0);
    state.counters["load_factor"] = getLoadFactor(container);
}

template <class Container, class LookupKey, class StorageGenerator>
void InsertTest(benchmark::State& state) {
    std::vector<LookupKey> insert_keys;
    StorageGenerator storage_gen;

    const int num = state.range(0);
    for (int i = num; i; --i) {
        insert_keys.push_back(storage_gen.template generate<LookupKey>());
    }

    int i = 0;
    Container container;
    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(container[insert_keys[i++]]);
        if (i == num) {
            i = 0;

            Container swap_container;
            std::swap(container, swap_container);
        }
    }

    state.counters["size"] = state.range(0);
}

template <class Container>
void BM_SuccessfulLookup(benchmark::State& state) {
    LookupTest<Container,
               typename LookupType<Container>::type,
               UniformDistribution<kDefaultSeed>,
               UniformDistribution<kDefaultSeed>>(state);
}

template <class Container>
void BM_UnsuccessfulLookup(benchmark::State& state) {
    LookupTest<Container,
               typename LookupType<Container>::type,
               UniformDistribution<kDefaultSeed>,
               UniformDistribution<kOtherSeed>>(state);
}

template <class Container>
void BM_UnsuccessfulLookupSeq(benchmark::State& state) {
    LookupTest<Container,
               typename LookupType<Container>::type,
               Sequence,
               UniformDistribution<kDefaultSeed>>(state);
}

template <class Container>
void BM_Insert(benchmark::State& state) {
    InsertTest<Container, typename LookupType<Container>::type, UniformDistribution<kDefaultSeed>>(
        state);
}

template <uint32_t Start = 0>
static void Range(benchmark::internal::Benchmark* b) {
    uint32_t n0 = Start, n1 = kMaxContainerSize;
    double fdn = 0.01;
    for (uint32_t n = n0; n <= n1; n += std::max(1u, static_cast<uint32_t>(n * fdn))) {
        b->Arg(n);
    }
}


// Integer key tests
BENCHMARK_TEMPLATE(BM_SuccessfulLookup, StdUnorderedInt)->Apply(Range);
BENCHMARK_TEMPLATE(BM_SuccessfulLookup, MongoUnorderedFastKeyTableInt)->Apply(Range);
BENCHMARK_TEMPLATE(BM_SuccessfulLookup, AbslFlatHashMapInt)->Apply(Range);
BENCHMARK_TEMPLATE(BM_SuccessfulLookup, AbslNodeHashMapInt)->Apply(Range);

BENCHMARK_TEMPLATE(BM_UnsuccessfulLookup, StdUnorderedInt)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookup, MongoUnorderedFastKeyTableInt)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookup, AbslFlatHashMapInt)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookup, AbslNodeHashMapInt)->Apply(Range);

BENCHMARK_TEMPLATE(BM_UnsuccessfulLookupSeq, StdUnorderedInt)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookupSeq, MongoUnorderedFastKeyTableInt)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookupSeq, AbslFlatHashMapInt)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookupSeq, AbslNodeHashMapInt)->Apply(Range);

BENCHMARK_TEMPLATE(BM_Insert, StdUnorderedInt)->Apply(Range<1>);
BENCHMARK_TEMPLATE(BM_Insert, MongoUnorderedFastKeyTableInt)->Apply(Range<1>);
BENCHMARK_TEMPLATE(BM_Insert, AbslFlatHashMapInt)->Apply(Range<1>);
BENCHMARK_TEMPLATE(BM_Insert, AbslNodeHashMapInt)->Apply(Range<1>);

// String key tests
BENCHMARK_TEMPLATE(BM_SuccessfulLookup, StdUnorderedString)->Apply(Range);
BENCHMARK_TEMPLATE(BM_SuccessfulLookup, MongoUnorderedFastKeyTableString)->Apply(Range);
BENCHMARK_TEMPLATE(BM_SuccessfulLookup, AbslFlatHashMapString)->Apply(Range);
BENCHMARK_TEMPLATE(BM_SuccessfulLookup, AbslNodeHashMapString)->Apply(Range);

BENCHMARK_TEMPLATE(BM_UnsuccessfulLookup, StdUnorderedString)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookup, MongoUnorderedFastKeyTableString)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookup, AbslFlatHashMapString)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookup, AbslNodeHashMapString)->Apply(Range);

BENCHMARK_TEMPLATE(BM_UnsuccessfulLookupSeq, StdUnorderedString)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookupSeq, MongoUnorderedFastKeyTableString)->Apply(Range);
BENCHMARK_TEMPLATE(BM_UnsuccessfulLookupSeq, AbslNodeHashMapString)->Apply(Range);

BENCHMARK_TEMPLATE(BM_Insert, StdUnorderedString)->Apply(Range<1>);
BENCHMARK_TEMPLATE(BM_Insert, MongoUnorderedFastKeyTableString)->Apply(Range<1>);
BENCHMARK_TEMPLATE(BM_Insert, AbslFlatHashMapString)->Apply(Range<1>);
BENCHMARK_TEMPLATE(BM_Insert, AbslNodeHashMapString)->Apply(Range<1>);

}  // namespace
}  // namespace mongo
