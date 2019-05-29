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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceInternalShardFilterTest = AggregationContextFixture;

/**
 * ShardFilterer with default implementations to be used/extended by other tests.
 */
class ShardFiltererBaseForTest : public ShardFilterer {
public:
    DocumentBelongsResult documentBelongsToMe(const MatchableDocument& doc) const override {
        MONGO_UNREACHABLE;
    }

    bool isCollectionSharded() const override {
        MONGO_UNREACHABLE;
    }

    const KeyPattern& getKeyPattern() const override {
        MONGO_UNREACHABLE;
    }
};

/**
 * ShardFilterer which indicates that the collection isn't sharded.
 */
class UnshardedShardFilterer : public ShardFiltererBaseForTest {
public:
    bool isCollectionSharded() const override {
        return false;
    }
};

TEST_F(DocumentSourceInternalShardFilterTest, ShouldOptimizeAwayIfUnshardedCollection) {
    Pipeline::SourceContainer container;
    auto mock = DocumentSourceMock::createForTest({"{a: 1}", "{a: 2}"});

    container.push_back(mock);
    container.push_back(new DocumentSourceInternalShardFilter(
        getExpCtx(), std::make_unique<UnshardedShardFilterer>()));

    // Make 'it' point to the the shard filter source.
    auto it = container.begin();
    ++it;

    // The shard filter should remove itself.
    container.back()->optimizeAt(it, &container);
    ASSERT_EQUALS(1U, container.size());
}

TEST_F(DocumentSourceInternalShardFilterTest,
       ShouldOptimizeAwayIfUnshardedCollectionAndFirstInPipeline) {
    Pipeline::SourceContainer container;

    container.push_back(new DocumentSourceInternalShardFilter(
        getExpCtx(), std::make_unique<UnshardedShardFilterer>()));

    // Make 'it' point to the the shard filter source.
    auto it = container.begin();

    // The shard filter should remove itself.
    container.back()->optimizeAt(it, &container);
    ASSERT_EQUALS(0U, container.size());
}

/**
 * Indicate that the first 'n' documents passed through documentBelongsToMe() don't belong.
 */
class FirstNShardFilterer : public ShardFiltererBaseForTest {
public:
    FirstNShardFilterer(unsigned int n) : _numToFilter(n) {}

    DocumentBelongsResult documentBelongsToMe(const MatchableDocument& doc) const override {
        return i++ >= _numToFilter ? DocumentBelongsResult::kBelongs
                                   : DocumentBelongsResult::kDoesNotBelong;
    }

    bool isCollectionSharded() const override {
        return true;
    }

private:
    unsigned int _numToFilter;

    // mutable because documentBelongsToMe() is marked const.
    mutable unsigned int i = 0;
};

TEST_F(DocumentSourceInternalShardFilterTest, FiltersDocuments) {
    Pipeline::SourceContainer container;
    auto mock = DocumentSourceMock::createForTest({"{a: 1}", "{a: 2}", "{a: 3}"});

    const auto nToFilter = 2;
    DocumentSourceInternalShardFilter filter(getExpCtx(),
                                             std::make_unique<FirstNShardFilterer>(nToFilter));
    filter.setSource(mock.get());

    // The first two documents should get filtered out.
    auto next = filter.getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_VALUE_EQ(Value(3), next.getDocument().getField("a"));

    ASSERT_TRUE(filter.getNext().isEOF());
}

/**
 * Indicate that documents don't have a shard key.
 */
class ShardFiltererNoShardKey : public ShardFiltererBaseForTest {
public:
    ShardFiltererNoShardKey() : _kp(BSON("b" << 1)) {}

    DocumentBelongsResult documentBelongsToMe(const MatchableDocument& doc) const override {
        return DocumentBelongsResult::kNoShardKey;
    }

    bool isCollectionSharded() const override {
        return true;
    }

    const KeyPattern& getKeyPattern() const override {
        return _kp;
    }

private:
    KeyPattern _kp;
};

TEST_F(DocumentSourceInternalShardFilterTest, SkipDocumentsWithoutShardKey) {
    Pipeline::SourceContainer container;
    auto mock = DocumentSourceMock::createForTest({"{a: 1}", "{a: 2}", "{a: 3}"});

    DocumentSourceInternalShardFilter filter(getExpCtx(),
                                             std::make_unique<ShardFiltererNoShardKey>());
    filter.setSource(mock.get());

    // The call to getNext() return nothing.
    ASSERT_TRUE(filter.getNext().isEOF());
}

}  // namespace
}  // namespace mongo
