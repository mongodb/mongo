// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_list_mql_entities.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/list_mql_entities_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_mql_entities_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class DocumentSourceListMqlEntitiesTest : public AggregationContextFixture {
public:
    boost::intrusive_ptr<DocumentSource> parse(const BSONObj& spec) {
        auto expCtx = getExpCtx();
        expCtx->setNamespaceString(NamespaceString::createNamespaceString_forTest(
            DatabaseName::kAdmin.db(omitTenant),
            NamespaceString::kCollectionlessAggregateCollection));
        return DocumentSourceListMqlEntities::createFromBson(spec.firstElement(), expCtx);
    }
};

TEST_F(DocumentSourceListMqlEntitiesTest, ParserRejectsInvalid) {
    ASSERT_THROWS(parse(fromjson("{$listMqlEntities: ''}")), AssertionException);
    ASSERT_THROWS(parse(fromjson("{$listMqlEntities: {}}")), AssertionException);
    ASSERT_THROWS(parse(fromjson("{$listMqlEntities: {'improperField': 'aggregationStages'}}")),
                  AssertionException);
    ASSERT_THROWS(parse(fromjson("{$listMqlEntities: {'entityType': 'improperValue'}}")),
                  AssertionException);
    ASSERT_DOES_NOT_THROW(
        parse(fromjson("{$listMqlEntities: {'entityType': 'aggregationStages'}}")));
}

TEST_F(DocumentSourceListMqlEntitiesTest, AggStages) {
    // Verify that the order of result is sorted.
    LiteParsedDocumentSource::ParserMap availableDocSources = {
        {"docSource1", {}}, {"docSource3", {}}, {"docSource2", {}}};
    auto stage =
        exec::agg::ListMqlEntitiesStage::create_forTest(DocumentSourceListMqlEntities::kStageName,
                                                        getExpCtx(),
                                                        MqlEntityTypeEnum::aggregationStages,
                                                        availableDocSources);
    ASSERT_EQ("docSource1", stage->getNext().getDocument().getField("name").getString());
    ASSERT_EQ("docSource2", stage->getNext().getDocument().getField("name").getString());
    ASSERT_EQ("docSource3", stage->getNext().getDocument().getField("name").getString());
    ASSERT(stage->getNext().isEOF());
}

TEST_F(DocumentSourceListMqlEntitiesTest, Serialize) {
    auto listMqlEntitiesDS =
        parse(fromjson("{$listMqlEntities: {'entityType': 'aggregationStages'}}"));
    ASSERT_VALUE_EQ(
        boost::dynamic_pointer_cast<DocumentSourceListMqlEntities>(listMqlEntitiesDS)->serialize(),
        Value(DOC("$listMqlEntities" << DOC("entityType"sv << "aggregationStages"sv))));
}

}  // namespace
}  // namespace mongo
