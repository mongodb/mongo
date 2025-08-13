/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class MongoProcessInterfaceForTest : public StandaloneProcessInterface {
public:
    using StandaloneProcessInterface::StandaloneProcessInterface;

    SupportingUniqueIndex fieldsHaveSupportingUniqueIndex(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const std::set<FieldPath>& fields) const override {
        return hasSupportingIndexForFields;
    }

    void checkRoutingInfoEpochOrThrow(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      const NamespaceString&,
                                      ChunkVersion) const override {
        // Assume it always matches for our tests here.
        return;
    }

    SupportingUniqueIndex hasSupportingIndexForFields{SupportingUniqueIndex::Full};
};

class ProcessInterfaceStandaloneTest : public AggregationContextFixture {
public:
    auto makeProcessInterface() {
        return std::make_unique<MongoProcessInterfaceForTest>(nullptr);
    }
};

TEST_F(ProcessInterfaceStandaloneTest,
       FailsToEnsureFieldsUniqueIftargetCollectionPlacementVersionIsSpecifiedOnMongos) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion =
        boost::make_optional(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {0, 0}));
    auto processInterface = makeProcessInterface();

    // Test that 'targetCollectionPlacementVersion' is not accepted if not from router.
    expCtx->setFromRouter(false);
    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"_id"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51123);

    // Test that 'targetCollectionPlacementVersion' is accepted if from router.
    expCtx->setFromRouter(true);
    auto [joinKey, chunkVersion, supportingUniqueIndex] =
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"_id"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString());
    ASSERT_EQ(joinKey.size(), 1UL);
    ASSERT_EQ(joinKey.count(FieldPath("_id")), 1UL);
    ASSERT(chunkVersion);
    ASSERT_EQ(*chunkVersion, *targetCollectionPlacementVersion);
    ASSERT_EQ(supportingUniqueIndex, MongoProcessInterface::SupportingUniqueIndex::Full);
}

TEST_F(ProcessInterfaceStandaloneTest, FailsToEnsureFieldsUniqueIfJoinFieldsAreNotSentFromMongos) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion =
        boost::make_optional(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {0, 0}));
    auto processInterface = makeProcessInterface();

    expCtx->setFromRouter(true);
    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, boost::none, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51124);
}

TEST_F(ProcessInterfaceStandaloneTest,
       FailsToEnsureFieldsUniqueIfFieldsDoesNotHaveSupportingUniqueIndex) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion = boost::none;
    auto processInterface = makeProcessInterface();

    expCtx->setFromRouter(false);
    processInterface->hasSupportingIndexForFields =
        MongoProcessInterface::SupportingUniqueIndex::None;
    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"x"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51183);
}
}  // namespace
}  // namespace mongo
