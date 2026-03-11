/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"

#include <benchmark/benchmark.h>

namespace mongo::sbe {

/**
 * Benchmark fixture with a generic, policy-based driver.
 *
 * Axes:
 *  - ShapePolicy: document shape / number of fields / target position
 *  - ValuePolicy: value type and size for the target field
 *  - FieldNamePolicy: field-name spelling / length (key size)
 *  - PresencePolicy: which field name is queried each iteration (hit/miss ratio)
 */
class GetFieldBenchmark : public benchmark::Fixture {
public:
    template <class ShapePolicy, class ValuePolicy, class FieldNamePolicy, class PresencePolicy>
    void runPolicyBm(benchmark::State& state) {
        BSONObj obj = ShapePolicy::template makeDoc<ValuePolicy, FieldNamePolicy>();

        auto [type, val] = stage_builder::makeValue(obj);
        vm::ByteCode byteCode;

        size_t i = 0;
        for (auto _ : state) {
            StringData fieldName = PresencePolicy::template selectField<FieldNamePolicy>(i++);
            benchmark::DoNotOptimize(byteCode.getField_test(type, val, fieldName));
        }
    }
};

/**
 * Helper: build a string of exactly 'len' characters from a base character and a numeric suffix.
 * E.g., makePaddedName('p', 8, 42) -> "p0000042" (8 chars total).
 */
inline std::string makePaddedName(char base, int len, int idx) {
    std::string suffix = std::to_string(idx);
    int prefixLen = len - static_cast<int>(suffix.size());
    if (prefixLen < 1) {
        prefixLen = 1;
    }
    return std::string(prefixLen, base) + suffix.substr(0, len - prefixLen);
}

/**
 * Field-name policy template.
 *
 * Each instantiation provides:
 *  - hit()              : the target field name (length N, starts with HitChar)
 *  - miss()             : a non-existent name (length N, different first character)
 *  - missCommonPrefix() : a non-existent name (length N, same first character as hit)
 *  - pad(int i)         : unique pad field names, each exactly length N
 *
 * Miss names differ from hit names to ensure they never accidentally match a pad field.
 */
template <int N, char HitChar, char MissChar, char MissCommonPrefixFill>
struct FieldNamePolicy {
    static_assert(HitChar != MissChar, "miss must differ in first character");

    static StringData hit() {
        static const std::string s(N, HitChar);
        return StringData(s);
    }

    static StringData miss() {
        static const std::string s(N, MissChar);
        return StringData(s);
    }

    static StringData missCommonPrefix() {
        // Same first character as hit(), rest differs.
        // For N == 1, a common-prefix miss is impossible (the single character must differ
        // to guarantee a miss), so we fall back to the regular miss.
        static const std::string s = [] {
            if constexpr (N == 1) {
                return std::string(1, MissChar);
            } else {
                std::string r(N, MissCommonPrefixFill);
                r[0] = HitChar;
                return r;
            }
        }();
        return StringData(s);
    }

    static std::string pad(int i) {
        return makePaddedName('p', N, i);
    }
};

// Concrete field-name policies: sizes 1, 4, 8, 16, 64.
//   hit:              N × HitChar
//   miss:             N × MissChar  (different first char → exercises first-char skip)
//   missCommonPrefix: HitChar + (N-1) × MissCommonPrefixFill (same first char → bypasses skip)
using FieldName1 = FieldNamePolicy<1, 'z', 'q', 'y'>;
using FieldName4 = FieldNamePolicy<4, 'a', 'x', 'b'>;
using FieldName8 = FieldNamePolicy<8, 'a', 'x', 'b'>;
using FieldName16 = FieldNamePolicy<16, 'a', 'x', 'b'>;
using FieldName64 = FieldNamePolicy<64, 'a', 'x', 'b'>;

/**
 * Shape policies (document layout / number of fields / target position).
 *
 * Template parameters:
 *  - NumPad: number of padding (non-target) fields
 *  - TargetFirst: if true, the target field is placed before the pad fields;
 *                 if false, the target field is placed after (worst-case scan)
 */
template <int NumPad, bool TargetFirst>
struct FieldsShape {
    template <class ValuePolicy, class FieldNamePolicy>
    static BSONObj makeDoc() {
        BSONObjBuilder bob;
        if constexpr (TargetFirst) {
            ValuePolicy::append(bob, FieldNamePolicy::hit());
        }
        for (int i = 0; i < NumPad; ++i) {
            const std::string name = FieldNamePolicy::pad(i);
            ValuePolicy::append(bob, name);
        }
        if constexpr (!TargetFirst) {
            ValuePolicy::append(bob, FieldNamePolicy::hit());
        }
        return bob.obj();
    }
};

// Concrete shape policies: vary field count, target position.
using Fields10TargetLast = FieldsShape<10, false>;
using Fields100TargetLast = FieldsShape<100, false>;
using Fields1000TargetLast = FieldsShape<1000, false>;
using Fields100TargetFirst = FieldsShape<100, true>;

/**
 * Value policies (value type / size for the target field).
 */

struct Int32Small {
    static void append(BSONObjBuilder& bob, StringData fieldName) {
        bob.append(fieldName, 42);
    }
};

struct String1KB {
    static void append(BSONObjBuilder& bob, StringData fieldName) {
        static const std::string kBuf(1024, 'x');  // 1 KB
        bob.append(fieldName, kBuf);
    }
};

/**
 * Presence policies (hit/miss / mix of field names).
 */

struct AlwaysHit {
    template <class FieldNamePolicy>
    static StringData selectField(size_t /*i*/) {
        return FieldNamePolicy::hit();
    }
};

struct AlwaysMiss {
    template <class FieldNamePolicy>
    static StringData selectField(size_t /*i*/) {
        return FieldNamePolicy::miss();
    }
};

struct Hit90Miss10 {
    template <class FieldNamePolicy>
    static StringData selectField(size_t i) {
        // Every 10th lookup is a miss.
        return (i % 10 == 0) ? FieldNamePolicy::miss() : FieldNamePolicy::hit();
    }
};

// Miss with a common first character — bypasses the first-char skip optimization.
struct AlwaysMissCommonPrefix {
    template <class FieldNamePolicy>
    static StringData selectField(size_t /*i*/) {
        return FieldNamePolicy::missCommonPrefix();
    }
};

struct Hit90MissCommonPrefix10 {
    template <class FieldNamePolicy>
    static StringData selectField(size_t i) {
        // Every 10th lookup is a common-prefix miss.
        return (i % 10 == 0) ? FieldNamePolicy::missCommonPrefix() : FieldNamePolicy::hit();
    }
};

/**
 * Helper macro to define a benchmark for a given combination of policies.
 */
#define DEFINE_GETFIELD_BM(shape, value, fname, presence)                  \
    BENCHMARK_F(GetFieldBenchmark, shape##_##value##_##fname##_##presence) \
    (benchmark::State & state) {                                           \
        runPolicyBm<shape, value, fname, presence>(state);                 \
    }

#define FOR_EACH_PRESENCE(M, shape, value, fname)  \
    M(shape, value, fname, AlwaysHit)              \
    M(shape, value, fname, AlwaysMiss)             \
    M(shape, value, fname, AlwaysMissCommonPrefix) \
    M(shape, value, fname, Hit90Miss10)            \
    M(shape, value, fname, Hit90MissCommonPrefix10)

#define FOR_EACH_FNAME(M, shape, value)             \
    FOR_EACH_PRESENCE(M, shape, value, FieldName1)  \
    FOR_EACH_PRESENCE(M, shape, value, FieldName4)  \
    FOR_EACH_PRESENCE(M, shape, value, FieldName8)  \
    FOR_EACH_PRESENCE(M, shape, value, FieldName16) \
    FOR_EACH_PRESENCE(M, shape, value, FieldName64)

#define FOR_EACH_VALUE(M, shape)         \
    FOR_EACH_FNAME(M, shape, Int32Small) \
    FOR_EACH_FNAME(M, shape, String1KB)

#define FOR_EACH_SHAPE(M)                   \
    FOR_EACH_VALUE(M, Fields10TargetLast)   \
    FOR_EACH_VALUE(M, Fields100TargetLast)  \
    FOR_EACH_VALUE(M, Fields1000TargetLast) \
    FOR_EACH_VALUE(M, Fields100TargetFirst)

#define GEN_GETFIELD_BM(shape, value, fname, presence) \
    DEFINE_GETFIELD_BM(shape, value, fname, presence)

// Generate the full matrix.
FOR_EACH_SHAPE(GEN_GETFIELD_BM);

#undef GEN_GETFIELD_BM
#undef FOR_EACH_SHAPE
#undef FOR_EACH_VALUE
#undef FOR_EACH_FNAME
#undef FOR_EACH_PRESENCE
}  // namespace mongo::sbe
