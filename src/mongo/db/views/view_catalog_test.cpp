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

#include <functional>
#include <memory>
#include <set>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

constexpr auto kLargeString =
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000";
const auto kOneKiBMatchStage = BSON("$match" << BSON("data" << kLargeString));
const auto kTinyMatchStage = BSON("$match" << BSONObj());

class ViewCatalogFixture : public CatalogTestFixture {
public:
    void setUp() override {
        CatalogTestFixture::setUp();

        WriteUnitOfWork wuow(operationContext());
        AutoGetDb autoDb(operationContext(), DatabaseName(boost::none, "db"), MODE_X);
        _db = autoDb.ensureDbExists(operationContext());
        invariant(_db);

        // Create any additional databases used throughout the test.
        ASSERT(AutoGetDb(operationContext(), DatabaseName(boost::none, "db1"), MODE_X)
                   .ensureDbExists(operationContext()));
        ASSERT(AutoGetDb(operationContext(), DatabaseName(boost::none, "db2"), MODE_X)
                   .ensureDbExists(operationContext()));

        auto durableViewCatalogUnique = std::make_unique<DurableViewCatalogImpl>(_db);
        durableViewCatalog = durableViewCatalogUnique.get();

        // Create the system views collection for the database.
        ASSERT(_db->createCollection(
            operationContext(),
            NamespaceString("db", NamespaceString::kSystemDotViewsCollectionName)));

        wuow.commit();
    }

    void tearDown() override {
        CatalogTestFixture::tearDown();
    }

    auto getCatalog() {
        return CollectionCatalog::get(operationContext());
    }

    Status createView(OperationContext* opCtx,
                      const NamespaceString& viewName,
                      const NamespaceString& viewOn,
                      const BSONArray& pipeline,
                      const BSONObj& collation) {
        Lock::DBLock dbLock(operationContext(), viewName.dbName(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), viewName, MODE_IX);
        Lock::CollectionLock sysCollLock(
            operationContext(),
            NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);

        WriteUnitOfWork wuow(opCtx);
        Status s = getCatalog()->createView(
            opCtx, viewName, viewOn, pipeline, collation, view_catalog_helpers::validatePipeline);
        wuow.commit();

        return s;
    }

    Status modifyView(OperationContext* opCtx,
                      const NamespaceString& viewName,
                      const NamespaceString& viewOn,
                      const BSONArray& pipeline) {
        Lock::DBLock dbLock(operationContext(), viewName.dbName(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), viewName, MODE_X);
        Lock::CollectionLock sysCollLock(
            operationContext(),
            NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);

        WriteUnitOfWork wuow(opCtx);
        Status s = getCatalog()->modifyView(
            opCtx, viewName, viewOn, pipeline, view_catalog_helpers::validatePipeline);
        wuow.commit();

        return s;
    }

    Status dropView(OperationContext* opCtx, const NamespaceString& viewName) {
        Lock::DBLock dbLock(operationContext(), viewName.dbName(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), viewName, MODE_IX);
        Lock::CollectionLock sysCollLock(
            operationContext(),
            NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);

        WriteUnitOfWork wuow(opCtx);
        Status s = getCatalog()->dropView(opCtx, viewName);
        wuow.commit();

        return s;
    }

    Database* db() {
        return _db;
    }

    std::shared_ptr<const ViewDefinition> lookup(OperationContext* opCtx,
                                                 const NamespaceString& ns) {
        Lock::DBLock dbLock(operationContext(), ns.dbName(), MODE_IS);
        return getCatalog()->lookupView(operationContext(), ns);
    }

private:
    Database* _db;

protected:
    DurableViewCatalogImpl* durableViewCatalog;
    const BSONArray emptyPipeline;
    const BSONObj emptyCollation;
};

// For tests which need to run in a replica set context.
class ReplViewCatalogFixture : public ViewCatalogFixture {
public:
    void setUp() override {
        ViewCatalogFixture::setUp();
        auto service = getServiceContext();
        repl::ReplSettings settings;

        settings.setReplSetString("viewCatalogTestSet/node1:12345");

        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service, settings);

        // Ensure that we are primary.
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
    }
};

TEST_F(ViewCatalogFixture, CreateExistingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_NOT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, CreateViewOnDifferentDatabase) {
    const NamespaceString viewName("db1.view");
    const NamespaceString viewOn("db2.coll");

    ASSERT_NOT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, CanCreateViewWithExprPredicate) {
    const NamespaceString viewOn("db.coll");
    ASSERT_OK(createView(operationContext(),
                         NamespaceString("db.view1"),
                         viewOn,
                         BSON_ARRAY(BSON("$match" << BSON("$expr" << 1))),
                         emptyCollation));

    ASSERT_OK(createView(operationContext(),
                         NamespaceString("db.view2"),
                         viewOn,
                         BSON_ARRAY(BSON("$facet" << BSON("output" << BSON_ARRAY(BSON(
                                                              "$match" << BSON("$expr" << 1)))))),
                         emptyCollation));
}

TEST_F(ViewCatalogFixture, CanCreateViewWithJSONSchemaPredicate) {
    const NamespaceString viewOn("db.coll");
    ASSERT_OK(createView(
        operationContext(),
        NamespaceString("db.view1"),
        viewOn,
        BSON_ARRAY(BSON("$match" << BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("x"))))),
        emptyCollation));

    ASSERT_OK(createView(
        operationContext(),
        NamespaceString("db.view2"),
        viewOn,
        BSON_ARRAY(BSON(
            "$facet" << BSON(
                "output" << BSON_ARRAY(BSON(
                    "$match" << BSON("$jsonSchema" << BSON("required" << BSON_ARRAY("x")))))))),
        emptyCollation));
}

TEST_F(ViewCatalogFixture, CanCreateViewWithLookupUsingPipelineSyntax) {
    const NamespaceString viewOn("db.coll");
    ASSERT_OK(createView(operationContext(),
                         NamespaceString("db.view"),
                         viewOn,
                         BSON_ARRAY(BSON("$lookup" << BSON("from"
                                                           << "fcoll"
                                                           << "as"
                                                           << "as"
                                                           << "pipeline" << BSONArray()))),
                         emptyCollation));
}

TEST_F(ViewCatalogFixture, CreateViewWithPipelineFailsOnInvalidStageName) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto invalidPipeline = BSON_ARRAY(BSON("INVALID_STAGE_NAME" << 1));
    ASSERT_THROWS(createView(operationContext(), viewName, viewOn, invalidPipeline, emptyCollation),
                  AssertionException);
}

TEST_F(ReplViewCatalogFixture, CreateViewWithPipelineFailsOnChangeStreamsStage) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    // $changeStream cannot be used in a view definition pipeline.
    auto invalidPipeline = BSON_ARRAY(BSON("$changeStream" << BSONObj()));

    ASSERT_THROWS_CODE(
        createView(operationContext(), viewName, viewOn, invalidPipeline, emptyCollation),
        AssertionException,
        ErrorCodes::OptionNotSupportedOnView);
}

TEST_F(ReplViewCatalogFixture, CreateViewWithPipelineFailsOnCollectionlessStage) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto invalidPipeline = BSON_ARRAY(BSON("$currentOp" << BSONObj()));

    ASSERT_THROWS_CODE(
        createView(operationContext(), viewName, viewOn, invalidPipeline, emptyCollation),
        AssertionException,
        ErrorCodes::InvalidNamespace);
}

TEST_F(ReplViewCatalogFixture, CreateViewWithPipelineFailsOnIneligibleStagePersistentWrite) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    // $out cannot be used in a view definition pipeline.
    auto invalidPipeline = BSON_ARRAY(BSON("$out"
                                           << "someOtherCollection"));

    ASSERT_THROWS_CODE(
        createView(operationContext(), viewName, viewOn, invalidPipeline, emptyCollation),
        AssertionException,
        ErrorCodes::OptionNotSupportedOnView);

    invalidPipeline = BSON_ARRAY(BSON("$merge"
                                      << "someOtherCollection"));

    ASSERT_THROWS_CODE(
        createView(operationContext(), viewName, viewOn, invalidPipeline, emptyCollation),
        AssertionException,
        ErrorCodes::OptionNotSupportedOnView);
}

TEST_F(ViewCatalogFixture, CreateViewOnInvalidCollectionName) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.$coll");

    ASSERT_NOT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, ExceedMaxViewDepthInOrder) {
    const char* ns = "db.view";
    int i = 0;

    for (; i < ViewGraph::kMaxViewDepth; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    const NamespaceString viewName(str::stream() << ns << i);
    const NamespaceString viewOn(str::stream() << ns << (i + 1));

    ASSERT_NOT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, ExceedMaxViewDepthByJoining) {
    const char* ns = "db.view";
    int i = 0;
    int size = ViewGraph::kMaxViewDepth * 2 / 3;

    for (; i < size; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    for (i = 1; i < size + 1; i++) {
        const NamespaceString viewName(str::stream() << ns << (size + i));
        const NamespaceString viewOn(str::stream() << ns << (size + i + 1));

        ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    const NamespaceString viewName(str::stream() << ns << size);
    const NamespaceString viewOn(str::stream() << ns << (size + 1));

    ASSERT_NOT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, CreateViewCycles) {
    {
        const NamespaceString viewName("db.view1");
        const NamespaceString viewOn("db.view1");

        ASSERT_NOT_OK(
            createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    {
        const NamespaceString view1("db.view1");
        const NamespaceString view2("db.view2");
        const NamespaceString view3("db.view3");

        ASSERT_OK(createView(operationContext(), view1, view2, emptyPipeline, emptyCollation));
        ASSERT_OK(createView(operationContext(), view2, view3, emptyPipeline, emptyCollation));
        ASSERT_NOT_OK(createView(operationContext(), view3, view1, emptyPipeline, emptyCollation));
    }
}

TEST_F(ViewCatalogFixture, CanSuccessfullyCreateViewWhosePipelineIsExactlyAtMaxSizeInBytes) {
    internalPipelineLengthLimit = 100000;
    ON_BLOCK_EXIT([] { internalPipelineLengthLimit = 1000; });

    ASSERT_EQ(ViewGraph::kMaxViewPipelineSizeBytes % kOneKiBMatchStage.objsize(), 0);

    BSONArrayBuilder builder(ViewGraph::kMaxViewPipelineSizeBytes);
    int pipelineSize = 0;
    for (; pipelineSize < ViewGraph::kMaxViewPipelineSizeBytes;
         pipelineSize += kOneKiBMatchStage.objsize()) {
        builder << kOneKiBMatchStage;
    }

    ASSERT_EQ(pipelineSize, ViewGraph::kMaxViewPipelineSizeBytes);

    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");
    const BSONObj collation;

    ASSERT_OK(createView(operationContext(), viewName, viewOn, builder.arr(), collation));
}

TEST_F(ViewCatalogFixture, CannotCreateViewWhosePipelineExceedsMaxSizeInBytes) {
    internalPipelineLengthLimit = 100000;
    ON_BLOCK_EXIT([] { internalPipelineLengthLimit = 1000; });

    // Fill the builder to exactly the maximum size, then push it just over the limit by adding an
    // additional tiny match stage.
    BSONArrayBuilder builder(ViewGraph::kMaxViewPipelineSizeBytes);
    for (int pipelineSize = 0; pipelineSize < ViewGraph::kMaxViewPipelineSizeBytes;
         pipelineSize += kOneKiBMatchStage.objsize()) {
        builder << kOneKiBMatchStage;
    }
    builder << kTinyMatchStage;

    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");
    const BSONObj collation;

    ASSERT_NOT_OK(createView(operationContext(), viewName, viewOn, builder.arr(), collation));
}

TEST_F(ViewCatalogFixture, CannotCreateViewIfItsFullyResolvedPipelineWouldExceedMaxSizeInBytes) {
    internalPipelineLengthLimit = 100000;
    ON_BLOCK_EXIT([] { internalPipelineLengthLimit = 1000; });

    BSONArrayBuilder builder1;
    BSONArrayBuilder builder2;

    for (int pipelineSize = 0; pipelineSize < ViewGraph::kMaxViewPipelineSizeBytes;
         pipelineSize += (kOneKiBMatchStage.objsize() * 2)) {
        builder1 << kOneKiBMatchStage;
        builder2 << kOneKiBMatchStage;
    }
    builder2 << kTinyMatchStage;

    const NamespaceString view1("db.view1");
    const NamespaceString view2("db.view2");
    const NamespaceString viewOn("db.coll");
    const BSONObj collation1;
    const BSONObj collation2;

    ASSERT_OK(createView(operationContext(), view1, viewOn, builder1.arr(), collation1));
    ASSERT_NOT_OK(createView(operationContext(), view2, view1, builder2.arr(), collation2));
}

TEST_F(ViewCatalogFixture, DropMissingView) {
    NamespaceString viewName("db.view");
    ASSERT_NOT_OK(dropView(operationContext(), viewName));
}

TEST_F(ViewCatalogFixture, ModifyMissingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_NOT_OK(modifyView(operationContext(), viewName, viewOn, emptyPipeline));
}

TEST_F(ViewCatalogFixture, ModifyViewOnDifferentDatabase) {
    const NamespaceString viewName("db1.view");
    const NamespaceString viewOn("db2.coll");

    ASSERT_NOT_OK(modifyView(operationContext(), viewName, viewOn, emptyPipeline));
}

TEST_F(ViewCatalogFixture, ModifyViewOnInvalidCollectionName) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.$coll");

    ASSERT_NOT_OK(modifyView(operationContext(), viewName, viewOn, emptyPipeline));
}

TEST_F(ReplViewCatalogFixture, ModifyViewWithPipelineFailsOnIneligibleStage) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto validPipeline = BSON_ARRAY(BSON("$match" << BSON("_id" << 1)));
    auto invalidPipeline = BSON_ARRAY(BSON("$changeStream" << BSONObj()));

    // Create the initial, valid view.
    ASSERT_OK(createView(operationContext(), viewName, viewOn, validPipeline, emptyCollation));

    // Now attempt to replace it with a pipeline containing $changeStream.
    ASSERT_THROWS_CODE(modifyView(operationContext(), viewName, viewOn, invalidPipeline),
                       AssertionException,
                       ErrorCodes::OptionNotSupportedOnView);
}

TEST_F(ViewCatalogFixture, LookupMissingView) {
    ASSERT(!lookup(operationContext(), NamespaceString("db.view")));
}

TEST_F(ViewCatalogFixture, LookupExistingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));

    ASSERT(lookup(operationContext(), viewName));
}

TEST_F(ViewCatalogFixture, LookupRIDExistingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    ASSERT(getCatalog()->lookupResourceName(resourceID).get() == "db.view");
}

TEST_F(ViewCatalogFixture, LookupRIDExistingViewRollback) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");
    {
        Lock::DBLock dbLock(operationContext(), viewName.dbName(), MODE_X);
        Lock::CollectionLock collLock(operationContext(), viewName, MODE_IX);
        Lock::CollectionLock sysCollLock(
            operationContext(),
            NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);

        WriteUnitOfWork wunit(operationContext());
        ASSERT_OK(getCatalog()->createView(operationContext(),
                                           viewName,
                                           viewOn,
                                           emptyPipeline,
                                           emptyCollation,
                                           view_catalog_helpers::validatePipeline));
    }
    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    ASSERT(!getCatalog()->lookupResourceName(resourceID));
}

TEST_F(ViewCatalogFixture, LookupRIDAfterDrop) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(dropView(operationContext(), viewName));

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    ASSERT(!getCatalog()->lookupResourceName(resourceID));
}

TEST_F(ViewCatalogFixture, LookupRIDAfterDropRollback) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    {
        WriteUnitOfWork wunit(operationContext());
        ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
        wunit.commit();
        ASSERT(getCatalog()->lookupResourceName(resourceID).get() == viewName.ns());
    }

    {
        Lock::DBLock dbLock(operationContext(), viewName.dbName(), MODE_X);
        Lock::CollectionLock collLock(operationContext(), viewName, MODE_IX);
        Lock::CollectionLock sysCollLock(
            operationContext(),
            NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);

        WriteUnitOfWork wunit(operationContext());
        ASSERT_OK(getCatalog()->dropView(operationContext(), viewName));
        // Do not commit, rollback.
    }
    // Make sure drop was rolled back and view is still in catalog.
    ASSERT(getCatalog()->lookupResourceName(resourceID).get() == viewName.ns());
}

TEST_F(ViewCatalogFixture, LookupRIDAfterModify) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(modifyView(operationContext(), viewName, viewOn, emptyPipeline));
    ASSERT(getCatalog()->lookupResourceName(resourceID).get() == viewName.ns());
}

TEST_F(ViewCatalogFixture, LookupRIDAfterModifyRollback) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto resourceID = ResourceId(RESOURCE_COLLECTION, "db.view"_sd);
    {
        WriteUnitOfWork wunit(operationContext());
        ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
        wunit.commit();
        ASSERT(getCatalog()->lookupResourceName(resourceID).get() == viewName.ns());
    }

    {
        Lock::DBLock dbLock(operationContext(), viewName.dbName(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), viewName, MODE_X);
        Lock::CollectionLock sysCollLock(
            operationContext(),
            NamespaceString(viewName.db(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);

        WriteUnitOfWork wunit(operationContext());
        ASSERT_OK(getCatalog()->modifyView(operationContext(),
                                           viewName,
                                           viewOn,
                                           emptyPipeline,
                                           view_catalog_helpers::validatePipeline));
        ASSERT(getCatalog()->lookupResourceName(resourceID).get() == viewName.ns());
        // Do not commit, rollback.
    }
    // Make sure view resource is still available after rollback.
    ASSERT(getCatalog()->lookupResourceName(resourceID).get() == viewName.ns());
}

TEST_F(ViewCatalogFixture, CreateViewThenDropAndLookup) {
    NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(createView(operationContext(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(dropView(operationContext(), viewName));

    ASSERT(!lookup(operationContext(), viewName));
}

TEST_F(ViewCatalogFixture, Iterate) {
    const NamespaceString view1("db.view1");
    const NamespaceString view2("db.view2");
    const NamespaceString view3("db.view3");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(createView(operationContext(), view1, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(createView(operationContext(), view2, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(createView(operationContext(), view3, viewOn, emptyPipeline, emptyCollation));

    std::set<std::string> viewNames = {"db.view1", "db.view2", "db.view3"};

    Lock::DBLock dbLock(operationContext(), view1.dbName(), MODE_IX);
    getCatalog()->iterateViews(
        operationContext(), view1.dbName(), [&viewNames](const ViewDefinition& view) {
            std::string name = view.name().toString();
            ASSERT(viewNames.end() != viewNames.find(name));
            viewNames.erase(name);
            return true;
        });

    ASSERT(viewNames.empty());
}

TEST_F(ViewCatalogFixture, ResolveViewCorrectPipeline) {
    const NamespaceString view1("db.view1");
    const NamespaceString view2("db.view2");
    const NamespaceString view3("db.view3");
    const NamespaceString viewOn("db.coll");
    BSONArrayBuilder pipeline1;
    BSONArrayBuilder pipeline3;
    BSONArrayBuilder pipeline2;

    pipeline1 << BSON("$match" << BSON("foo" << 1));
    pipeline2 << BSON("$match" << BSON("foo" << 2));
    pipeline3 << BSON("$match" << BSON("foo" << 3));

    ASSERT_OK(createView(operationContext(), view1, viewOn, pipeline1.arr(), emptyCollation));
    ASSERT_OK(createView(operationContext(), view2, view1, pipeline2.arr(), emptyCollation));
    ASSERT_OK(createView(operationContext(), view3, view2, pipeline3.arr(), emptyCollation));

    Lock::DBLock dbLock(operationContext(), view1.dbName(), MODE_IX);
    auto resolvedView =
        view_catalog_helpers::resolveView(operationContext(), getCatalog(), view3, boost::none);
    ASSERT(resolvedView.isOK());

    std::vector<BSONObj> expected = {BSON("$match" << BSON("foo" << 1)),
                                     BSON("$match" << BSON("foo" << 2)),
                                     BSON("$match" << BSON("foo" << 3))};

    std::vector<BSONObj> result = resolvedView.getValue().getPipeline();

    ASSERT_EQ(expected.size(), result.size());

    for (uint32_t i = 0; i < expected.size(); i++) {
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(expected[i] == result[i]));
    }
}

TEST_F(ViewCatalogFixture, ResolveViewOnCollectionNamespace) {
    const NamespaceString collectionNamespace("db.coll");

    Lock::DBLock dbLock(operationContext(), collectionNamespace.dbName(), MODE_IS);
    auto resolvedView = uassertStatusOK(view_catalog_helpers::resolveView(
        operationContext(), getCatalog(), collectionNamespace, boost::none));

    ASSERT_EQ(resolvedView.getNamespace(), collectionNamespace);
    ASSERT_EQ(resolvedView.getPipeline().size(), 0U);
}

TEST_F(ViewCatalogFixture, ResolveViewCorrectlyExtractsDefaultCollation) {
    const NamespaceString view1("db.view1");
    const NamespaceString view2("db.view2");
    const NamespaceString viewOn("db.coll");
    BSONArrayBuilder pipeline1;
    BSONArrayBuilder pipeline2;

    pipeline1 << BSON("$match" << BSON("foo" << 1));
    pipeline2 << BSON("$match" << BSON("foo" << 2));

    BSONObj collation = BSON("locale"
                             << "en_US");

    ASSERT_OK(createView(operationContext(), view1, viewOn, pipeline1.arr(), collation));
    ASSERT_OK(createView(operationContext(), view2, view1, pipeline2.arr(), collation));

    Lock::DBLock dbLock(operationContext(), view1.dbName(), MODE_IS);
    auto resolvedView =
        view_catalog_helpers::resolveView(operationContext(), getCatalog(), view2, boost::none);
    ASSERT(resolvedView.isOK());

    ASSERT_EQ(resolvedView.getValue().getNamespace(), viewOn);

    std::vector<BSONObj> expected = {BSON("$match" << BSON("foo" << 1)),
                                     BSON("$match" << BSON("foo" << 2))};
    std::vector<BSONObj> result = resolvedView.getValue().getPipeline();
    ASSERT_EQ(expected.size(), result.size());
    for (uint32_t i = 0; i < expected.size(); i++) {
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(expected[i] == result[i]));
    }

    auto expectedCollation = CollatorFactoryInterface::get(operationContext()->getServiceContext())
                                 ->makeFromBSON(collation);
    ASSERT_OK(expectedCollation.getStatus());
    ASSERT_BSONOBJ_EQ(resolvedView.getValue().getDefaultCollation(),
                      expectedCollation.getValue()->getSpec().toBSON());
}

}  // namespace
}  // namespace mongo
