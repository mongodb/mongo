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

#include "mongo/db/pipeline/document_source_list_extensions.h"

#include "mongo/base/string_data.h"
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
