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

#include "mongo/db/pipeline/document_source_internal_shard_filter.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cstddef>
#include <list>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceInternalShardFilterTest = AggregationContextFixture;

/**
 * ShardFilterer with default implementations to be used/extended by other tests.
 */
class ShardFiltererBaseForTest : public ShardFilterer {
public:
    std::unique_ptr<ShardFilterer> clone() const override {
        MONGO_UNREACHABLE;
    }

    DocumentBelongsResult documentBelongsToMe(const BSONObj& obj) const override {
        MONGO_UNREACHABLE;
    }

    bool isCollectionSharded() const override {
        MONGO_UNREACHABLE;
    }

    const KeyPattern& getKeyPattern() const override {
        MONGO_UNREACHABLE;
    }

    bool keyBelongsToMe(const BSONObj& obj) const override {
        MONGO_UNREACHABLE;
    }

    size_t getApproximateSize() const override {
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
    DocumentSourceContainer container;
    auto mock = DocumentSourceMock::createForTest({"{a: 1}", "{a: 2}"}, getExpCtx());

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
    DocumentSourceContainer container;

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

    DocumentBelongsResult documentBelongsToMe(const BSONObj& obj) const override {
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
    DocumentSourceContainer container;
    auto mock = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 2}", "{a: 3}"}, getExpCtx());

    const auto nToFilter = 2;
    auto filterSource = make_intrusive<DocumentSourceInternalShardFilter>(
        getExpCtx(), std::make_unique<FirstNShardFilterer>(nToFilter));
    auto filterStage = exec::agg::buildStage(filterSource);
    filterStage->setSource(mock.get());

    // The first two documents should get filtered out.
    auto next = filterStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_VALUE_EQ(Value(3), next.getDocument().getField("a"));

    ASSERT_TRUE(filterStage->getNext().isEOF());
}

/**
 * Indicate that documents don't have a shard key.
 */
class ShardFiltererNoShardKey : public ShardFiltererBaseForTest {
public:
    ShardFiltererNoShardKey() : _kp(BSON("b" << 1)) {}

    DocumentBelongsResult documentBelongsToMe(const BSONObj& obj) const override {
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
    DocumentSourceContainer container;
    auto mock = exec::agg::MockStage::createForTest({"{a: 1}", "{a: 2}", "{a: 3}"}, getExpCtx());

    auto filterSource = make_intrusive<DocumentSourceInternalShardFilter>(
        getExpCtx(), std::make_unique<ShardFiltererNoShardKey>());
    auto filterStage = exec::agg::buildStage(filterSource);
    filterStage->setSource(mock.get());

    // The call to getNext() return nothing.
    ASSERT_TRUE(filterStage->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
