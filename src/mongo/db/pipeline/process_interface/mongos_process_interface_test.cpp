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

#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class MongosProcessInterfaceForTest : public MongosProcessInterface {
public:
    using MongosProcessInterface::MongosProcessInterface;

    SupportingUniqueIndex fieldsHaveSupportingUniqueIndex(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const std::set<FieldPath>& fieldPaths) const override {
        return hasSupportingIndexForFields;
    }

    SupportingUniqueIndex hasSupportingIndexForFields{SupportingUniqueIndex::Full};
};

class MongosProcessInterfaceTest : public AggregationContextFixture {
public:
    MongosProcessInterfaceTest() {
        getExpCtx()->setInRouter(true);
    }

    auto makeProcessInterface() {
        return std::make_unique<MongosProcessInterfaceForTest>(nullptr);
    }
};

TEST_F(MongosProcessInterfaceTest,
       FailsToEnsureFieldsUniqueIftargetCollectionPlacementVersionIsSpecified) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion =
        boost::make_optional(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {0, 0}));
    auto processInterface = makeProcessInterface();

    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"_id"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51179);
}

TEST_F(MongosProcessInterfaceTest, FailsToEnsureFieldsUniqueIfNotSupportedByIndex) {
    auto expCtx = getExpCtx();
    auto targetCollectionPlacementVersion = boost::none;
    auto processInterface = makeProcessInterface();

    processInterface->hasSupportingIndexForFields =
        MongoProcessInterface::SupportingUniqueIndex::None;
    ASSERT_THROWS_CODE(
        processInterface->ensureFieldsUniqueOrResolveDocumentKey(
            expCtx, {{"x"}}, targetCollectionPlacementVersion, expCtx->getNamespaceString()),
        AssertionException,
        51190);
}
}  // namespace
}  // namespace mongo
