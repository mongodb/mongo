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
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString viewNss("testdb.testview");
const NamespaceString backingNss("testdb.testcoll");
const BSONObj samplePipeline = BSON_ARRAY(BSON("limit" << 9));

TEST(ViewDefinitionTest, ViewDefinitionCreationCorrectlyBuildsNamespaceStrings) {
    ViewDefinition viewDef(
        viewNss.db(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);
    ASSERT_EQ(viewDef.name(), viewNss);
    ASSERT_EQ(viewDef.viewOn(), backingNss);
}

TEST(ViewDefinitionTest, CopyConstructorProperlyClonesAllFields) {
    auto collator =
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    ViewDefinition originalView(
        viewNss.db(), viewNss.coll(), backingNss.coll(), samplePipeline, std::move(collator));
    ViewDefinition copiedView(originalView);

    ASSERT_EQ(originalView.name(), copiedView.name());
    ASSERT_EQ(originalView.viewOn(), copiedView.viewOn());
    ASSERT(std::equal(originalView.pipeline().begin(),
                      originalView.pipeline().end(),
                      copiedView.pipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
    ASSERT(CollatorInterface::collatorsMatch(originalView.defaultCollator(),
                                             copiedView.defaultCollator()));
}

TEST(ViewDefinitionTest, CopyAssignmentOperatorProperlyClonesAllFields) {
    auto collator =
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    ViewDefinition originalView(
        viewNss.db(), viewNss.coll(), backingNss.coll(), samplePipeline, std::move(collator));
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

DEATH_TEST(ViewDefinitionTest,
           SetViewOnFailsIfNewViewOnNotInSameDatabaseAsView,
           "Invariant failure _viewNss.db() == viewOnNss.db()") {
    ViewDefinition viewDef(
        viewNss.db(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);
    NamespaceString badViewOn("someOtherDb.someOtherCollection");
    viewDef.setViewOn(badViewOn);
}

TEST(ViewDefinitionTest, SetViewOnSucceedsIfNewViewOnIsInSameDatabaseAsView) {
    ViewDefinition viewDef(
        viewNss.db(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);
    ASSERT_EQ(viewDef.viewOn(), backingNss);

    NamespaceString newViewOn("testdb.othercollection");
    viewDef.setViewOn(newViewOn);
    ASSERT_EQ(newViewOn, viewDef.viewOn());
}

DEATH_TEST(ViewDefinitionTest,
           SetPiplineFailsIfPipelineTypeIsNotArray,
           "Invariant failure pipeline.type() == Array") {
    ViewDefinition viewDef(
        viewNss.db(), viewNss.coll(), backingNss.coll(), samplePipeline, nullptr);

    // We'll pass in a BSONElement that could be a valid array, but is BSONType::Object rather than
    // BSONType::Array.
    BSONObjBuilder builder;
    BSONArrayBuilder pipelineBuilder(builder.subobjStart("pipeline"));
    pipelineBuilder.append(BSON("skip" << 7));
    pipelineBuilder.append(BSON("limit" << 4));
    pipelineBuilder.doneFast();
    BSONObj newPipeline = builder.obj();

    viewDef.setPipeline(newPipeline["pipeline"]);
}

TEST(ViewDefinitionTest, SetPipelineSucceedsOnValidArrayBSONElement) {
    ViewDefinition viewDef(viewNss.db(), viewNss.coll(), backingNss.coll(), BSONObj(), nullptr);
    ASSERT(viewDef.pipeline().empty());

    BSONObj matchStage = BSON("match" << BSON("x" << 9));
    BSONObj sortStage = BSON("sort" << BSON("name" << -1));
    BSONObj newPipeline = BSON("pipeline" << BSON_ARRAY(matchStage << sortStage));

    viewDef.setPipeline(newPipeline["pipeline"]);

    std::vector<BSONObj> expectedPipeline{matchStage, sortStage};
    ASSERT(std::equal(expectedPipeline.begin(),
                      expectedPipeline.end(),
                      viewDef.pipeline().begin(),
                      SimpleBSONObjComparator::kInstance.makeEqualTo()));
}
}  // namespace
}  // namespace mongo
