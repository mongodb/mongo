// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/find_and_modify_key.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/write_ops/delete_util.h"
#include "mongo/db/query/write_ops/parsed_find_and_modify.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/update/update_util.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>
#include <utility>

#include <absl/hash/hash.h>

namespace mongo::query_stats {
namespace {

using namespace std::literals::string_view_literals;

using write_ops::FindAndModifyCommandRequest;

class FindAndModifyKeyTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    /**
     * Makes a findAndModify key. The key is self-contained: the base Key owns copies of the hint
     * and other BSON components, and the shape owns its representative components.
     */
    std::unique_ptr<const Key> makeOneKey(std::string_view cmd) {
        // The parsed findAndModify borrows the command and the write request, so both are kept
        // alive in this scope while the self-contained key is built.
        auto request = FindAndModifyCommandRequest::parseOwned(fromjson(cmd));

        auto runtimeConstants = request.getLegacyRuntimeConstants()
            ? request.getLegacyRuntimeConstants()
            : boost::optional<LegacyRuntimeConstants>{Variables::generateRuntimeConstants(opCtx())};
        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(opCtx())
                          .ns(request.getNamespace())
                          .letParameters(request.getLet())
                          .runtimeConstants(std::move(runtimeConstants))
                          .build();

        auto shape = [&]() -> std::unique_ptr<query_shape::Shape> {
            if (request.getRemove().value_or(false)) {
                DeleteRequest deleteRequest;
                deleteRequest.setNsString(request.getNamespace());
                delete_util::makeDeleteRequest(opCtx(), request, false, &deleteRequest);
                auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
                    expCtx,
                    &request,
                    &deleteRequest,
                    makeExtensionsCallback<ExtensionsCallbackNoop>()));
                return std::make_unique<query_shape::FindAndModifyCmdShape>(
                    request, *parsed.getParsedDelete(), expCtx);
            }
            UpdateRequest updateRequest;
            updateRequest.setNamespaceString(request.getNamespace());
            update::makeUpdateRequest(opCtx(), request, boost::none /*explain*/, &updateRequest);
            auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
                expCtx,
                &request,
                &updateRequest,
                makeExtensionsCallback<ExtensionsCallbackNoop>()));
            return std::make_unique<query_shape::FindAndModifyCmdShape>(
                request, *parsed.getParsedUpdate(), expCtx);
        }();

        return std::make_unique<FindAndModifyKey>(
            expCtx, request, std::move(shape), query_benchmark_constants::kCollectionType);
    }

    FindAndModifyCmdComponents makeComponents(std::string_view cmd) {
        auto request = FindAndModifyCommandRequest::parseOwned(fromjson(cmd));
        return FindAndModifyCmdComponents(request);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(FindAndModifyKeyTest, DefaultFindAndModifyCmdComponents) {
    auto components = makeComponents(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);

    BSONObjBuilder bob;
    components.appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ new: false, bypassDocumentValidation: false })"), bob.obj());
}

TEST_F(FindAndModifyKeyTest, FindAndModifyCmdComponentsWithExplicitValues) {
    auto components = makeComponents(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        new: true,
        bypassDocumentValidation: true,
        $db: "testDB"
    })"sv);

    BSONObjBuilder bob;
    components.appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(fromjson(R"({ new: true, bypassDocumentValidation: true })"), bob.obj());
}

// 'new' must differentiate keys while leaving the query shape untouched.
TEST_F(FindAndModifyKeyTest, NewDifferentiatesKeyButNotShape) {
    auto newTrueKey = makeOneKey(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        new: true,
        $db: "testDB"
    })"sv);
    auto newFalseKey = makeOneKey(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        new: false,
        $db: "testDB"
    })"sv);

    ASSERT_NE(absl::HashOf(*newTrueKey), absl::HashOf(*newFalseKey));
    ASSERT_BSONOBJ_EQ(
        newTrueKey->universalComponents()._queryShape->toBson(
            opCtx(),
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
            SerializationContext::stateDefault()),
        newFalseKey->universalComponents()._queryShape->toBson(
            opCtx(),
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
            SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyKeyTest, HintDifferentiatesKey) {
    auto hintedKey = makeOneKey(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        hint: {x: 1},
        $db: "testDB"
    })"sv);
    auto unhintedKey = makeOneKey(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);

    ASSERT_NE(absl::HashOf(*hintedKey), absl::HashOf(*unhintedKey));
}

// A $natural sort is treated as a hint for key purposes, so it must differentiate keys even
// though it does not appear in the shape.
TEST_F(FindAndModifyKeyTest, NaturalSortActsAsHintForKey) {
    auto naturalKey = makeOneKey(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        sort: {$natural: 1},
        $db: "testDB"
    })"sv);
    auto plainKey = makeOneKey(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);

    ASSERT_NE(absl::HashOf(*naturalKey), absl::HashOf(*plainKey));

    // The $natural sort shows up as the key's hint.
    ASSERT_BSONOBJ_EQ(fromjson(R"({$natural: 1})"), naturalKey->universalComponents()._hintObj);
}

TEST_F(FindAndModifyKeyTest, BypassDocumentValidationDifferentiatesKey) {
    auto bypassKey = makeOneKey(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        bypassDocumentValidation: true,
        $db: "testDB"
    })"sv);
    auto normalKey = makeOneKey(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);

    ASSERT_NE(absl::HashOf(*bypassKey), absl::HashOf(*normalKey));
}

TEST_F(FindAndModifyKeyTest, KeySizeAccounting) {
    auto key = makeOneKey(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        new: true,
        $db: "testDB"
    })"sv);

    ASSERT_GTE(key->size(), sizeof(Key) + sizeof(FindAndModifyCmdComponents));
}

}  // namespace
}  // namespace mongo::query_stats
