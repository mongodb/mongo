/**
*    Copyright (C) 2016 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include <memory>
#include <set>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/db/views/view_graph.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

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

class DurableViewCatalogDummy final : public DurableViewCatalog {
public:
    explicit DurableViewCatalogDummy() : _upsertCount(0), _iterateCount(0) {}
    static const std::string name;

    using Callback = stdx::function<Status(const BSONObj& view)>;
    virtual Status iterate(OperationContext* opCtx, Callback callback) {
        ++_iterateCount;
        return Status::OK();
    }
    virtual void upsert(OperationContext* opCtx, const NamespaceString& name, const BSONObj& view) {
        ++_upsertCount;
    }
    virtual void remove(OperationContext* opCtx, const NamespaceString& name) {}
    virtual const std::string& getName() const {
        return name;
    };

    int getUpsertCount() {
        return _upsertCount;
    }

    int getIterateCount() {
        return _iterateCount;
    }

private:
    int _upsertCount;
    int _iterateCount;
};

const std::string DurableViewCatalogDummy::name = "dummy";

class ViewCatalogFixture : public unittest::Test {
public:
    ViewCatalogFixture()
        : _queryServiceContext(stdx::make_unique<QueryTestServiceContext>()),
          opCtx(_queryServiceContext->makeOperationContext()),
          viewCatalog(&durableViewCatalog) {}

private:
    std::unique_ptr<QueryTestServiceContext> _queryServiceContext;

protected:
    DurableViewCatalogDummy durableViewCatalog;
    ServiceContext::UniqueOperationContext opCtx;
    ViewCatalog viewCatalog;
    const BSONArray emptyPipeline;
    const BSONObj emptyCollation;
};

TEST_F(ViewCatalogFixture, CreateExistingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, CreateViewOnDifferentDatabase) {
    const NamespaceString viewName("db1.view");
    const NamespaceString viewOn("db2.coll");

    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, CreateViewWithPipelineFailsOnInvalidStageName) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    auto invalidPipeline = BSON_ARRAY(BSON("INVALID_STAGE_NAME" << 1));
    ASSERT_THROWS(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, invalidPipeline, emptyCollation)
            .transitional_ignore(),
        UserException);
}

TEST_F(ViewCatalogFixture, CreateViewOnInvalidCollectionName) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.$coll");

    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, ExceedMaxViewDepthInOrder) {
    const char* ns = "db.view";
    int i = 0;

    for (; i < ViewGraph::kMaxViewDepth; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    const NamespaceString viewName(str::stream() << ns << i);
    const NamespaceString viewOn(str::stream() << ns << (i + 1));

    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, ExceedMaxViewDepthByJoining) {
    const char* ns = "db.view";
    int i = 0;
    int size = ViewGraph::kMaxViewDepth * 2 / 3;

    for (; i < size; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    for (i = 1; i < size + 1; i++) {
        const NamespaceString viewName(str::stream() << ns << (size + i));
        const NamespaceString viewOn(str::stream() << ns << (size + i + 1));

        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    const NamespaceString viewName(str::stream() << ns << size);
    const NamespaceString viewOn(str::stream() << ns << (size + 1));

    ASSERT_NOT_OK(
        viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
}

TEST_F(ViewCatalogFixture, CreateViewCycles) {
    {
        const NamespaceString viewName("db.view1");
        const NamespaceString viewOn("db.view1");

        ASSERT_NOT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    {
        const NamespaceString view1("db.view1");
        const NamespaceString view2("db.view2");
        const NamespaceString view3("db.view3");

        ASSERT_OK(viewCatalog.createView(opCtx.get(), view1, view2, emptyPipeline, emptyCollation));
        ASSERT_OK(viewCatalog.createView(opCtx.get(), view2, view3, emptyPipeline, emptyCollation));
        ASSERT_NOT_OK(
            viewCatalog.createView(opCtx.get(), view3, view1, emptyPipeline, emptyCollation));
    }
}

TEST_F(ViewCatalogFixture, CanSuccessfullyCreateViewWhosePipelineIsExactlyAtMaxSizeInBytes) {
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

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, builder.arr(), collation));
}

TEST_F(ViewCatalogFixture, CannotCreateViewWhosePipelineExceedsMaxSizeInBytes) {
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

    ASSERT_NOT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, builder.arr(), collation));
}

TEST_F(ViewCatalogFixture, CannotCreateViewIfItsFullyResolvedPipelineWouldExceedMaxSizeInBytes) {
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

    ASSERT_OK(viewCatalog.createView(opCtx.get(), view1, viewOn, builder1.arr(), collation1));
    ASSERT_NOT_OK(viewCatalog.createView(opCtx.get(), view2, view1, builder2.arr(), collation2));
}

TEST_F(ViewCatalogFixture, DropMissingView) {
    NamespaceString viewName("db.view");
    ASSERT_NOT_OK(viewCatalog.dropView(opCtx.get(), viewName));
}

TEST_F(ViewCatalogFixture, ModifyMissingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_NOT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
}

TEST_F(ViewCatalogFixture, ModifyViewOnDifferentDatabase) {
    const NamespaceString viewName("db1.view");
    const NamespaceString viewOn("db2.coll");

    ASSERT_NOT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
}

TEST_F(ViewCatalogFixture, ModifyViewOnInvalidCollectionName) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.$coll");

    ASSERT_NOT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
}

TEST_F(ViewCatalogFixture, LookupMissingView) {
    ASSERT(!viewCatalog.lookup(opCtx.get(), "db.view"_sd));
}

TEST_F(ViewCatalogFixture, LookupExistingView) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));

    ASSERT(viewCatalog.lookup(opCtx.get(), "db.view"_sd));
}

TEST_F(ViewCatalogFixture, CreateViewThenDropAndLookup) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(viewCatalog.dropView(opCtx.get(), viewName));

    ASSERT(!viewCatalog.lookup(opCtx.get(), "db.view"_sd));
}

TEST_F(ViewCatalogFixture, ModifyTenTimes) {
    const char* ns = "db.view";
    int i;

    for (i = 0; i < 5; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(
            viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    }

    for (i = 0; i < 5; i++) {
        const NamespaceString viewName(str::stream() << ns << i);
        const NamespaceString viewOn(str::stream() << ns << (i + 1));

        ASSERT_OK(viewCatalog.modifyView(opCtx.get(), viewName, viewOn, emptyPipeline));
    }

    ASSERT_EQ(10, durableViewCatalog.getUpsertCount());
}

TEST_F(ViewCatalogFixture, Iterate) {
    const NamespaceString view1("db.view1");
    const NamespaceString view2("db.view2");
    const NamespaceString view3("db.view3");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), view1, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(viewCatalog.createView(opCtx.get(), view2, viewOn, emptyPipeline, emptyCollation));
    ASSERT_OK(viewCatalog.createView(opCtx.get(), view3, viewOn, emptyPipeline, emptyCollation));

    std::set<std::string> viewNames = {"db.view1", "db.view2", "db.view3"};

    viewCatalog.iterate(opCtx.get(), [&viewNames](const ViewDefinition& view) {
        std::string name = view.name().toString();
        ASSERT(viewNames.end() != viewNames.find(name));
        viewNames.erase(name);
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

    ASSERT_OK(viewCatalog.createView(opCtx.get(), view1, viewOn, pipeline1.arr(), emptyCollation));
    ASSERT_OK(viewCatalog.createView(opCtx.get(), view2, view1, pipeline2.arr(), emptyCollation));
    ASSERT_OK(viewCatalog.createView(opCtx.get(), view3, view2, pipeline3.arr(), emptyCollation));

    auto resolvedView = viewCatalog.resolveView(opCtx.get(), view3);
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

TEST_F(ViewCatalogFixture, InvalidateThenReload) {
    const NamespaceString viewName("db.view");
    const NamespaceString viewOn("db.coll");

    ASSERT_OK(viewCatalog.createView(opCtx.get(), viewName, viewOn, emptyPipeline, emptyCollation));
    ASSERT_EQ(1, durableViewCatalog.getIterateCount());

    ASSERT(viewCatalog.lookup(opCtx.get(), "db.view"_sd));
    ASSERT_EQ(1, durableViewCatalog.getIterateCount());

    viewCatalog.invalidate();
    ASSERT_OK(viewCatalog.reloadIfNeeded(opCtx.get()));
    ASSERT_EQ(2, durableViewCatalog.getIterateCount());
}
}  // namespace
}  // namespace mongo
