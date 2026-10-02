// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/document_source_extension_for_query_shape.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::extension {

class DocumentSourceExtensionForQueryShapeTest : public AggregationContextFixture {
public:
    DocumentSourceExtensionForQueryShapeTest() : DocumentSourceExtensionForQueryShapeTest(_nss) {}
    explicit DocumentSourceExtensionForQueryShapeTest(NamespaceString nsString)
        : AggregationContextFixture(std::move(nsString)) {};

    void setUp() override {
        AggregationContextFixture::setUp();
        extension::sdk::HostServicesAPI::setHostServices(
            &extension::host_connector::HostServicesAdapter::get());
    }

protected:
    static inline NamespaceString _nss = NamespaceString::createNamespaceString_forTest(
        boost::none, "document_source_extension_for_query_shape_test");

    sdk::ExtensionAggStageDescriptorAdapter _transformStageDescriptor{
        sdk::shared_test_stages::TransformAggStageDescriptor::make()};
};

// Literal-preserving serialization (e.g. $rankFusion's desugarer re-serializing a non-first input
// pipeline into the $unionWith it constructs) must round-trip the original user-provided stage.
TEST_F(DocumentSourceExtensionForQueryShapeTest, SerializeWithDefaultOptsRoundTrips) {
    auto rawStage = BSON(sdk::shared_test_stages::TransformAggStageDescriptor::kStageName
                         << BSON("foo" << true));

    auto expandable = host::DocumentSourceExtensionForQueryShape::create(
        getExpCtx(), rawStage, AggStageDescriptorHandle(&_transformStageDescriptor));

    auto serialized = expandable->serialize(query_shape::SerializationOptions{});
    ASSERT_BSONOBJ_EQ(serialized.getDocument().toBson(), rawStage);
}

// Serializing for query stats (literals abstracted to debug type strings) must also succeed and
// delegate to the parse node's query shape generation without tripping a tripwire.
TEST_F(DocumentSourceExtensionForQueryShapeTest, SerializeForQueryStatsSucceeds) {
    auto rawStage = BSON(sdk::shared_test_stages::TransformAggStageDescriptor::kStageName
                         << BSON("foo" << true));

    auto expandable = host::DocumentSourceExtensionForQueryShape::create(
        getExpCtx(), rawStage, AggStageDescriptorHandle(&_transformStageDescriptor));

    auto serialized =
        expandable->serialize(query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions);
    ASSERT_FALSE(serialized.getDocument().toBson().isEmpty());
}

// Same contract for the parse-node create() overload (the path taken when a lite-parsed extension
// stage is converted to a DocumentSource via StageParams).
TEST_F(DocumentSourceExtensionForQueryShapeTest,
       SerializeWithDefaultOptsFromParseNodeReturnsOriginalStage) {
    auto rawStage = BSON(sdk::shared_test_stages::TransformAggStageDescriptor::kStageName
                         << BSON("foo" << true));

    auto descriptor = AggStageDescriptorHandle(&_transformStageDescriptor);
    auto expandable = host::DocumentSourceExtensionForQueryShape::create(
        getExpCtx(), descriptor->parse(rawStage), rawStage);

    auto serialized = expandable->serialize(query_shape::SerializationOptions{});
    ASSERT_BSONOBJ_EQ(serialized.getDocument().toBson(), rawStage);
}

// -----------------------------------------------------------------------------
// The host must reject extension stages whose query shape is not a single-field
// {$stageName: ...} object because these shapes would otherwise be stored in the $queryStats store
// and fail re-parse when later read back.
// -----------------------------------------------------------------------------
namespace {
enum class StubShapeKind { kEmpty, kMultiField, kSingleField, kWrongName };

class StubShapeParseNode
    : public sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode> {
public:
    StubShapeParseNode(std::string_view stageName,
                       const mongo::BSONObj& arguments,
                       StubShapeKind kind)
        : sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode>(stageName,
                                                                                arguments),
          _kind(kind) {}

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        switch (_kind) {
            case StubShapeKind::kEmpty:
                return mongo::BSONObj();
            case StubShapeKind::kMultiField:
                return BSON("a" << 1 << "b" << 2);
            case StubShapeKind::kSingleField:
                return BSON(getName() << mongo::BSONObj());
            case StubShapeKind::kWrongName:
                return BSON("$other" << mongo::BSONObj());
        }
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<StubShapeParseNode>(getName(), _arguments, _kind);
    }

private:
    StubShapeKind _kind;
};

class StubShapeDescriptor
    : public sdk::TestStageDescriptor<"$stubShape", StubShapeParseNode, false> {
public:
    explicit StubShapeDescriptor(StubShapeKind kind) : _kind(kind) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        auto arguments = sdk::validateStageDefinition(stageBson, kStageName, false);
        return std::make_unique<StubShapeParseNode>(kStageName, arguments, _kind);
    }

private:
    StubShapeKind _kind;
};
}  // namespace

TEST_F(DocumentSourceExtensionForQueryShapeTest, RejectsEmptyExtensionQueryShape) {
    sdk::ExtensionAggStageDescriptorAdapter descriptor{
        std::make_unique<StubShapeDescriptor>(StubShapeKind::kEmpty)};
    auto expandable = host::DocumentSourceExtensionForQueryShape::create(
        getExpCtx(), BSON("$stubShape" << BSONObj()), AggStageDescriptorHandle(&descriptor));
    ASSERT_THROWS_CODE(
        expandable->serialize(query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions),
        AssertionException,
        13462501);
}

TEST_F(DocumentSourceExtensionForQueryShapeTest, RejectsMultiFieldExtensionQueryShape) {
    sdk::ExtensionAggStageDescriptorAdapter descriptor{
        std::make_unique<StubShapeDescriptor>(StubShapeKind::kMultiField)};
    auto expandable = host::DocumentSourceExtensionForQueryShape::create(
        getExpCtx(), BSON("$stubShape" << BSONObj()), AggStageDescriptorHandle(&descriptor));
    ASSERT_THROWS_CODE(
        expandable->serialize(query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions),
        AssertionException,
        13462501);
}

TEST_F(DocumentSourceExtensionForQueryShapeTest, RejectsWrongStageNameExtensionQueryShape) {
    sdk::ExtensionAggStageDescriptorAdapter descriptor{
        std::make_unique<StubShapeDescriptor>(StubShapeKind::kWrongName)};
    auto expandable = host::DocumentSourceExtensionForQueryShape::create(
        getExpCtx(), BSON("$stubShape" << BSONObj()), AggStageDescriptorHandle(&descriptor));
    ASSERT_THROWS_CODE(
        expandable->serialize(query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions),
        AssertionException,
        13462502);
}

TEST_F(DocumentSourceExtensionForQueryShapeTest, AcceptsSingleFieldExtensionQueryShape) {
    sdk::ExtensionAggStageDescriptorAdapter descriptor{
        std::make_unique<StubShapeDescriptor>(StubShapeKind::kSingleField)};
    auto expandable = host::DocumentSourceExtensionForQueryShape::create(
        getExpCtx(), BSON("$stubShape" << BSONObj()), AggStageDescriptorHandle(&descriptor));
    auto serialized =
        expandable->serialize(query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions);
    ASSERT_EQ(serialized.getDocument().toBson().nFields(), 1);
}

}  // namespace mongo::extension
