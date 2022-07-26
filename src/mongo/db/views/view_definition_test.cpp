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

#include <memory>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/views/view.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString viewNss("testdb.testview");
const NamespaceString backingNss("testdb.testcoll");
const NamespaceString bucketsColl("testdb.system.buckets.testcoll");
const NamespaceString timeseriesColl("testdb.testcoll");
const BSONObj samplePipeline = BSON_ARRAY(BSON("limit" << 9));

TEST(ViewDefinitionTest, ViewDefinitionCreationCorrectlyBuildsNamespaceStrings) {
    ViewDefinition viewDef(
        viewNss.dbName(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);
    ASSERT_EQ(viewDef.name(), viewNss);
    ASSERT_EQ(viewDef.viewOn(), backingNss);
}

TEST(ViewDefinitionTest, CopyConstructorProperlyClonesAllFields) {
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    ViewDefinition originalView(
        viewNss.dbName(), viewNss.coll(), backingNss.coll(), samplePipeline, std::move(collator));
    ViewDefinition copiedView(originalView);

    ASSERT_EQ(originalView.name(), copiedView.name());
    ASSERT_EQ(originalView.viewOn(), copiedView.viewOn());
    ASSERT(std::equal(originalView.pipeline().begin(),
                      originalView.pipeline().end(),
                      copiedView.pipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
    ASSERT(CollatorInterface::collatorsMatch(originalView.defaultCollator(),
                                             copiedView.defaultCollator()));
    ASSERT_EQ(originalView.timeseries(), copiedView.timeseries());
}

TEST(ViewDefinitionTest, CopyAssignmentOperatorProperlyClonesAllFields) {
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    ViewDefinition originalView(
        viewNss.dbName(), viewNss.coll(), backingNss.coll(), samplePipeline, std::move(collator));
    ViewDefinition copiedView = originalView;

    ASSERT_EQ(originalView.name(), copiedView.name());
    ASSERT_EQ(originalView.viewOn(), copiedView.viewOn());
    ASSERT(std::equal(originalView.pipeline().begin(),
                      originalView.pipeline().end(),
                      copiedView.pipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
    ASSERT(CollatorInterface::collatorsMatch(originalView.defaultCollator(),
                                             copiedView.defaultCollator()));
}

DEATH_TEST_REGEX(ViewDefinitionTest,
                 SetViewOnFailsIfNewViewOnNotInSameDatabaseAsView,
                 R"#(Invariant failure.*_viewNss.db\(\) == viewOnNss.db\(\))#") {
    ViewDefinition viewDef(
        viewNss.dbName(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);
    NamespaceString badViewOn("someOtherDb.someOtherCollection");
    viewDef.setViewOn(badViewOn);
}

TEST(ViewDefinitionTest, SetViewOnSucceedsIfNewViewOnIsInSameDatabaseAsView) {
    ViewDefinition viewDef(
        viewNss.dbName(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);
    ASSERT_EQ(viewDef.viewOn(), backingNss);

    NamespaceString newViewOn("testdb.othercollection");
    viewDef.setViewOn(newViewOn);
    ASSERT_EQ(newViewOn, viewDef.viewOn());
}

TEST(ViewDefinitionTest, SetPipelineSucceedsOnValidArrayBSONElement) {
    ViewDefinition viewDef(viewNss.db(), viewNss.coll(), backingNss.coll(), BSONObj(), nullptr);
    ASSERT(viewDef.pipeline().empty());

    BSONObj matchStage = BSON("match" << BSON("x" << 9));
    BSONObj sortStage = BSON("sort" << BSON("name" << -1));
    std::vector<BSONObj> newPipeline{matchStage, sortStage};

    viewDef.setPipeline(newPipeline);

    const auto& expectedPipeline = newPipeline;
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      viewDef.pipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST(ViewDefinitionTest, ViewDefinitionCreationCorrectlySetsTimeseries) {
    ViewDefinition viewDef(
        viewNss.dbName(), viewNss.coll(), bucketsColl.coll(), samplePipeline, nullptr);
    ASSERT_FALSE(viewDef.timeseries());

    ViewDefinition timeseriesDef(timeseriesColl.dbName(),
                                 timeseriesColl.coll(),
                                 bucketsColl.coll(),
                                 samplePipeline,
                                 nullptr);
    ASSERT_TRUE(timeseriesDef.timeseries());
}

TEST(ViewDefinitionTest, ViewDefinitionCreationCorrectlyBuildsNamespaceStringsWithTenantIds) {
    TenantId tenantId(OID::gen());
    NamespaceString viewNss(tenantId, "testdb.testview");
    NamespaceString backingNss(tenantId, "testdb.testcoll");

    ViewDefinition viewDef(
        viewNss.dbName(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);
    ASSERT(viewDef.name().tenantId());
    ASSERT_EQ(*viewDef.name().tenantId(), tenantId);
    ASSERT_EQ(viewDef.name(), viewNss);

    ASSERT(viewDef.viewOn().tenantId());
    ASSERT_EQ(*viewDef.viewOn().tenantId(), tenantId);
    ASSERT_EQ(viewDef.viewOn(), backingNss);
}
}  // namespace
}  // namespace mongo
