// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/write_component_serialization.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_shape {
namespace {

using namespace std::literals::string_view_literals;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("testDb.testColl");

BSONObj wrapValue(const Value& value) {
    return value.wrap(""sv);
}

class WriteComponentSerializationTest : public unittest::Test {
public:
    void setUp() final {
        _serviceContext = std::make_unique<QueryTestServiceContext>();
        _opCtx = _serviceContext->makeOperationContext();
    }

    ParsedUpdate parseUpdate(UpdateRequest& request) {
        _expCtx = ExpressionContextBuilder{}.fromRequest(_opCtx.get(), request).build();
        return uassertStatusOK(parsed_update_command::parse(
            _expCtx, &request, makeExtensionsCallback<ExtensionsCallbackNoop>()));
    }

    ParsedDelete parseDelete(DeleteRequest& request) {
        _expCtx = ExpressionContextBuilder{}.fromRequest(_opCtx.get(), request).build();
        return uassertStatusOK(parsed_delete_command::parse(
            _expCtx, &request, makeExtensionsCallback<ExtensionsCallbackNoop>()));
    }

    UpdateRequest makeUpdate(BSONObj query, write_ops::UpdateModification update) {
        UpdateRequest request;
        request.setNamespaceString(kNss);
        request.setQuery(query);
        request.setUpdateModification(std::move(update));
        request.setReturnDocs(UpdateRequest::RETURN_NEW);
        return request;
    }

    std::unique_ptr<QueryTestServiceContext> _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(WriteComponentSerializationTest, SerializesUpdateQueriesIncludingIdFastPath) {
    UpdateRequest request = makeUpdate(
        fromjson("{a: {$gte: 5}}"),
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$set: {b: 1}}")));
    auto parsed = parseUpdate(request);

    ASSERT_BSONOBJ_EQ_AUTO(
        R"({a: {$gte: 1}})",
        serializeWriteQuery(parsed,
                            SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({a: {$gte: "?number"}})",
        serializeWriteQuery(parsed, SerializationOptions::kDebugQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"HASH<a>": {$gte: "?number"}})",
        serializeWriteQuery(parsed, SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST));

    UpdateRequest idRequest = makeUpdate(
        fromjson("{_id: 42}"),
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$set: {b: 1}}")));
    auto parsedId = parseUpdate(idRequest);
    ASSERT_FALSE(parsedId.hasParsedFindCommand());
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({_id: {$eq: 1}})",
        serializeWriteQuery(parsedId,
                            SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({_id: {$eq: "?number"}})",
        serializeWriteQuery(parsedId, SerializationOptions::kDebugQueryShapeSerializeOptions));

    UpdateRequest objectIdRequest = makeUpdate(
        fromjson("{_id: {a: 42}}"),
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$set: {b: 1}}")));
    auto parsedObjectId = parseUpdate(objectIdRequest);
    ASSERT_FALSE(parsedObjectId.hasParsedFindCommand());
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({_id: {$eq: {"?": "?"}}})",
        serializeWriteQuery(parsedObjectId,
                            SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({_id: {$eq: "?object"}})",
        serializeWriteQuery(parsedObjectId,
                            SerializationOptions::kDebugQueryShapeSerializeOptions));
}

TEST_F(WriteComponentSerializationTest, SerializesDeleteQueriesIncludingIdFastPath) {
    DeleteRequest request;
    request.setNsString(kNss);
    request.setQuery(fromjson("{a: {$gte: 5}}"));
    auto parsed = parseDelete(request);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({a: {$gte: 1}})",
        serializeWriteQuery(parsed,
                            SerializationOptions::kRepresentativeQueryShapeSerializeOptions));

    DeleteRequest idRequest;
    idRequest.setNsString(kNss);
    idRequest.setQuery(fromjson("{_id: 42}"));
    auto parsedId = parseDelete(idRequest);
    ASSERT_FALSE(parsedId.hasParsedFindCommand());
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({_id: {$eq: 1}})",
        serializeWriteQuery(parsedId,
                            SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({_id: {$eq: "?number"}})",
        serializeWriteQuery(parsedId, SerializationOptions::kDebugQueryShapeSerializeOptions));
}

TEST_F(WriteComponentSerializationTest, OmitsInvalidSortForIdFastPath) {
    UpdateRequest request = makeUpdate(
        fromjson("{_id: 42}"),
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$set: {b: 1}}")));
    request.setSort(fromjson("{a: {$meta: 'invalid'}}"));
    auto parsed = parseUpdate(request);
    ASSERT_FALSE(parsed.hasParsedFindCommand());

    ASSERT_BSONOBJ_EQ(BSONObj{},
                      serializeFindAndModifySort(
                          parsed.parsedFind.get(),
                          parsed.getRequest()->getSort(),
                          _expCtx,
                          SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
}

TEST_F(WriteComponentSerializationTest, SerializesReplacementAndModifierUpdates) {
    UpdateRequest replacementRequest = makeUpdate(
        fromjson("{a: 5}"),
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{secret: 'payload'}")));
    auto replacement = parseUpdate(replacementRequest);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"": {"?": "?"}})",
        wrapValue(serializeUpdate(
            replacement, SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));
    ASSERT(ValueComparator{}.evaluate(
        serializeUpdate(replacement, SerializationOptions::kDebugQueryShapeSerializeOptions) ==
        Value(std::string("?object"))));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"": {secret: "payload"}})",
        wrapValue(serializeUpdate(replacement, SerializationOptions::kMarkIdentifiers_FOR_TEST)));

    UpdateRequest modifierRequest =
        makeUpdate(fromjson("{a: 5}"),
                   write_ops::UpdateModification::parseFromClassicUpdate(
                       fromjson("{$set: {item: 'ABC'}, $inc: {n: 5}}")));
    auto modifier = parseUpdate(modifierRequest);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"": {$inc: {n: 1}, $set: {item: "?"}}})",
        wrapValue(serializeUpdate(
            modifier, SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));
    ASSERT_BSONOBJ_EQ_AUTO(R"({"": {$inc: {n: "?number"}, $set: {item: "?string"}}})",
                           wrapValue(serializeUpdate(
                               modifier, SerializationOptions::kDebugQueryShapeSerializeOptions)));
}

TEST_F(WriteComponentSerializationTest, SerializesPipelineAndConstants) {
    UpdateRequest request = makeUpdate(fromjson("{a: 5}"),
                                       write_ops::UpdateModification(std::vector<BSONObj>{
                                           fromjson("{$set: {foo: '$$myVar', num: 42}}")}));
    request.setUpdateConstants(BSON("myVar" << "hello"));
    auto parsed = parseUpdate(request);

    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"": [{$set: {foo: "$$myVar", num: {$const: 1}}}]})",
        wrapValue(serializeUpdate(
            parsed, SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"": [{$set: {foo: "$$myVar", num: "?number"}}]})",
        wrapValue(serializeUpdate(parsed, SerializationOptions::kDebugQueryShapeSerializeOptions)));
    auto constants = serializeUpdateConstants(
        parsed, SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT(constants);
    ASSERT_BSONOBJ_EQ_AUTO(R"({myVar: "?"})", (*constants));
    auto debugConstants =
        serializeUpdateConstants(parsed, SerializationOptions::kDebugQueryShapeSerializeOptions);
    ASSERT(debugConstants);
    ASSERT_BSONOBJ_EQ_AUTO(R"({myVar: "?string"})", (*debugConstants));
}

TEST_F(WriteComponentSerializationTest, PreservesNoopModifierAndArrayFilterSerialization) {
    UpdateRequest noOpRequest = makeUpdate(
        fromjson("{a: 5}"),
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$inc: {}, $set: {}}")));
    auto noOp = parseUpdate(noOpRequest);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"": {$inc: {}, $set: {}}})",
        wrapValue(serializeUpdate(
            noOp, SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));

    UpdateRequest filteredRequest;
    filteredRequest.setNamespaceString(kNss);
    filteredRequest.setQuery(fromjson("{a: 5}"));
    filteredRequest.setUpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(
        fromjson("{$set: {'grades.$[g].score': 5}}")));
    filteredRequest.setArrayFilters({fromjson("{'g.kind': 'ok'}")});
    auto filtered = parseUpdate(filteredRequest);

    auto filters = serializeArrayFilters(
        filtered, SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT(filters);
    ASSERT_EQ(1U, filters->size());
    ASSERT_BSONOBJ_EQ_AUTO(R"({"g.kind": {$eq: "?"}})", (*filters)[0]);
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"": {$set: {"grades.$[g].score": 1}}})",
        wrapValue(serializeUpdate(
            filtered, SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));
}

}  // namespace
}  // namespace mongo::query_shape
