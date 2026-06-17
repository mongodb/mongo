/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo::exec::agg {
namespace {

using LookupResult = SingleDocumentLookupExecutor::LookupResult;
using HandledStatus = LookupResult::HandledStatus;

/**
 * Mock executor that records every performLookup() / detach / reattach call and returns a
 * preconfigured result, so a test can assert the PrimaryWithFallback chain's dispatch and
 * argument-forwarding behaviour without a storage engine or a real executor.
 */
class SingleDocumentLookupExecutorMock : public SingleDocumentLookupExecutor {
public:
    explicit SingleDocumentLookupExecutorMock(LookupResult result) : _result(std::move(result)) {}

    LookupResult performLookup(const boost::intrusive_ptr<ExpressionContext>&,
                               const NamespaceString& nss,
                               boost::optional<UUID> collectionUUID,
                               const Document& documentKey,
                               boost::optional<Timestamp> afterClusterTime) override {
        ++performLookupCalls;
        lastNss = nss;
        lastCollectionUUID = collectionUUID;
        lastDocumentKey = documentKey;
        lastAfterClusterTime = afterClusterTime;
        return _result;
    }

    void detachFromOperationContext() override {
        ++detachCalls;
    }
    void reattachToOperationContext(OperationContext*) override {
        ++reattachCalls;
    }

    int performLookupCalls = 0;
    int detachCalls = 0;
    int reattachCalls = 0;
    NamespaceString lastNss;
    boost::optional<UUID> lastCollectionUUID;
    Document lastDocumentKey;
    boost::optional<Timestamp> lastAfterClusterTime;

private:
    LookupResult _result;
};

class PrimaryWithFallbackSingleDocumentLookupExecutorTest : public unittest::Test {
protected:
    // Builds a chain from two mock executors and stashes raw pointers to them (the chain owns them;
    // the pointers stay valid for the chain's lifetime) so tests can inspect call counts.
    PrimaryWithFallbackSingleDocumentLookupExecutor makeChain(LookupResult primaryResult,
                                                              LookupResult fallbackResult) {
        auto primaryOwned =
            std::make_unique<SingleDocumentLookupExecutorMock>(std::move(primaryResult));
        auto fallbackOwned =
            std::make_unique<SingleDocumentLookupExecutorMock>(std::move(fallbackResult));
        _primary = primaryOwned.get();
        _fallback = fallbackOwned.get();
        return PrimaryWithFallbackSingleDocumentLookupExecutor(std::move(primaryOwned),
                                                               std::move(fallbackOwned));
    }

    SingleDocumentLookupExecutor::LookupResult runLookup(
        PrimaryWithFallbackSingleDocumentLookupExecutor& chain) {
        return chain.performLookup(boost::intrusive_ptr<ExpressionContext>{},
                                   nss,
                                   collectionUUID,
                                   documentKey,
                                   afterClusterTime);
    }

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testdb.testcoll");
    const UUID collectionUUID = UUID::gen();
    const Document documentKey = Document{{"_id", 7}};
    const Timestamp afterClusterTime = Timestamp(100, 1);

    SingleDocumentLookupExecutorMock* _primary = nullptr;
    SingleDocumentLookupExecutorMock* _fallback = nullptr;
};

using PrimaryWithFallbackSingleDocumentLookupExecutorDeathTest =
    PrimaryWithFallbackSingleDocumentLookupExecutorTest;

TEST_F(PrimaryWithFallbackSingleDocumentLookupExecutorTest, KDocumentFoundDoesNotInvokeFallback) {
    auto bsonObj = BSON("_id" << 7 << "v" << 1);
    auto chain = makeChain({HandledStatus::kDocumentFound, Document(bsonObj)},
                           {HandledStatus::kDocumentNotFound, boost::none});

    auto result = runLookup(chain);

    ASSERT(result.status == HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), bsonObj);
    ASSERT_EQ(_primary->performLookupCalls, 1);
    ASSERT_EQ(_fallback->performLookupCalls, 0);
}

TEST_F(PrimaryWithFallbackSingleDocumentLookupExecutorTest,
       KDocumentNotFoundDoesNotInvokeFallback) {
    auto chain = makeChain({HandledStatus::kDocumentNotFound, boost::none},
                           {HandledStatus::kDocumentFound, Document{{"_id", 7}}});

    auto result = runLookup(chain);

    ASSERT(result.status == HandledStatus::kDocumentNotFound);
    ASSERT_FALSE(result.document.has_value());
    ASSERT_EQ(_primary->performLookupCalls, 1);
    ASSERT_EQ(_fallback->performLookupCalls, 0);
}

TEST_F(PrimaryWithFallbackSingleDocumentLookupExecutorTest,
       KNotHandledInvokesFallbackExactlyOnceWithIdenticalArgs) {
    auto bsonObj = BSON("_id" << 7 << "v" << 2);
    auto chain = makeChain({HandledStatus::kNotHandled, boost::none},
                           {HandledStatus::kDocumentFound, Document(bsonObj)});

    auto result = runLookup(chain);

    // Result comes from the fallback.
    ASSERT(result.status == HandledStatus::kDocumentFound);
    ASSERT_BSONOBJ_EQ(result.document->toBson(), bsonObj);

    // Primary tried once, fallback invoked exactly once.
    ASSERT_EQ(_primary->performLookupCalls, 1);
    ASSERT_EQ(_fallback->performLookupCalls, 1);

    // Fallback received the identical arguments the chain was called with.
    ASSERT_EQ(_fallback->lastNss, nss);
    ASSERT(_fallback->lastCollectionUUID == boost::optional<UUID>(collectionUUID));
    ASSERT_BSONOBJ_EQ(_fallback->lastDocumentKey.toBson(), documentKey.toBson());
    ASSERT(_fallback->lastAfterClusterTime == boost::optional<Timestamp>(afterClusterTime));
}

TEST_F(PrimaryWithFallbackSingleDocumentLookupExecutorTest,
       DetachAndReattachForwardedToBothChildren) {
    auto chain = makeChain({HandledStatus::kNotHandled, boost::none},
                           {HandledStatus::kDocumentNotFound, boost::none});

    chain.detachFromOperationContext();
    chain.reattachToOperationContext(nullptr);

    ASSERT_EQ(_primary->detachCalls, 1);
    ASSERT_EQ(_fallback->detachCalls, 1);
    ASSERT_EQ(_primary->reattachCalls, 1);
    ASSERT_EQ(_fallback->reattachCalls, 1);
}

TEST_F(PrimaryWithFallbackSingleDocumentLookupExecutorTest,
       PrimaryAndFallbackForTestExposeComposedChildren) {
    auto chain = makeChain({HandledStatus::kNotHandled, boost::none},
                           {HandledStatus::kDocumentNotFound, boost::none});

    ASSERT_EQ(chain.primary_forTest(), _primary);
    ASSERT_EQ(chain.fallback_forTest(), _fallback);
}

DEATH_TEST_REGEX_F(PrimaryWithFallbackSingleDocumentLookupExecutorDeathTest,
                   NullPrimaryTripsConstructor,
                   "Tripwire assertion.*12840800") {
    ASSERT_THROWS_CODE(PrimaryWithFallbackSingleDocumentLookupExecutor(
                           nullptr,
                           std::make_unique<SingleDocumentLookupExecutorMock>(
                               LookupResult{HandledStatus::kDocumentNotFound, boost::none})),
                       AssertionException,
                       12840800);
}

DEATH_TEST_REGEX_F(PrimaryWithFallbackSingleDocumentLookupExecutorDeathTest,
                   NullFallbackTripsConstructor,
                   "Tripwire assertion.*12840801") {
    ASSERT_THROWS_CODE(PrimaryWithFallbackSingleDocumentLookupExecutor(
                           std::make_unique<SingleDocumentLookupExecutorMock>(
                               LookupResult{HandledStatus::kNotHandled, boost::none}),
                           nullptr),
                       AssertionException,
                       12840801);
}

}  // namespace
}  // namespace mongo::exec::agg
