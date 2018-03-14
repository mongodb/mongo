/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceSkipTest = AggregationContextFixture;

TEST_F(DocumentSourceSkipTest, ShouldPropagatePauses) {
    auto skip = DocumentSourceSkip::create(getExpCtx(), 2);
    auto mock = DocumentSourceMock::create({Document(),
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document(),
                                            Document(),
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            DocumentSource::GetNextResult::makePauseExecution()});
    skip->setSource(mock.get());

    // Skip the first document.
    ASSERT_TRUE(skip->getNext().isPaused());

    // Skip one more, then advance.
    ASSERT_TRUE(skip->getNext().isAdvanced());

    ASSERT_TRUE(skip->getNext().isPaused());
    ASSERT_TRUE(skip->getNext().isPaused());

    ASSERT_TRUE(skip->getNext().isEOF());
    ASSERT_TRUE(skip->getNext().isEOF());
    ASSERT_TRUE(skip->getNext().isEOF());
}

TEST_F(DocumentSourceSkipTest, SkipsChainedTogetherShouldNotOverFlowWhenOptimizing) {
    auto skipShort = DocumentSourceSkip::create(getExpCtx(), 1);
    auto skipLong = DocumentSourceSkip::create(getExpCtx(), std::numeric_limits<long long>::max());
    Pipeline::SourceContainer overFlowContainer;
    overFlowContainer.push_back(skipShort);
    overFlowContainer.push_back(skipLong);
    skipShort->doOptimizeAt(overFlowContainer.begin(), &overFlowContainer);
    ASSERT_EQUALS(overFlowContainer.size(), 2U);
    ASSERT_EQUALS(skipShort->getSkip(), 1U);
    ASSERT_EQUALS(skipLong->getSkip(), std::numeric_limits<long long>::max());

    auto skipFirst = DocumentSourceSkip::create(getExpCtx(), 1);
    auto skipSecond = DocumentSourceSkip::create(getExpCtx(), 1);
    Pipeline::SourceContainer containerOptimized;
    containerOptimized.push_back(skipFirst);
    containerOptimized.push_back(skipSecond);
    skipFirst->doOptimizeAt(containerOptimized.begin(), &containerOptimized);
    ASSERT_EQUALS(containerOptimized.size(), 1U);
    ASSERT_EQUALS(skipFirst->getSkip(), 2);
}

}  // namespace
}  // namespace mongo
