/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_list_mql_entities.h"

#include "mongo/base/string_data.h"
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

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

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
    StringMap<DocumentSource::ParserRegistration> availableDocSources = {
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
        Value(DOC("$listMqlEntities" << DOC("entityType"_sd << "aggregationStages"_sd))));
}

}  // namespace
}  // namespace mongo
