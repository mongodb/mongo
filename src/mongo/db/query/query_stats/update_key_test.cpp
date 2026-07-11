// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/update_cmd_shape.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_stats {

namespace {
using namespace std::literals::string_view_literals;

using write_ops::UpdateCommandRequest;

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("testDB.testColl")};

static constexpr auto collectionType = query_shape::CollectionType::kCollection;

class UpdateKeyTest : public ServiceContextTest {
public:
    void setUp() override {
        _opCtx = makeOperationContext();
    }

    std::vector<std::unique_ptr<const Key>> makeUpdateKeys(
        const boost::intrusive_ptr<ExpressionContext>&, std::string_view cmd) {
        auto ucr = UpdateCommandRequest::parseOwned(fromjson(cmd));

        std::vector<std::unique_ptr<const Key>> keys;
        for (const auto& op : ucr.getUpdates()) {
            UpdateRequest request(op);
            request.setNamespaceString(kDefaultTestNss.nss());
            if (ucr.getLet()) {
                request.setLetParameters(*ucr.getLet());
            }
            auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx(), request).build();

            auto parsedUpdate = uassertStatusOK(parsed_update_command::parse(
                expCtx,
                &request,
                makeExtensionsCallback<ExtensionsCallbackReal>(expCtx->getOperationContext(),
                                                               &request.getNsString())));
            auto shape = std::make_unique<query_shape::UpdateCmdShape>(ucr, parsedUpdate, expCtx);

            auto key = std::make_unique<UpdateKey>(expCtx,
                                                   ucr,
                                                   parsedUpdate.getRequest()->getHint(),
                                                   std::move(shape),
                                                   collectionType);
            keys.push_back(std::move(key));
        }
        return keys;
    }

    std::unique_ptr<const Key> makeOneUpdateKey(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, std::string_view cmd) {
        auto keys = makeUpdateKeys(expCtx, cmd);
        ASSERT_EQ(keys.size(), 1);
        return std::move(keys.front());
    }

private:
    OperationContext* opCtx() {
        return _opCtx.get();
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(UpdateKeyTest, ReplacementUpdateCmdComponents) {
    // Create a request that none of the optional values are set.
    auto ucr = UpdateCommandRequest::parseOwned(fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB"
    })"sv));

    auto updateComponents = std::make_unique<UpdateCmdComponents>(ucr);

    // Confirm all the optional values are still present. They are provided with their default
    // values specified in the IDL.
    BSONObjBuilder bob;
    updateComponents->appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: true, bypassDocumentValidation: false })"), bob.obj());
}

TEST_F(UpdateKeyTest, IncludesOptionalValues) {
    // Create a request that all the optional values are included.
    auto ucr = UpdateCommandRequest::parseOwned(fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: true, upsert: true } ],
        bypassDocumentValidation: true,
        ordered: false,
        "$db": "testDB"
    })"sv));

    auto updateComponents = std::make_unique<UpdateCmdComponents>(ucr);

    // Verify that the optional values are reflected in the stats key components.
    BSONObjBuilder bob;
    updateComponents->appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ ordered: false, bypassDocumentValidation: true })"), bob.obj());
}

TEST_F(UpdateKeyTest, SizeOfUpdateCmdComponents) {
    auto update = fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false } ],
        "$db": "testDB"
    })"sv);
    auto ucr = UpdateCommandRequest::parse(std::move(update));

    auto updateComponents = std::make_unique<UpdateCmdComponents>(ucr);

    const auto minimumSize = sizeof(SpecificKeyComponents) + 2 /*size for bools*/;
    ASSERT_GTE(updateComponents->size(), minimumSize);
    ASSERT_LTE(updateComponents->size(), minimumSize + 8 /*padding*/);
}

TEST_F(UpdateKeyTest, EquivalentUpdateCmdComponentSizes) {
    // Create a request that has no values set.
    auto ucrNoSetValues = UpdateCommandRequest::parseOwned(fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB"
    })"sv));

    auto updateComponentsNoValues = std::make_unique<UpdateCmdComponents>(ucrNoSetValues);

    // Create a request that has all values set. None of these should affect the size.
    auto ucrAllValues = UpdateCommandRequest::parseOwned(fromjson(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: true, upsert: true } ],
        bypassDocumentValidation: true,
        ordered: false,
        "$db": "testDB"
    })"sv));

    auto updateComponentsAllValues = std::make_unique<UpdateCmdComponents>(ucrAllValues);

    // Verify their sizes are equal. This is because the optional parameters such as
    // 'bypassDocumentValidation' and 'ordered' are always provided with their default values when
    // they are not set from command requests.
    ASSERT_EQ(updateComponentsAllValues->size(), updateComponentsNoValues->size());
}

// Testing item in opCtx that should impact key size.
TEST_F(UpdateKeyTest, SizeOfUpdateKeyWithAndWithoutComment) {
    auto cmd = R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB"
    })"sv;
    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    auto keyWithoutComment = makeOneUpdateKey(expCtx, cmd);

    expCtx->getOperationContext()->setComment(BSON("comment" << " foo"));
    auto keyWithComment = makeOneUpdateKey(expCtx, cmd);
    ASSERT_LT(keyWithoutComment->size(), keyWithComment->size());
}

// Testing item in command request that should impact key size.
TEST_F(UpdateKeyTest, SizeOfUpdateKeyWithAndWithoutReadConcern) {
    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    auto keyWithoutReadConcern = makeOneUpdateKey(expCtx, R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB"
    })"sv);

    auto keyWithReadConcern = makeOneUpdateKey(expCtx, R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" } } ],
        "$db": "testDB",
        readConcern: {
            afterClusterTime: Timestamp(1654272333, 13),
            level: "majority"
        }
    })"sv);
    ASSERT_LT(keyWithoutReadConcern->size(), keyWithReadConcern->size());
}
}  // namespace
}  // namespace mongo::query_stats
