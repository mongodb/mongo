/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression_bm_fixture.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

BSONArray rangeBSONArray(int count) {
    BSONArrayBuilder builder;
    for (int i = 0; i < count; i++) {
        builder.append(std::to_string(i));
    }
    return builder.arr();
}

template <typename Generator>
std::vector<Document> randomPairs(int count, Generator generator) {
    std::vector<Document> documents;
    documents.reserve(count);

    for (int i = 0; i < count; ++i) {
        documents.emplace_back(BSON("lhs" << generator() << "rhs" << generator()));
    }

    return documents;
}

auto getDecimalGenerator(PseudoRandom& random) {
    static constexpr int64_t kMax = 9'000'000'000'000'000;
    static constexpr Decimal128 kHighDigits(1'000'000'000'000'000'000);
    return [&]() {
        return Decimal128(random.nextInt64(kMax))
            .multiply(kHighDigits)
            .add(Decimal128(random.nextInt64(kMax)));
    };
}

}  // namespace

void ExpressionBenchmarkFixture::benchmarkExpression(BSONObj expressionSpec,
                                                     benchmark::State& state) {
    std::vector<Document> documents = {{}};
    benchmarkExpression(expressionSpec, state, documents);
}

void ExpressionBenchmarkFixture::noOpBenchmark(benchmark::State& state) {
    benchmarkExpression(BSON("$const" << 1), state);
}

void ExpressionBenchmarkFixture::benchmarkDateDiffEvaluateMinute300Years(benchmark::State& state) {
    testDateDiffExpression(-1640989478000LL /* 1918-01-01*/,
                           7826117722000LL /* 2218-01-01*/,
                           "minute",
                           boost::none /*timezone*/,
                           boost::none /*startOfWeek*/,
                           state);
}

void ExpressionBenchmarkFixture::benchmarkDateDiffEvaluateMinute2Years(benchmark::State& state) {
    testDateDiffExpression(1542448721000LL /* 2018-11-17*/,
                           1605607121000LL /* 2020-11-17*/,
                           "minute",
                           boost::none /*timezone*/,
                           boost::none /*startOfWeek*/,
                           state);
}

void ExpressionBenchmarkFixture::benchmarkDateDiffEvaluateMinute2YearsWithTimezone(
    benchmark::State& state) {
    testDateDiffExpression(1542448721000LL /* 2018-11-17*/,
                           1605607121000LL /* 2020-11-17*/,
                           "minute",
                           std::string{"America/New_York"},
                           boost::none /*startOfWeek*/,
                           state);
}

void ExpressionBenchmarkFixture::benchmarkDateDiffEvaluateWeek(benchmark::State& state) {
    testDateDiffExpression(7826117722000LL /* 2218-01-01*/,
                           4761280721000LL /*2120-11-17*/,
                           "week",
                           boost::none /*timezone*/,
                           std::string("Sunday") /*startOfWeek*/,
                           state);
}


void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate10Days(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "day",
                          10LL,
                          boost::none, /* timezone */
                          state);
}

void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate600Minutes(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "minute",
                          600LL,
                          boost::none, /* timezone */
                          state);
}

void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate100KSeconds(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "second",
                          100000LL,
                          boost::none, /* timezone */
                          state);
}

void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate100Years(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "year",
                          100LL,
                          boost::none, /* timezone */
                          state);
}

void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate12HoursWithTimezone(
    benchmark::State& state) {
    testDateAddExpression(1604131115000LL, "hour", 12LL, std::string{"America/New_York"}, state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMinute15NewYork(
    benchmark::State& state) {
    testDateTruncExpression(1615460825000LL /* year 2021*/,
                            "minute",
                            15,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMinute15UTC(benchmark::State& state) {
    testDateTruncExpression(1615460825000LL /* year 2021*/,
                            "minute",
                            15,
                            boost::none,
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateHour1UTCMinus0700(
    benchmark::State& state) {
    testDateTruncExpression(1615460825000LL /* year 2021*/,
                            "hour",
                            1,
                            std::string{"-07:00"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateWeek2NewYorkValue2100(
    benchmark::State& state) {
    testDateTruncExpression(4108446425000LL /* year 2100*/,
                            "week",
                            2,
                            std::string{"America/New_York"},
                            std::string{"monday"} /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateWeek2UTCValue2100(
    benchmark::State& state) {
    testDateTruncExpression(4108446425000LL /* year 2100*/,
                            "week",
                            2,
                            std::string{"UTC"},
                            std::string{"monday"} /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMonth6NewYorkValue2100(
    benchmark::State& state) {
    testDateTruncExpression(4108446425000LL /* year 2100*/,
                            "month",
                            6,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMonth6NewYorkValue2030(
    benchmark::State& state) {
    testDateTruncExpression(1893466800000LL /* year 2030*/,
                            "month",
                            6,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMonth6UTCValue2030(
    benchmark::State& state) {
    testDateTruncExpression(1893466800000LL /* year 2030*/,
                            "month",
                            8,
                            boost::none,
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateYear1NewYorkValue2020(
    benchmark::State& state) {
    testDateTruncExpression(1583924825000LL /* year 2020*/,
                            "year",
                            1,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateYear1UTCValue2020(
    benchmark::State& state) {
    testDateTruncExpression(1583924825000LL /* year 2020*/,
                            "year",
                            1,
                            boost::none,
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateYear1NewYorkValue2100(
    benchmark::State& state) {
    testDateTruncExpression(4108446425000LL /* year 2100*/,
                            "year",
                            1,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

/**
 * Tests performance of $getField expression.
 */
void ExpressionBenchmarkFixture::benchmarkGetFieldEvaluateExpression(benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "field"
               << "x.y$z"
               << "input"
               << BSON("$const" << BSON("x.y$z"
                                        << "abc"));

    benchmarkExpression(BSON("$getField" << objBuilder.obj()), state);
}

void ExpressionBenchmarkFixture::benchmarkGetFieldEvaluateShortSyntaxExpression(
    benchmark::State& state) {
    benchmarkExpression(BSON("$getField" << BSON("$const"
                                                 << "$foo")),
                        state);
}

void ExpressionBenchmarkFixture::benchmarkGetFieldNestedExpression(benchmark::State& state) {
    BSONObjBuilder innerObjBuilder;
    innerObjBuilder << "field"
                    << "a.b"
                    << "input" << BSON("$const" << BSON("a.b" << BSON("c" << 123)));
    BSONObjBuilder outerObjBuilder;
    outerObjBuilder << "field"
                    << "c"
                    << "input" << BSON("$getField" << innerObjBuilder.obj());
    benchmarkExpression(BSON("$getField" << outerObjBuilder.obj()), state);
}

void ExpressionBenchmarkFixture::benchmarkSetFieldEvaluateExpression(benchmark::State& state) {
    testSetFieldExpression("a.b", "x", "y", state);
}

// The following two functions test different syntax for equivalent expressions:
// $unsetField is an alias for $setField with $$REMOVE.
void ExpressionBenchmarkFixture::benchmarkSetFieldWithRemoveExpression(benchmark::State& state) {
    testSetFieldExpression("a$b", "x", "$$REMOVE", state);
}

void ExpressionBenchmarkFixture::benchmarkUnsetFieldEvaluateExpression(benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "field"
               << "a$b.c"
               << "input"
               << BSON("$const" << BSON("a$b.c"
                                        << "x"
                                        << "f1" << 1 << "f2" << 2));

    benchmarkExpression(BSON("$unsetField" << objBuilder.obj()), state);
}

/**
 * Tests performance of $set* expressions.
 */
void ExpressionBenchmarkFixture::benchmarkSetIsSubset_allPresent(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = rangeBSONArray(kMax);

    benchmarkExpression(BSON("$setIsSubset" << BSON_ARRAY("$arr" << rhs)),
                        state,
                        std::vector<Document>(100, {{"arr"_sd, lhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetIsSubset_nonePresent(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax, kMax);
    BSONArray rhs = rangeBSONArray(kMax);

    benchmarkExpression(BSON("$setIsSubset" << BSON_ARRAY("$arr" << rhs)),
                        state,
                        std::vector<Document>(100, {{"arr"_sd, lhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetIntersection(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = randomBSONArray(100000, kMax);

    benchmarkExpression(BSON("$setIntersection" << BSON_ARRAY("$lhs"
                                                              << "$rhs")),
                        state,
                        std::vector<Document>(100, {{"lhs", lhs}, {"rhs", rhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetDifference(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = randomBSONArray(100000, kMax);

    benchmarkExpression(BSON("$setDifference" << BSON_ARRAY("$lhs"
                                                            << "$rhs")),
                        state,
                        std::vector<Document>(100, {{"lhs", lhs}, {"rhs", rhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetEquals(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = randomBSONArray(100000, kMax);

    benchmarkExpression(BSON("$setEquals" << BSON_ARRAY("$lhs"
                                                        << "$rhs")),
                        state,
                        std::vector<Document>(100, {{"lhs", lhs}, {"rhs", rhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetUnion(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = randomBSONArray(100000, kMax);

    benchmarkExpression(BSON("$setUnion" << BSON_ARRAY("$lhs"
                                                       << "$rhs")),
                        state,
                        std::vector<Document>(100, {{"lhs"_sd, lhs}, {"rhs"_sd, rhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSubtractIntegers(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() { return random.nextInt32(kMax); };
    testBinaryOpExpression("$subtract", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkSubtractDoubles(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() { return random.nextCanonicalDouble() * kMax; };
    testBinaryOpExpression("$subtract", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkSubtractDecimals(benchmark::State& state) {
    const int kCount = 1000;
    testBinaryOpExpression("$subtract", randomPairs(kCount, getDecimalGenerator(random)), state);
}

void ExpressionBenchmarkFixture::benchmarkSubtractDates(benchmark::State& state) {
    const int kCount = 1000;
    const int64_t kMin = -93724129050000;
    const int64_t kMax = 32503676400000;
    auto generator = [this]() {
        int64_t timestamp = std::uniform_int_distribution<int64_t>(kMin, kMax)(random.urbg());
        return Date_t::fromMillisSinceEpoch(timestamp);
    };
    testBinaryOpExpression("$subtract", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkSubtractNullAndMissing(benchmark::State& state) {
    const int kCount = 1000;
    std::vector<Document> documents;
    for (int i = 0; i < kCount / 4; ++i) {
        documents.emplace_back(BSON("empty" << true));
        documents.emplace_back(BSON("lhs" << BSONNULL));
        documents.emplace_back(BSON("rhs" << BSONNULL));
        documents.emplace_back(BSON("lhs" << BSONNULL << "rhs" << BSONNULL));
    }
    testBinaryOpExpression("$subtract", documents, state);
}

void ExpressionBenchmarkFixture::benchmarkAddIntegers(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() { return random.nextInt32(kMax); };
    testBinaryOpExpression("$add", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkAddDoubles(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() { return random.nextCanonicalDouble() * kMax; };
    testBinaryOpExpression("$add", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkAddDecimals(benchmark::State& state) {
    const int kCount = 1000;
    testBinaryOpExpression("$add", randomPairs(kCount, getDecimalGenerator(random)), state);
}

void ExpressionBenchmarkFixture::benchmarkAddDates(benchmark::State& state) {
    const int kCount = 1000;
    const int kMaxInt = 1000000;
    const int64_t kMinDate = -93724129050000;
    const int64_t kMaxDate = 32503676400000;

    std::vector<Document> documents;
    documents.reserve(kCount);

    auto nextDate = [this]() {
        int64_t timestamp =
            std::uniform_int_distribution<int64_t>(kMinDate, kMaxDate)(random.urbg());
        return Date_t::fromMillisSinceEpoch(timestamp);
    };

    for (int i = 0; i < kCount; ++i) {
        documents.emplace_back(BSON("lhs" << nextDate() << "rhs" << random.nextInt32(kMaxInt)));
    }

    testBinaryOpExpression("$add", documents, state);
}

void ExpressionBenchmarkFixture::benchmarkAddNullAndMissing(benchmark::State& state) {
    const int kCount = 1000;
    std::vector<Document> documents;
    for (int i = 0; i < kCount / 4; ++i) {
        documents.emplace_back(BSON("empty" << true));
        documents.emplace_back(BSON("lhs" << BSONNULL));
        documents.emplace_back(BSON("rhs" << BSONNULL));
        documents.emplace_back(BSON("lhs" << BSONNULL << "rhs" << BSONNULL));
    }
    testBinaryOpExpression("$add", documents, state);
}

void ExpressionBenchmarkFixture::benchmarkAddArray(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    BSONArrayBuilder elements;
    for (int i = 0; i < kCount; ++i) {
        elements.append(random.nextInt32(kMax));
    }
    Document document{BSON("operands" << elements.done())};
    BSONArrayBuilder operands;
    for (int i = 0; i < kCount; ++i) {
        operands.append("$operands." + std::to_string(i));
    }
    BSONObj expr = BSON("$add" << operands.arr());
    benchmarkExpression(std::move(expr), state, {document});
}

void ExpressionBenchmarkFixture::benchmarkMultiplyIntegers(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() { return random.nextInt32(kMax); };
    testBinaryOpExpression("$add", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkMultiplyDoubles(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() { return random.nextCanonicalDouble() * kMax; };
    testBinaryOpExpression("$multiply", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkMultiplyDecimals(benchmark::State& state) {
    const int kCount = 1000;
    testBinaryOpExpression("$multiply", randomPairs(kCount, getDecimalGenerator(random)), state);
}

void ExpressionBenchmarkFixture::benchmarkMultiplyNullAndMissing(benchmark::State& state) {
    const int kCount = 1000;
    std::vector<Document> documents;
    for (int i = 0; i < kCount / 4; ++i) {
        documents.emplace_back(BSON("empty" << true));
        documents.emplace_back(BSON("lhs" << BSONNULL));
        documents.emplace_back(BSON("rhs" << BSONNULL));
        documents.emplace_back(BSON("lhs" << BSONNULL << "rhs" << BSONNULL));
    }
    testBinaryOpExpression("$multiply", documents, state);
}

void ExpressionBenchmarkFixture::benchmarkMultiplyArray(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 2;
    BSONArrayBuilder elements;
    for (int i = 0; i < kCount; ++i) {
        elements.append(random.nextInt32(kMax) + 1);
    }
    Document document{BSON("operands" << elements.done())};
    BSONArrayBuilder operands;
    for (int i = 0; i < kCount; ++i) {
        operands.append("$operands." + std::to_string(i));
    }
    BSONObj expr = BSON("$multiply" << operands.arr());
    benchmarkExpression(std::move(expr), state, {document});
}

void ExpressionBenchmarkFixture::testDateDiffExpression(long long startDate,
                                                        long long endDate,
                                                        std::string unit,
                                                        boost::optional<std::string> timezone,
                                                        boost::optional<std::string> startOfWeek,
                                                        benchmark::State& state) {
    // Build a $dateDiff expression.
    BSONObjBuilder objBuilder;
    objBuilder << "startDate" << Date_t::fromMillisSinceEpoch(startDate) << "endDate"
               << "$endDate"
               << "unit" << unit;
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    if (startOfWeek) {
        objBuilder << "startOfWeek" << *startOfWeek;
    }
    benchmarkExpression(
        BSON("$dateDiff" << objBuilder.obj()),
        state,
        std::vector<Document>(1, {{"endDate"_sd, Date_t::fromMillisSinceEpoch(endDate)}}));
}

void ExpressionBenchmarkFixture::testDateTruncExpression(long long date,
                                                         std::string unit,
                                                         unsigned long binSize,
                                                         boost::optional<std::string> timezone,
                                                         boost::optional<std::string> startOfWeek,
                                                         benchmark::State& state) {
    // Build a $dateTrunc expression.
    BSONObjBuilder objBuilder;
    objBuilder << "date"
               << "$date"
               << "unit" << unit << "binSize" << static_cast<long long>(binSize);
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    if (startOfWeek) {
        objBuilder << "startOfWeek" << *startOfWeek;
    }
    benchmarkExpression(
        BSON("$dateTrunc" << objBuilder.obj()),
        state,
        std::vector<Document>(1, {{"date"_sd, Date_t::fromMillisSinceEpoch(date)}}));
}

void ExpressionBenchmarkFixture::testDateAddExpression(long long startDate,
                                                       std::string unit,
                                                       long long amount,
                                                       boost::optional<std::string> timezone,
                                                       benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "startDate"
               << "$startDate"
               << "unit" << unit << "amount" << amount;
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    benchmarkExpression(
        BSON("$dateAdd" << objBuilder.obj()),
        state,
        std::vector<Document>(1, {{"startDate"_sd, Date_t::fromMillisSinceEpoch(startDate)}}));
}

void ExpressionBenchmarkFixture::testSetFieldExpression(std::string fieldname,
                                                        std::string oldFieldValue,
                                                        std::string newFieldValue,
                                                        benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "field" << fieldname << "input"
               << BSON("$const" << BSON(fieldname << oldFieldValue << "f1" << 1 << "f2" << 2))
               << "value" << newFieldValue;

    benchmarkExpression(BSON("$setField" << objBuilder.obj()), state);
}

void ExpressionBenchmarkFixture::testBinaryOpExpression(const std::string& binaryOp,
                                                        const std::vector<Document>& documents,
                                                        benchmark::State& state) {
    BSONObj expr = BSON(binaryOp << BSON_ARRAY("$lhs"
                                               << "$rhs"));
    benchmarkExpression(std::move(expr), state, documents);
}

BSONArray ExpressionBenchmarkFixture::randomBSONArray(int count, int max, int offset) {
    BSONArrayBuilder builder;
    for (int i = 0; i < count; i++) {
        builder.append(std::to_string(offset + random.nextInt32(max)));
    }
    return builder.arr();
}

}  // namespace mongo
