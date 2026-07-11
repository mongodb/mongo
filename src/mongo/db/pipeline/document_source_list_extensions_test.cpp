// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_list_extensions.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class DocumentSourceListExtensionsTest : public AggregationContextFixture {
public:
    boost::intrusive_ptr<DocumentSource> parse(const BSONObj& spec) {
        auto expCtx = getExpCtx();
        expCtx->setNamespaceString(NamespaceString::createNamespaceString_forTest(
            DatabaseName::kAdmin.db(omitTenant),
            NamespaceString::kCollectionlessAggregateCollection));
        return DocumentSourceListExtensions::createFromBson(spec.firstElement(), expCtx);
    }
};

TEST_F(DocumentSourceListExtensionsTest, ListExtensionDeserialization) {
    ASSERT_TRUE(parse(fromjson("{$listExtensions: {}}")));
}

TEST_F(DocumentSourceListExtensionsTest, InvalidListExtensionDeserialization) {
    ASSERT_THROWS_CODE(parse(fromjson("{$listExtensions: ''}")), AssertionException, 10983501);
    ASSERT_THROWS_CODE(parse(fromjson("{$listExtensions: {'improperField': 'aggregationStages'}}")),
                       AssertionException,
                       10983502);
}

TEST_F(DocumentSourceListExtensionsTest, DocumentSourceQueueCreated) {
    const auto documentSource = parse(fromjson("{$listExtensions: {}}"));
    ASSERT(dynamic_cast<DocumentSourceQueue*>(documentSource.get()));
}

}  // namespace
}  // namespace mongo
