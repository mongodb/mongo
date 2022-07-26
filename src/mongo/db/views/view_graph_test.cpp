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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
constexpr auto kEmptyPipelineSize = 0;
const auto kTestDb = DatabaseName(boost::none, "test");
constexpr auto kFooName = "foo"_sd;
constexpr auto kBarName = "bar"_sd;
constexpr auto kQuxName = "qux"_sd;
const auto kFooNamespace = NamespaceString(kTestDb, kFooName);
const auto kBarNamespace = NamespaceString(kTestDb, kBarName);
const auto kQuxNamespace = NamespaceString(kTestDb, kQuxName);
const auto kEmptyPipeline = BSONArray();
const auto kBinaryCollation = BSONObj();
const auto kFilipinoCollation = BSON("locale"
                                     << "fil");

class ViewGraphFixture : public unittest::Test {
public:
    ViewGraphFixture()
        : _queryServiceContext(), _opCtx(_queryServiceContext.makeOperationContext()) {}

    const OperationContext* opCtx() const {
        return _opCtx.get();
    }

    ViewGraph* viewGraph() {
        return &_viewGraph;
    }

    ViewDefinition makeViewDefinition(const DatabaseName& dbName,
                                      StringData view,
                                      StringData viewOn,
                                      BSONArray pipeline,
                                      BSONObj collatorSpec) const {
        auto collator = std::unique_ptr<CollatorInterface>(nullptr);
        if (!collatorSpec.isEmpty()) {
            auto factoryCollator = CollatorFactoryInterface::get(_opCtx->getServiceContext())
                                       ->makeFromBSON(collatorSpec);
            ASSERT_OK(factoryCollator.getStatus());
            collator = std::move(factoryCollator.getValue());
        }

        return {dbName, view, viewOn, pipeline, std::move(collator)};
    }

private:
    QueryTestServiceContext _queryServiceContext;
    ServiceContext::UniqueOperationContext _opCtx;
    ViewGraph _viewGraph;
};

TEST_F(ViewGraphFixture, CanInsertViewsWithMatchingBinaryCollations) {
    const auto fooView =
        makeViewDefinition(kTestDb, kFooName, kBarName, kEmptyPipeline, kBinaryCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(fooView, {kBarNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 2UL);

    const auto barView =
        makeViewDefinition(kTestDb, kBarName, kQuxName, kEmptyPipeline, kBinaryCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(barView, {kQuxNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 3UL);
}

TEST_F(ViewGraphFixture, CanInsertViewsWithMatchingNonTrivialCollations) {
    const auto fooView =
        makeViewDefinition(kTestDb, kFooName, kBarName, kEmptyPipeline, kFilipinoCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(fooView, {kBarNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 2UL);

    const auto barView =
        makeViewDefinition(kTestDb, kBarName, kQuxName, kEmptyPipeline, kFilipinoCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(barView, {kQuxNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 3UL);
}

TEST_F(ViewGraphFixture, CannotInsertViewsWithNonMatchingCollations) {
    const auto fooView =
        makeViewDefinition(kTestDb, kFooName, kBarName, kEmptyPipeline, kFilipinoCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(fooView, {kBarNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 2UL);

    const auto barView =
        makeViewDefinition(kTestDb, kBarName, kQuxName, kEmptyPipeline, kBinaryCollation);
    ASSERT_EQ(viewGraph()->insertAndValidate(barView, {kQuxNamespace}, kEmptyPipelineSize),
              ErrorCodes::OptionNotSupportedOnView);
    ASSERT_EQ(viewGraph()->size(), 2UL);
}

TEST_F(ViewGraphFixture, CannotRecreateViewWithDifferentCollationIfDependedOnByOtherViews) {
    const auto fooView =
        makeViewDefinition(kTestDb, kFooName, kBarName, kEmptyPipeline, kFilipinoCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(fooView, {kBarNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 2UL);

    const auto barView =
        makeViewDefinition(kTestDb, kBarName, kQuxName, kEmptyPipeline, kFilipinoCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(barView, {kQuxNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 3UL);

    viewGraph()->remove(kBarNamespace);
    ASSERT_EQ(viewGraph()->size(), 2UL);

    const auto barViewBinaryCollation =
        makeViewDefinition(kTestDb, kBarName, kQuxName, kEmptyPipeline, kBinaryCollation);
    ASSERT_EQ(
        viewGraph()->insertAndValidate(barViewBinaryCollation, {kQuxNamespace}, kEmptyPipelineSize),
        ErrorCodes::OptionNotSupportedOnView);
    ASSERT_EQ(viewGraph()->size(), 2UL);
}

// Tests that an insertion that would create a view cycle is rejected, but will later be accepted if
// the cycle is broken by removing another existing view.
TEST_F(ViewGraphFixture, CanCreateViewThatReferencesDroppedView) {
    const auto fooView =
        makeViewDefinition(kTestDb, kFooName, kBarName, kEmptyPipeline, kBinaryCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(fooView, {kBarNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 2UL);

    const auto barView =
        makeViewDefinition(kTestDb, kBarName, kQuxName, kEmptyPipeline, kBinaryCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(barView, {kQuxNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 3UL);

    const auto quxView =
        makeViewDefinition(kTestDb, kQuxName, kBarName, kEmptyPipeline, kBinaryCollation);

    // Inserting qux should fail, as it cycles with bar.
    ASSERT_EQ(viewGraph()->insertAndValidate(quxView, {kBarNamespace}, kEmptyPipelineSize),
              ErrorCodes::GraphContainsCycle);
    ASSERT_EQ(viewGraph()->size(), 3UL);

    viewGraph()->remove(kBarNamespace);
    ASSERT_EQ(viewGraph()->size(), 2UL);

    // With bar removed, we expect qux to be inserted successfully.
    ASSERT_OK(viewGraph()->insertAndValidate(quxView, {kBarNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 3UL);
}

// Tests that an insertion that would create mismatching collators is rejected, but will later be
// accepted if the existing view with the conflicting collator is removed.
TEST_F(ViewGraphFixture, CanCreateViewWithDifferentCollationThanDroppedView) {
    const auto fooView =
        makeViewDefinition(kTestDb, kFooName, kBarName, kEmptyPipeline, kFilipinoCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(fooView, {kBarNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 2UL);

    const auto barView =
        makeViewDefinition(kTestDb, kBarName, kQuxName, kEmptyPipeline, kBinaryCollation);

    // Inserting bar should fail, as foo depends on bar and has a different collation.
    ASSERT_EQ(viewGraph()->insertAndValidate(barView, {kQuxNamespace}, kEmptyPipelineSize),
              ErrorCodes::OptionNotSupportedOnView);
    ASSERT_EQ(viewGraph()->size(), 2UL);

    viewGraph()->remove(kFooNamespace);
    ASSERT_EQ(viewGraph()->size(), 0UL);

    // Now bar should be inserted successfully, as there are no existing views in the graph that
    // depend on it.
    ASSERT_OK(viewGraph()->insertAndValidate(barView, {kFooNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 2UL);
}

// Tests that a node in the graph is properly converted from a "view" node to a "non-view" node when
// a view with that namespace is removed.
TEST_F(ViewGraphFixture, DroppingViewPreservesNodeInGraphIfDependedOnByOtherViews) {
    const auto fooView =
        makeViewDefinition(kTestDb, kFooName, kBarName, kEmptyPipeline, kBinaryCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(fooView, {kBarNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 2UL);

    const auto barView =
        makeViewDefinition(kTestDb, kBarName, kQuxName, kEmptyPipeline, kBinaryCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(barView, {kQuxNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 3UL);

    // Inserts baz into the graph so that qux has another namespace that depends on it. This way,
    // the node for qux won't be destroyed when baz is removed.
    const auto bazView =
        makeViewDefinition(kTestDb, "baz"_sd, kQuxName, kEmptyPipeline, kBinaryCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(bazView, {kQuxNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 4UL);

    // Inserting a view that depends on bar but has a different collation should fail.
    const auto viewWithDifferentCollation = makeViewDefinition(
        kTestDb, "badCollation"_sd, kBarName, kEmptyPipeline, kFilipinoCollation);
    ASSERT_EQ(viewGraph()->insertAndValidate(
                  viewWithDifferentCollation, {kBarNamespace}, kEmptyPipelineSize),
              ErrorCodes::OptionNotSupportedOnView);
    ASSERT_EQ(viewGraph()->size(), 4UL);

    // Removes bar from the graph. The graph's size should remain at 4, since bar is still depended
    // on by qux.
    viewGraph()->remove(kBarNamespace);
    ASSERT_EQ(viewGraph()->size(), 4UL);

    // Inserting the viewWithDifferentCollation from above should now succeed, since bar is no
    // longer a view.
    ASSERT_OK(viewGraph()->insertAndValidate(
        viewWithDifferentCollation, {kBarNamespace}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 5UL);
}

TEST_F(ViewGraphFixture, DifferentTenantsCanCreateViewWithConflictingNamespaces) {
    DatabaseName db1(TenantId(OID::gen()), "test");
    DatabaseName db2(TenantId(OID::gen()), "test");

    NamespaceString viewOn1(db1, kBarName);
    NamespaceString viewOn2(db2, kBarName);

    // Create a view "foo" on tenant1's collection "test.bar".
    const auto fooView1 =
        makeViewDefinition(db1, kFooName, kBarName, kEmptyPipeline, kBinaryCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(fooView1, {viewOn1}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 2UL);

    // Create a view "foo" on tenant2's collection "test.bar".
    const auto fooView2 =
        makeViewDefinition(db2, kFooName, kBarName, kEmptyPipeline, kBinaryCollation);
    ASSERT_OK(viewGraph()->insertAndValidate(fooView2, {viewOn2}, kEmptyPipelineSize));
    ASSERT_EQ(viewGraph()->size(), 4UL);

    // Remove tenant1's view "foo".
    NamespaceString viewToRemove(db1, kFooName);
    viewGraph()->remove(viewToRemove);
    ASSERT_EQ(viewGraph()->size(), 2UL);
}
}  // namespace
}  // namespace mongo
