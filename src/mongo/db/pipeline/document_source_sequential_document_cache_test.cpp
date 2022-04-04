/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceSequentialDocumentCacheTest = AggregationContextFixture;

const long long kDefaultMaxCacheSize = internalDocumentSourceLookupCacheSizeBytes.load();

TEST_F(DocumentSourceSequentialDocumentCacheTest, ReturnsEOFOnSubsequentCallsAfterSourceExhausted) {
    SequentialDocumentCache cache(kDefaultMaxCacheSize);
    auto documentCache = DocumentSourceSequentialDocumentCache::create(getExpCtx(), &cache);

    auto source = DocumentSourceMock::createForTest({"{a: 1, b: 2}", "{a: 3, b: 4}"});
    documentCache->setSource(source.get());

    ASSERT(documentCache->getNext().isAdvanced());
    ASSERT(documentCache->getNext().isAdvanced());
    ASSERT(documentCache->getNext().isEOF());
    ASSERT(documentCache->getNext().isEOF());
}

TEST_F(DocumentSourceSequentialDocumentCacheTest, ReturnsEOFAfterCacheExhausted) {
    SequentialDocumentCache cache(kDefaultMaxCacheSize);
    cache.add(DOC("_id" << 0));
    cache.add(DOC("_id" << 1));
    cache.freeze();

    auto documentCache = DocumentSourceSequentialDocumentCache::create(getExpCtx(), &cache);

    ASSERT(cache.isServing());
    ASSERT(documentCache->getNext().isAdvanced());
    ASSERT(documentCache->getNext().isAdvanced());
    ASSERT(documentCache->getNext().isEOF());
    ASSERT(documentCache->getNext().isEOF());
}
}  // namespace
}  // namespace mongo
