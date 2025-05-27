/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/pipeline/search/document_source_search.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/intrusive_ptr.hpp>

namespace mongo {

namespace {

using boost::intrusive_ptr;
using std::list;
using std::vector;

class SearchTest : service_context_test::WithSetupTransportLayer,
                   public AggregationContextFixture {};

struct MockMongoInterface final : public StubMongoProcessInterface {
    bool inShardedEnvironment(OperationContext* opCtx) const override {
        return false;
    }
};

TEST_F(SearchTest, ShouldSerializeAllNecessaryFieldsAtUnspecifiedVerbosity) {
    const auto mongotQuery = fromjson("{term: 'asdf'}");
    const auto stageObj = BSON("$search" << mongotQuery);

    auto expCtx = getExpCtx();
    expCtx->setMongoProcessInterface(std::make_unique<MockMongoInterface>());
    expCtx->setUUID(UUID::gen());

    intrusive_ptr<DocumentSource> searchDS =
        DocumentSourceSearch::createFromBson(stageObj.firstElement(), expCtx);
    list<intrusive_ptr<DocumentSource>> results =
        dynamic_cast<DocumentSourceSearch*>(searchDS.get())->desugar();
    ASSERT_EQUALS(results.size(), 2UL);

    const auto* mongotRemoteStage =
        dynamic_cast<DocumentSourceInternalSearchMongotRemote*>(results.front().get());
    ASSERT(mongotRemoteStage);

    const auto* idLookupStage =
        dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(results.back().get());
    ASSERT(idLookupStage);

    vector<Value> explainedStages;
    mongotRemoteStage->serializeToArray(explainedStages);
    idLookupStage->serializeToArray(explainedStages);
    ASSERT_EQUALS(explainedStages.size(), 2UL);

    auto mongotRemoteExplain = explainedStages[0];
    ASSERT_DOCUMENT_EQ(mongotRemoteExplain.getDocument(),
                       Document({{"$_internalSearchMongotRemote", Document(mongotQuery)}}));

    auto idLookupExplain = explainedStages[1];
    ASSERT_DOCUMENT_EQ(idLookupExplain.getDocument(),
                       Document({{"$_internalSearchIdLookup", Document()}}));
}

TEST_F(SearchTest, ShouldFailToParseIfSpecIsNotObject) {
    const auto specObj = fromjson("{$search: 1}");
    ASSERT_THROWS_CODE(DocumentSourceSearch::createFromBson(specObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

}  // namespace
}  // namespace mongo
