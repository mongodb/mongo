/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <memory>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/query/cqf_fast_paths.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/util/uuid.h"

namespace mongo::optimizer::fast_path {
namespace {
unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/exec/sbe"};

ExecTreeGeneratorParams makeParams(const BSONObj& filter) {
    const auto collUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    return {collUuid, nullptr, filter};
}

void verifySbePlan(unittest::GoldenTestContext& gctx,
                   const std::string& name,
                   const BSONObj& filter) {
    auto& stream = gctx.outStream();

    stream << std::endl;
    stream << "==== VARIATION: " << name << " ====" << std::endl;
    stream << "-- INPUT:" << std::endl;
    stream << "filter = " << filter.toString() << std::endl;
    stream << "-- OUTPUT:" << std::endl;

    auto params = makeParams(filter);
    auto [sbePlan, data] = getFastPathExecTreeForTest(std::move(params));

    sbe::DebugPrinter printer;
    stream << printer.print(*sbePlan) << std::endl;
}

TEST(FastPathPlanGeneration, EmptyQuery) {
    unittest::GoldenTestContext gctx{&goldenTestConfig};
    gctx.printTestHeader(unittest::GoldenTestContext::HeaderFormat::Text);

    auto filter = fromjson(R"({})");
    verifySbePlan(gctx, "Empty query", filter);
}

TEST(FastPathPlanGeneration, SinglePredicateOnTopLevelField) {
    unittest::GoldenTestContext gctx{&goldenTestConfig};
    gctx.printTestHeader(unittest::GoldenTestContext::HeaderFormat::Text);

    {
        auto filter = fromjson(R"({a: 1})");
        verifySbePlan(gctx, "equals int", filter);
    }

    {
        auto filter = fromjson(R"({a: {$eq: 1}})");
        verifySbePlan(gctx, "$eq int", filter);
    }

    {
        auto filter = fromjson(R"({a: {$eq: NaN}})");
        verifySbePlan(gctx, "$eq NaN", filter);
    }

    {
        auto filter = fromjson(R"({a: {$eq: null}})");
        verifySbePlan(gctx, "$eq null", filter);
    }

    {
        auto filter = fromjson(R"({a: {$eq: [1]}})");
        verifySbePlan(gctx, "$eq array", filter);
    }

    {
        auto filter = BSON("a" << BSON("$eq" << MINKEY));
        verifySbePlan(gctx, "$eq MinKey", filter);
    }

    {
        auto filter = BSON("a" << BSON("$eq" << MAXKEY));
        verifySbePlan(gctx, "$eq MaxKey", filter);
    }

    {
        auto filter = fromjson(R"({a: {$lt: 1}})");
        verifySbePlan(gctx, "$lt int", filter);
    }

    {
        auto filter = fromjson(R"({a: {$lt: NaN}})");
        verifySbePlan(gctx, "$lt NaN", filter);
    }

    {
        auto filter = fromjson(R"({a: {$lt: null}})");
        verifySbePlan(gctx, "$lt null", filter);
    }

    {
        auto filter = fromjson(R"({a: {$lt: [1]}})");
        verifySbePlan(gctx, "$lt array", filter);
    }

    {
        auto filter = BSON("a" << BSON("$lt" << MINKEY));
        verifySbePlan(gctx, "$lt MinKey", filter);
    }

    {
        auto filter = BSON("a" << BSON("$lt" << MAXKEY));
        verifySbePlan(gctx, "$lt MaxKey", filter);
    }

    {
        auto filter = fromjson(R"({a: {$lte: 1}})");
        verifySbePlan(gctx, "$lte int", filter);
    }

    {
        auto filter = fromjson(R"({a: {$lte: NaN}})");
        verifySbePlan(gctx, "$lte NaN", filter);
    }

    {
        auto filter = fromjson(R"({a: {$lte: null}})");
        verifySbePlan(gctx, "$lte null", filter);
    }

    {
        auto filter = fromjson(R"({a: {$lte: [1]}})");
        verifySbePlan(gctx, "$lte array", filter);
    }

    {
        auto filter = BSON("a" << BSON("$lte" << MINKEY));
        verifySbePlan(gctx, "$lte MinKey", filter);
    }

    {
        auto filter = BSON("a" << BSON("$lte" << MAXKEY));
        verifySbePlan(gctx, "$lte MaxKey", filter);
    }
}

}  // namespace
}  // namespace mongo::optimizer::fast_path
