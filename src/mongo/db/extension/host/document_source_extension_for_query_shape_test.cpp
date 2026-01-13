/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/unittest/death_test.h"
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

    sdk::ExtensionAggStageDescriptor _transformStageDescriptor{
        sdk::shared_test_stages::TransformAggStageDescriptor::make()};
};

using DocumentSourceExtensionForQueryShapeTestDeathTest = DocumentSourceExtensionForQueryShapeTest;
DEATH_TEST_F(DocumentSourceExtensionForQueryShapeTestDeathTest,
             SerializeWithWrongOptsFails,
             "10978000") {
    auto rawStage = BSON(sdk::shared_test_stages::TransformAggStageDescriptor::kStageName
                         << BSON("foo" << true));

    auto expandable = host::DocumentSourceExtensionForQueryShape::create(
        getExpCtx(), rawStage, AggStageDescriptorHandle(&_transformStageDescriptor));

    [[maybe_unused]] auto serialized = expandable->serialize(SerializationOptions{});
}

}  // namespace mongo::extension
