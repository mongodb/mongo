// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/document_source_extension_for_query_shape.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/host_services.h"
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

}  // namespace mongo::extension
