// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/views/view.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString viewNss = NamespaceString::createNamespaceString_forTest("testdb.testview");
const NamespaceString backingNss =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");
const NamespaceString bucketsColl =
    NamespaceString::createNamespaceString_forTest("testdb.system.buckets.testcoll");
const NamespaceString timeseriesColl =
    NamespaceString::createNamespaceString_forTest("testdb.testcoll");
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

DEATH_TEST_REGEX(ViewDefinitionTestDeathTest,
                 SetViewOnFailsIfNewViewOnNotInSameDatabaseAsView,
                 R"#(Invariant failure.*_viewNss\.isEqualDb\(viewOnNss\))#") {
    ViewDefinition viewDef(
        viewNss.dbName(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);
    NamespaceString badViewOn =
        NamespaceString::createNamespaceString_forTest("someOtherDb.someOtherCollection");
    viewDef.setViewOn(badViewOn);
}

TEST(ViewDefinitionTest, SetViewOnSucceedsIfNewViewOnIsInSameDatabaseAsView) {
    ViewDefinition viewDef(
        viewNss.dbName(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);
    ASSERT_EQ(viewDef.viewOn(), backingNss);

    NamespaceString newViewOn =
        NamespaceString::createNamespaceString_forTest("testdb.othercollection");
    viewDef.setViewOn(newViewOn);
    ASSERT_EQ(newViewOn, viewDef.viewOn());
}

TEST(ViewDefinitionTest, SetPipelineSucceedsOnValidArrayBSONElement) {
    ViewDefinition viewDef(viewNss.dbName(), viewNss.coll(), backingNss.coll(), BSONObj(), nullptr);
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
    ASSERT(viewDef.timeseries());
}

TEST(ViewDefinitionTest, ViewDefinitionCreationCorrectlyBuildsNamespaceStringsWithTenantIds) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);

    TenantId tenantId(OID::gen());
    NamespaceString viewNss =
        NamespaceString::createNamespaceString_forTest(tenantId, "testdb.testview");
    NamespaceString backingNss =
        NamespaceString::createNamespaceString_forTest(tenantId, "testdb.testcoll");

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
