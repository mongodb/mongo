/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 */

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/modules/enterprise/src/streams/exec/context.h"
#include "mongo/db/modules/enterprise/src/streams/exec/message.h"
#include "mongo/db/modules/enterprise/src/streams/exec/operator.h"
#include "mongo/db/modules/enterprise/src/streams/exec/stages_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/timer.h"

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>

#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mongo::streams {

/**
 * Provider type drives endpoint shape, request body, and response parsing.
 */
enum class EmbeddingProvider {
    kVoyage,
    kOpenAi,
    kBedrock,
    kAzureOpenAi,
    kVertexAi,
};

/**
 * Connection Registry record resolved at plan time. Holds endpoint + credentials so
 * the operator can issue authenticated requests without API keys ever appearing
 * inline in the pipeline spec.
 */
struct EmbeddingConnection {
    EmbeddingProvider provider;
    std::string endpoint;
    std::string apiKey;
    std::string region;  // bedrock / vertex
    std::string deployment;  // azureOpenai
};

/**
 * $embed: a pure enrichment stage that adds vector embeddings to streaming documents
 * via a remote embedding provider. Documents are accumulated into a micro-batch and
 * flushed when either maxSize or maxWaitMs is reached; an optional content-addressed
 * LRU cache short-circuits identical inputs.
 *
 * Embedding is deterministic for a given (input, model, dimensions), so re-processing
 * after checkpoint recovery is idempotent — no extra exactly-once machinery required.
 */
class EmbedOperator final : public Operator {
public:
    EmbedOperator(Context* ctx,
                  boost::intrusive_ptr<Expression> inputExpr,
                  FieldPath intoField,
                  EmbeddingConnection conn,
                  std::string modelName,
                  boost::optional<int> dimensions,
                  int maxBatchSize,
                  Milliseconds maxWait,
                  bool cacheEnabled,
                  int cacheMaxEntries,
                  EmbedErrorModeEnum onError);

    ~EmbedOperator() override;

    StringData kind() const override {
        return "embed"_sd;
    }

    // Operator overrides.
    void onMessage(Message msg) override;
    void onWatermark(Watermark wm) override;
    void onCheckpoint(CheckpointToken token) override;
    void onTick() override;
    void close() override;

    // Test seam: lets unit tests swap in a fake HttpClient without going to the network.
    void setHttpClientForTest(std::unique_ptr<HttpClient> client);

private:
    struct PendingDoc {
        Message msg;        // input document, kept until embedding is attached
        std::string text;   // resolved input string
        std::string cacheKey;
        bool cacheKeyValid;
    };

    // Cache entry: vector + raw key for LRU bookkeeping.
    struct CacheEntry {
        std::vector<double> vec;
        std::list<std::string>::iterator lruIt;
    };

    // Evaluate the input expression on `msg` and coerce to string. Returns boost::none
    // if the expression produces missing/null — those docs are forwarded with the
    // `into` field left absent regardless of onError mode (matches $project semantics).
    boost::optional<std::string> evaluateInput(const Message& msg) const;

    // Hash(input, model.name, dimensions) — stable across process restarts within a
    // model version, so cache is content-addressed.
    std::string makeCacheKey(StringData inputText) const;

    // Look up in cache and, if hit, attach vector to msg & forward. Returns true on hit.
    bool tryServeFromCache(Message& msg, const std::string& cacheKey);

    // Insert (or move-to-front) a cache entry, evicting LRU tail if over capacity.
    void cacheInsert(std::string key, std::vector<double> vec);

    // Add a doc to the pending batch and possibly flush.
    void enqueue(PendingDoc doc);

    // Flush the current pending batch: build provider request, call HTTP, attach
    // vectors in input order, then forward downstream. Always clears the batch on
    // return (including on error paths) so the operator can make progress.
    void flushBatch();

    // Provider-specific request body + URL.
    std::pair<std::string, std::string> buildRequest(const std::vector<StringData>& inputs) const;

    // Provider-specific response parser. Returns one vector per input, in order.
    std::vector<std::vector<double>> parseResponse(const BSONObj& body) const;

    // Single HTTP attempt; throws on non-2xx or parse failure. The HTTP status is
    // surfaced via the thrown exception so the retry loop can distinguish 429 / 5xx
    // (retryable) from 4xx (terminal).
    std::vector<std::vector<double>> callProvider(const std::vector<StringData>& inputs);

    // Drives the retry loop with exponential backoff for 429 / transient network /
    // 5xx. Non-retryable failures go straight to applyError.
    std::vector<std::vector<double>> callProviderWithRetry(
        const std::vector<StringData>& inputs);

    // Apply the configured onError policy to a single doc.
    void applyError(Message msg, const Status& reason);

    // Attach vector at `_intoField` and emit downstream.
    void emitWithEmbedding(Message msg, const std::vector<double>& vec);

    Context* _ctx;
    boost::intrusive_ptr<Expression> _inputExpr;
    FieldPath _intoField;

    EmbeddingConnection _conn;
    std::string _modelName;
    boost::optional<int> _dimensions;

    int _maxBatchSize;
    Milliseconds _maxWait;
    EmbedErrorModeEnum _onError;

    // Cache state. `_lru.front()` is most-recently-used.
    bool _cacheEnabled;
    size_t _cacheMaxEntries;
    std::unordered_map<std::string, CacheEntry> _cache;
    std::list<std::string> _lru;

    // Pending batch state.
    std::deque<PendingDoc> _pending;
    Timer _sinceFirstQueued;  // resets each time the batch transitions from empty.
    bool _hasPending = false;

    std::unique_ptr<HttpClient> _http;

    // Retry tuning. Exposed as private constants for now; promote to feature flags
    // if operational tuning becomes a need.
    static constexpr int kMaxRetries = 4;
    static constexpr Milliseconds kInitialBackoff{200};
};

}  // namespace mongo::streams
