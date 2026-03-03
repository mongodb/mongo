/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/engine_selection_plan.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

// TODO(SERVER-117622): Share fieldsToKeyPattern and buildSimpleIndexEntry with cbr_test_utils.h.
BSONObj fieldsToKeyPattern(const std::vector<std::string>& indexFields) {
    BSONObjBuilder bob;
    for (auto& fieldName : indexFields) {
        bob.append(fieldName, 1);
    }
    return bob.obj();
}

IndexEntry buildSimpleIndexEntry(const std::vector<std::string>& indexFields) {
    BSONObj kp = fieldsToKeyPattern(indexFields);
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexConfig::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            {},
            nullptr};
}

TEST(GetExecutor, LookupUnwind) {
    auto nssLocal = NamespaceString::createNamespaceString_forTest("testdb.collLocal");
    auto nssForeign = NamespaceString::createNamespaceString_forTest("testdb.collForeign");

    std::vector<std::string> indexFields = {"a"};
    auto indexScan = std::make_unique<IndexScanNode>(nssLocal, buildSimpleIndexEntry(indexFields));
    auto lookupUnwind =
        std::make_unique<EqLookupUnwindNode>(std::move(indexScan),
                                             FieldPath("a"),
                                             nssForeign,
                                             FieldPath("b"),
                                             FieldPath("c"),
                                             EqLookupNode::LookupStrategy::kHashJoin,
                                             boost::none,
                                             false,
                                             false,
                                             boost::none);
    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(lookupUnwind));
    ASSERT_TRUE(engineSelectionForPlan(solution.get()) == EngineChoice::kSbe);
}

}  // namespace

}  // namespace mongo
