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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class MongosProcessInterfaceForTest : public MongosProcessInterface {
public:
    using MongosProcessInterface::MongosProcessInterface;

    bool fieldsHaveSupportingUniqueIndex(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const NamespaceString& nss,
                                         const std::set<FieldPath>& fieldPaths) const override {
        return hasSupportingIndexForFields;
    }

    bool hasSupportingIndexForFields{true};
};

class MongosProcessInterfaceTest : public AggregationContextFixture {
public:
    MongosProcessInterfaceTest() {
        getExpCtx()->inMongos = true;
    }

    auto makeProcessInterface() {
        return std::make_unique<MongosProcessInterfaceForTest>(nullptr);
    }
};

TEST_F(MongosProcessInterfaceTest, FailsToEnsureFieldsUniqueIfTargetCollectionVersionIsSpecified) {
    auto expCtx = getExpCtx();
    auto targetCollectionVersion =
        boost::make_optional(ChunkVersion(0, 0, OID::gen(), Timestamp(1, 1)));
    auto processInterface = makeProcessInterface();

    ASSERT_THROWS_CODE(processInterface->ensureFieldsUniqueOrResolveDocumentKey(
                           expCtx, {{"_id"}}, targetCollectionVersion, expCtx->ns),
                       AssertionException,
                       51179);
}

TEST_F(MongosProcessInterfaceTest, FailsToEnsureFieldsUniqueIfNotSupportedByIndex) {
    auto expCtx = getExpCtx();
    auto targetCollectionVersion = boost::none;
    auto processInterface = makeProcessInterface();

    processInterface->hasSupportingIndexForFields = false;
    ASSERT_THROWS_CODE(processInterface->ensureFieldsUniqueOrResolveDocumentKey(
                           expCtx, {{"x"}}, targetCollectionVersion, expCtx->ns),
                       AssertionException,
                       51190);
}
}  // namespace
}  // namespace mongo
