/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 */

#include "mongo/db/modules/enterprise/src/streams/exec/embed/embed_operator.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/modules/enterprise/src/streams/exec/embed/planning.h"
#include "mongo/db/modules/enterprise/src/streams/exec/tests/operator_test_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/http_client.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace mongo::streams {
namespace {

// Programmable HTTP client. Captures every request body and returns a queued
// response. The default response is a 200 with one embedding per input parsed
// out of the request body, which is exactly what the success-path tests want.
class FakeEmbeddingHttpClient final : public HttpClient {
public:
    struct Response {
        std::uint16_t code = 200;
        std::string body;
    };

    void allowInsecureHTTP(bool) final {}
    void setHeaders(const std::vector<std::string>& headers) final {
        lastHeaders = headers;
    }
    HttpReply request(HttpMethod method, StringData url, ConstDataRange data) const final {
        ++callCount;
        capturedBodies.emplace_back(data.data(), data.length());
        capturedUrls.emplace_back(url);

        Response resp;
        if (!queued.empty()) {
            resp = std::move(queued.front());
            queued.erase(queued.begin());
        } else {
            resp = makeEchoEmbeddings(capturedBodies.back());
        }
        DataBuilder hdr;
        DataBuilder body;
        body.writeAndAdvance(ConstDataRange(resp.body.data(), resp.body.size()))
            .ignore();
        return HttpReply{resp.code, std::move(hdr), std::move(body)};
    }

    // Build a fake response: one 3-dim embedding per input, with components
    // derived from input length so tests can assert vector contents.
    static Response makeEchoEmbeddings(const std::string& requestBody) {
        BSONObj parsed = fromjson(requestBody);
        BSONArrayBuilder dataArr;
        for (auto el : parsed["input"].Array()) {
            int len = el.String().size();
            BSONObjBuilder item;
            BSONArrayBuilder emb(item.subarrayStart("embedding"));
            emb.append(static_cast<double>(len));
            emb.append(static_cast<double>(len) * 0.5);
            emb.append(static_cast<double>(len) * 0.25);
            emb.done();
            dataArr.append(item.obj());
        }
        BSONObjBuilder body;
        body.append("data", dataArr.arr());
        return {200, body.obj().jsonString()};
    }

    mutable std::atomic<int> callCount{0};
    mutable std::vector<std::string> capturedBodies;
    mutable std::vector<std::string> capturedUrls;
    mutable std::vector<Response> queued;
    std::vector<std::string> lastHeaders;
};

// Pulls an embedding array out of the `into` field of a forwarded document.
std::vector<double> getEmbedding(const Document& doc, StringData path = "embedding") {
    auto v = doc.getNestedField(FieldPath{path});
    std::vector<double> out;
    for (auto& e : v.getArray()) {
        out.push_back(e.coerceToDouble());
    }
    return out;
}

// Build a $embed operator with sensible defaults. Individual tests override
// only the fields they care about.
struct OpBuilder {
    OperatorTestFixture* fix;
    int maxBatchSize = 4;
    Milliseconds maxWait{50};
    bool cacheEnabled = false;
    int cacheMaxEntries = 100;
    EmbedErrorModeEnum onError = EmbedErrorModeEnum::kFail;
    boost::optional<int> dimensions;
    std::string into = "embedding";
    BSONObj inputExpr = BSON("$expr"
                             << "$body");

    std::unique_ptr<EmbedOperator> build(std::unique_ptr<HttpClient> http) {
        auto expr = Expression::parseOperand(
            fix->ctx()->expCtx().get(),
            inputExpr.firstElement(),
            fix->ctx()->expCtx()->variablesParseState);
        EmbeddingConnection conn{EmbeddingProvider::kOpenAi,
                                 "https://test.example/v1/embeddings",
                                 "fake-key",
                                 "",
                                 ""};
        auto op = std::make_unique<EmbedOperator>(fix->ctx(),
                                                  std::move(expr),
                                                  FieldPath{into},
                                                  std::move(conn),
                                                  "test-model",
                                                  dimensions,
                                                  maxBatchSize,
                                                  maxWait,
                                                  cacheEnabled,
                                                  cacheMaxEntries,
                                                  onError);
        op->setHttpClientForTest(std::move(http));
        fix->connect(op.get());
        return op;
    }
};

Message makeMsg(StringData body) {
    return Message{Document{BSON("body" << body)}};
}

class EmbedOperatorTest : public OperatorTestFixture {};

// Batch fills exactly: 4 docs with maxBatchSize=4 -> exactly one provider call,
// 4 docs forwarded, embeddings attached in input order.
TEST_F(EmbedOperatorTest, BatchFillFlushes) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    auto op = OpBuilder{this}.build(std::move(httpRaw));

    op->onMessage(makeMsg("aa"));     // len 2
    op->onMessage(makeMsg("bbb"));    // len 3
    op->onMessage(makeMsg("cccc"));   // len 4
    ASSERT_EQ(0, http->callCount.load()) << "should not flush before batch full";
    ASSERT_EQ(0u, output().size());

    op->onMessage(makeMsg("ddddd"));  // len 5 -- triggers flush
    ASSERT_EQ(1, http->callCount.load());
    ASSERT_EQ(4u, output().size());

    auto v0 = getEmbedding(output()[0].doc());
    auto v3 = getEmbedding(output()[3].doc());
    ASSERT_EQ(2.0, v0[0]);
    ASSERT_EQ(5.0, v3[0]);
}

// Time-based flush: batch is not full, but onTick fires after maxWait elapses.
TEST_F(EmbedOperatorTest, TimeBasedFlush) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    OpBuilder b{this};
    b.maxBatchSize = 100;
    b.maxWait = Milliseconds{10};
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("hello"));
    op->onMessage(makeMsg("world"));
    ASSERT_EQ(0, http->callCount.load());

    advanceClock(Milliseconds{20});
    op->onTick();
    ASSERT_EQ(1, http->callCount.load());
    ASSERT_EQ(2u, output().size());
}

// Cache hit on second sighting of the same input -- no provider call.
TEST_F(EmbedOperatorTest, CacheHitSkipsProvider) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    OpBuilder b{this};
    b.cacheEnabled = true;
    b.maxBatchSize = 1;  // flush every message so the cache populates immediately
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("hello"));
    ASSERT_EQ(1, http->callCount.load());
    ASSERT_EQ(1u, output().size());

    op->onMessage(makeMsg("hello"));  // identical input -> cache hit
    ASSERT_EQ(1, http->callCount.load()) << "second call should be served from cache";
    ASSERT_EQ(2u, output().size());

    op->onMessage(makeMsg("different"));  // miss
    ASSERT_EQ(2, http->callCount.load());
    ASSERT_EQ(3u, output().size());

    // All three docs got embeddings of the right shape.
    for (const auto& m : output()) {
        ASSERT_EQ(3u, getEmbedding(m.doc()).size());
    }
}

// LRU eviction: with maxEntries=2, the third unique input should evict the
// oldest, so re-asking for it triggers a provider call.
TEST_F(EmbedOperatorTest, CacheLruEviction) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    OpBuilder b{this};
    b.cacheEnabled = true;
    b.cacheMaxEntries = 2;
    b.maxBatchSize = 1;
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("a"));  // miss
    op->onMessage(makeMsg("b"));  // miss
    op->onMessage(makeMsg("c"));  // miss, evicts "a"
    ASSERT_EQ(3, http->callCount.load());

    op->onMessage(makeMsg("a"));  // miss again -- "a" was evicted
    ASSERT_EQ(4, http->callCount.load());

    op->onMessage(makeMsg("c"));  // hit
    ASSERT_EQ(4, http->callCount.load());
}

// 429 + 500 are retried; second attempt succeeds. Documents still forward.
TEST_F(EmbedOperatorTest, RetryOn429AndServerError) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    http->queued.push_back({429, R"({"error":"rate limit"})"});
    http->queued.push_back({502, R"({"error":"bad gateway"})"});
    // Third call falls through to the default echo response.

    OpBuilder b{this};
    b.maxBatchSize = 1;
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("hi"));
    ASSERT_EQ(3, http->callCount.load());
    ASSERT_EQ(1u, output().size());
}

// onError=fail: the second (terminal) HTTP error escapes as a DBException.
TEST_F(EmbedOperatorTest, OnErrorFailThrows) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    http->queued.push_back({400, R"({"error":"bad input"})"});  // non-retryable

    OpBuilder b{this};
    b.maxBatchSize = 1;
    b.onError = EmbedErrorModeEnum::kFail;
    auto op = b.build(std::move(httpRaw));

    ASSERT_THROWS_CODE(op->onMessage(makeMsg("hi")), DBException, ErrorCodes::OperationFailed);
}

// onError=dlq: the document is routed to the DLQ, downstream sees nothing.
TEST_F(EmbedOperatorTest, OnErrorDlqRoutesDocument) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    http->queued.push_back({400, R"({"error":"bad input"})"});

    OpBuilder b{this};
    b.maxBatchSize = 1;
    b.onError = EmbedErrorModeEnum::kDlq;
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("hi"));
    ASSERT_EQ(0u, output().size());
    ASSERT_EQ(1u, dlq().size());
    ASSERT_EQ("$embed", dlq()[0].source);
}

// onError=ignore: the document passes through with no `into` field set.
TEST_F(EmbedOperatorTest, OnErrorIgnorePassesThrough) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    http->queued.push_back({400, R"({"error":"bad input"})"});

    OpBuilder b{this};
    b.maxBatchSize = 1;
    b.onError = EmbedErrorModeEnum::kIgnore;
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("hi"));
    ASSERT_EQ(1u, output().size());
    ASSERT(output()[0].doc().getNestedField(FieldPath{"embedding"}).missing());
}

// Output order matches input order even when some docs are cache hits.
TEST_F(EmbedOperatorTest, OrderPreservedWithMixedCacheHits) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    OpBuilder b{this};
    b.cacheEnabled = true;
    b.maxBatchSize = 1;  // each cache miss triggers an immediate flush
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("apple"));     // miss
    op->onMessage(makeMsg("banana"));    // miss
    op->onMessage(makeMsg("apple"));     // hit
    op->onMessage(makeMsg("cherry"));    // miss

    ASSERT_EQ(4u, output().size());
    ASSERT_EQ("apple", output()[0].doc().getField("body").getString());
    ASSERT_EQ("banana", output()[1].doc().getField("body").getString());
    ASSERT_EQ("apple", output()[2].doc().getField("body").getString());
    ASSERT_EQ("cherry", output()[3].doc().getField("body").getString());
}

// A null-valued input expression forwards the doc with no embedding attached.
TEST_F(EmbedOperatorTest, NullInputPassesThrough) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    OpBuilder b{this};
    b.inputExpr = BSON("$expr" << BSONNULL);
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("hi"));
    ASSERT_EQ(0, http->callCount.load());
    ASSERT_EQ(1u, output().size());
    ASSERT(output()[0].doc().getNestedField(FieldPath{"embedding"}).missing());
}

// Checkpoint barrier flushes the pending batch before propagating.
TEST_F(EmbedOperatorTest, CheckpointFlushesPending) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    OpBuilder b{this};
    b.maxBatchSize = 100;
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("a"));
    op->onMessage(makeMsg("b"));
    ASSERT_EQ(0, http->callCount.load());

    op->onCheckpoint(CheckpointToken{1});
    ASSERT_EQ(1, http->callCount.load());
    ASSERT_EQ(2u, output().size());
    ASSERT_EQ(1u, checkpoints().size());
}

// Watermark flushes pending before propagating, preserving ordering.
TEST_F(EmbedOperatorTest, WatermarkFlushesPending) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    OpBuilder b{this};
    b.maxBatchSize = 100;
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("a"));
    op->onWatermark(Watermark{Date_t::fromMillisSinceEpoch(1000)});
    ASSERT_EQ(1, http->callCount.load());
    ASSERT_EQ(1u, output().size());
    ASSERT_EQ(1u, watermarks().size());
}

// Provider's "dimensions" parameter is forwarded in the request body.
TEST_F(EmbedOperatorTest, DimensionsForwardedInRequest) {
    auto httpRaw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http = httpRaw.get();
    OpBuilder b{this};
    b.maxBatchSize = 1;
    b.dimensions = 512;
    auto op = b.build(std::move(httpRaw));

    op->onMessage(makeMsg("hi"));
    BSONObj reqBody = fromjson(http->capturedBodies.back());
    ASSERT_EQ(512, reqBody["dimensions"].numberInt());
    ASSERT_EQ("test-model", reqBody["model"].String());
}

// Cache key includes dimensions: same input under different dim configs must
// not collide. (Two operators, sharing inputs but different dimensions, must
// each call the provider once.)
TEST_F(EmbedOperatorTest, CacheKeyIncludesDimensions) {
    auto http1Raw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http2Raw = std::make_unique<FakeEmbeddingHttpClient>();
    auto http1 = http1Raw.get();
    auto http2 = http2Raw.get();

    OpBuilder ba{this};
    ba.cacheEnabled = true;
    ba.maxBatchSize = 1;
    ba.dimensions = 128;
    auto opA = ba.build(std::move(http1Raw));

    OpBuilder bb{this};
    bb.cacheEnabled = true;
    bb.maxBatchSize = 1;
    bb.dimensions = 256;
    auto opB = bb.build(std::move(http2Raw));

    opA->onMessage(makeMsg("hello"));
    opA->onMessage(makeMsg("hello"));  // hit for opA's cache
    opB->onMessage(makeMsg("hello"));  // different op, separate cache; one call

    ASSERT_EQ(1, http1->callCount.load());
    ASSERT_EQ(1, http2->callCount.load());
}

}  // namespace
}  // namespace mongo::streams
