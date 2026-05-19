/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 */

#include "mongo/db/modules/enterprise/src/streams/exec/embed/embed_operator.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/modules/enterprise/src/streams/exec/context.h"
#include "mongo/db/modules/enterprise/src/streams/exec/dead_letter_queue.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace mongo::streams {

namespace {

// One process-wide HTTP client factory. We use the firewall-aware variant because
// the embedding provider endpoint is user-controlled (via connection registry) and
// must respect the same egress rules ASP applies to $https.
std::unique_ptr<HttpClient> defaultHttpClient(Context* ctx) {
    return HttpClient::createWithFirewall(ctx->egressDenyList());
}

// FNV-1a over (input || modelName || dimensions). Hex-encoded to make the key
// suitable as an unordered_map key without any further escaping. Cheap and stable
// across process restarts — perfect for content-addressed cache.
std::string fnv1aHex(StringData a, StringData b, StringData c) {
    constexpr uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr uint64_t kPrime = 0x100000001b3ULL;
    uint64_t h = kOffset;
    auto mix = [&](StringData s) {
        for (char ch : s) {
            h ^= static_cast<unsigned char>(ch);
            h *= kPrime;
        }
        h ^= 0xff;  // field separator: prevents ("ab","c") colliding with ("a","bc")
        h *= kPrime;
    };
    mix(a);
    mix(b);
    mix(c);
    static const char* kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[i] = kHex[h & 0xf];
        h >>= 4;
    }
    return out;
}

}  // namespace

EmbedOperator::EmbedOperator(Context* ctx,
                             boost::intrusive_ptr<Expression> inputExpr,
                             FieldPath intoField,
                             EmbeddingConnection conn,
                             std::string modelName,
                             boost::optional<int> dimensions,
                             int maxBatchSize,
                             Milliseconds maxWait,
                             bool cacheEnabled,
                             int cacheMaxEntries,
                             EmbedErrorModeEnum onError)
    : _ctx(ctx),
      _inputExpr(std::move(inputExpr)),
      _intoField(std::move(intoField)),
      _conn(std::move(conn)),
      _modelName(std::move(modelName)),
      _dimensions(dimensions),
      _maxBatchSize(maxBatchSize),
      _maxWait(maxWait),
      _onError(onError),
      _cacheEnabled(cacheEnabled),
      _cacheMaxEntries(cacheMaxEntries > 0 ? static_cast<size_t>(cacheMaxEntries) : 0),
      _http(defaultHttpClient(ctx)) {
    uassert(ErrorCodes::BadValue, "$embed batch.maxSize must be >= 1", _maxBatchSize >= 1);
    uassert(ErrorCodes::BadValue,
            "$embed batch.maxWaitMs must be >= 0",
            _maxWait >= Milliseconds(0));
    if (_cacheEnabled) {
        uassert(ErrorCodes::BadValue,
                "$embed cache.maxEntries must be >= 1 when cache is enabled",
                _cacheMaxEntries >= 1);
    }
}

EmbedOperator::~EmbedOperator() = default;

void EmbedOperator::setHttpClientForTest(std::unique_ptr<HttpClient> client) {
    _http = std::move(client);
}

void EmbedOperator::onMessage(Message msg) {
    auto text = evaluateInput(msg);
    if (!text) {
        // Missing/null input: forward unchanged. Same shape as $project on a missing
        // field — the `into` field simply stays absent.
        forward(std::move(msg));
        return;
    }

    PendingDoc doc;
    doc.text = std::move(*text);

    if (_cacheEnabled) {
        doc.cacheKey = makeCacheKey(doc.text);
        doc.cacheKeyValid = true;
        if (tryServeFromCache(msg, doc.cacheKey)) {
            return;
        }
    } else {
        doc.cacheKeyValid = false;
    }

    doc.msg = std::move(msg);
    enqueue(std::move(doc));
}

void EmbedOperator::onWatermark(Watermark wm) {
    // Watermarks must traverse the operator in order. Flush any pending batch
    // before forwarding so downstream sees the watermark only after all docs that
    // preceded it have been enriched.
    if (_hasPending) {
        flushBatch();
    }
    forwardWatermark(wm);
}

void EmbedOperator::onCheckpoint(CheckpointToken token) {
    // Drain in-flight work before the checkpoint barrier so recovery doesn't have
    // to replay docs whose input expression already evaluated.
    if (_hasPending) {
        flushBatch();
    }
    forwardCheckpoint(token);
}

void EmbedOperator::onTick() {
    if (!_hasPending) {
        return;
    }
    if (Milliseconds(_sinceFirstQueued.millis()) >= _maxWait) {
        flushBatch();
    }
}

void EmbedOperator::close() {
    if (_hasPending) {
        flushBatch();
    }
}

boost::optional<std::string> EmbedOperator::evaluateInput(const Message& msg) const {
    auto val = _inputExpr->evaluate(msg.doc(), &_ctx->expCtx()->variables);
    if (val.nullish()) {
        return boost::none;
    }
    // Provider APIs are universally string-typed for embedding input. Coerce
    // numeric/bool conservatively rather than uasserting — the user wrote an
    // expression and probably expects it to produce *something*.
    if (val.getType() == BSONType::String) {
        return std::string{val.getStringData()};
    }
    return val.coerceToString();
}

std::string EmbedOperator::makeCacheKey(StringData inputText) const {
    std::string dimStr = _dimensions ? std::to_string(*_dimensions) : "";
    return fnv1aHex(inputText, _modelName, dimStr);
}

bool EmbedOperator::tryServeFromCache(Message& msg, const std::string& cacheKey) {
    auto it = _cache.find(cacheKey);
    if (it == _cache.end()) {
        return false;
    }
    // Move the key to LRU front.
    _lru.erase(it->second.lruIt);
    _lru.push_front(cacheKey);
    it->second.lruIt = _lru.begin();
    emitWithEmbedding(std::move(msg), it->second.vec);
    return true;
}

void EmbedOperator::cacheInsert(std::string key, std::vector<double> vec) {
    if (!_cacheEnabled) {
        return;
    }
    auto existing = _cache.find(key);
    if (existing != _cache.end()) {
        _lru.erase(existing->second.lruIt);
        _lru.push_front(key);
        existing->second.lruIt = _lru.begin();
        existing->second.vec = std::move(vec);
        return;
    }
    _lru.push_front(key);
    CacheEntry entry{std::move(vec), _lru.begin()};
    _cache.emplace(std::move(key), std::move(entry));
    while (_cache.size() > _cacheMaxEntries) {
        const auto& evictKey = _lru.back();
        _cache.erase(evictKey);
        _lru.pop_back();
    }
}

void EmbedOperator::enqueue(PendingDoc doc) {
    if (!_hasPending) {
        _sinceFirstQueued.reset();
        _hasPending = true;
    }
    _pending.push_back(std::move(doc));
    if (static_cast<int>(_pending.size()) >= _maxBatchSize) {
        flushBatch();
    }
}

void EmbedOperator::flushBatch() {
    // Drain the queue into a local first: flushBatch must be reentrant-safe with
    // respect to its own error handling paths (applyError -> DLQ -> downstream).
    std::deque<PendingDoc> batch;
    batch.swap(_pending);
    _hasPending = false;

    if (batch.empty()) {
        return;
    }

    std::vector<StringData> inputs;
    inputs.reserve(batch.size());
    for (const auto& d : batch) {
        inputs.push_back(d.text);
    }

    std::vector<std::vector<double>> vectors;
    try {
        vectors = callProviderWithRetry(inputs);
    } catch (const DBException& ex) {
        // Whole-batch failure: each doc is treated according to onError. This is
        // the right granularity — partial-batch retries against the provider would
        // re-order documents, breaking the "output order matches input order"
        // contract documented in the design.
        for (auto& d : batch) {
            applyError(std::move(d.msg), ex.toStatus());
        }
        return;
    }

    tassert(ErrorCodes::InternalError,
            str::stream() << "$embed provider returned " << vectors.size()
                          << " vectors for " << batch.size() << " inputs",
            vectors.size() == batch.size());

    for (size_t i = 0; i < batch.size(); ++i) {
        auto& doc = batch[i];
        if (_cacheEnabled && doc.cacheKeyValid) {
            cacheInsert(doc.cacheKey, vectors[i]);
        }
        emitWithEmbedding(std::move(doc.msg), vectors[i]);
    }
}

std::pair<std::string, std::string> EmbedOperator::buildRequest(
    const std::vector<StringData>& inputs) const {
    BSONObjBuilder bob;
    BSONArrayBuilder arr(bob.subarrayStart("input"));
    for (auto in : inputs) {
        arr.append(in);
    }
    arr.done();
    bob.append("model", _modelName);
    if (_dimensions) {
        bob.append("dimensions", *_dimensions);
    }

    std::string url;
    switch (_conn.provider) {
        case EmbeddingProvider::kVoyage:
            url = _conn.endpoint.empty() ? "https://api.voyageai.com/v1/embeddings"
                                         : _conn.endpoint;
            break;
        case EmbeddingProvider::kOpenAi:
            url = _conn.endpoint.empty() ? "https://api.openai.com/v1/embeddings"
                                         : _conn.endpoint;
            break;
        case EmbeddingProvider::kAzureOpenAi:
            // Azure path embeds deployment name + api-version query param.
            uassert(ErrorCodes::BadValue,
                    "azureOpenai connection requires endpoint and deployment",
                    !_conn.endpoint.empty() && !_conn.deployment.empty());
            url = str::stream() << _conn.endpoint << "/openai/deployments/" << _conn.deployment
                                << "/embeddings?api-version=2024-02-01";
            break;
        case EmbeddingProvider::kBedrock:
        case EmbeddingProvider::kVertexAi:
            // Bedrock + Vertex use AWS SigV4 / GCP-IAM signing respectively. They
            // hook in via the existing AWS/GCP credential pattern and bypass the
            // generic Authorization header path. The signing layer is shared with
            // $https and is not duplicated here.
            uassert(ErrorCodes::BadValue,
                    "bedrock/vertex endpoint must be set in the connection",
                    !_conn.endpoint.empty());
            url = _conn.endpoint;
            break;
    }

    return {bob.obj().jsonString(), std::move(url)};
}

std::vector<std::vector<double>> EmbedOperator::parseResponse(const BSONObj& body) const {
    // OpenAI / Voyage / AzureOpenAI all return `{ data: [ { embedding: [...] }, ... ] }`.
    // Bedrock / Vertex have provider-specific shapes that we normalize before
    // arriving here in production; for the cross-provider implementation in this
    // file we accept the OpenAI-compatible shape only and rely on a thin adapter
    // (out of scope for this stage) for the others.
    auto dataElt = body["data"];
    uassert(ErrorCodes::FailedToParse,
            "embedding response missing `data` array",
            dataElt.type() == BSONType::Array);

    std::vector<std::vector<double>> result;
    for (const auto& el : dataElt.Array()) {
        auto embeddingElt = el["embedding"];
        uassert(ErrorCodes::FailedToParse,
                "embedding response item missing `embedding` array",
                embeddingElt.type() == BSONType::Array);
        std::vector<double> vec;
        for (const auto& v : embeddingElt.Array()) {
            vec.push_back(v.numberDouble());
        }
        result.push_back(std::move(vec));
    }
    return result;
}

std::vector<std::vector<double>> EmbedOperator::callProvider(
    const std::vector<StringData>& inputs) {
    auto [body, url] = buildRequest(inputs);

    std::vector<std::string> headers{"Content-Type: application/json"};
    if (!_conn.apiKey.empty()) {
        // Voyage + OpenAI use `Authorization: Bearer`. Azure uses `api-key:`.
        // Bedrock/Vertex are signed elsewhere — apiKey is unset for those.
        if (_conn.provider == EmbeddingProvider::kAzureOpenAi) {
            headers.push_back(str::stream() << "api-key: " << _conn.apiKey);
        } else {
            headers.push_back(str::stream() << "Authorization: Bearer " << _conn.apiKey);
        }
    }
    _http->setHeaders(headers);

    auto reply = _http->request(HttpClient::HttpMethod::kPOST,
                                url,
                                ConstDataRange(body.data(), body.size()));

    auto bodyCursor = reply.body.getCursor();
    StringData bodyStr{bodyCursor.data(), bodyCursor.length()};

    if (reply.code != 200) {
        // The retry layer reads ex.code() to decide retryability. We pick error
        // codes deliberately: 4xx (other than 429) is non-retryable; the rest get
        // retried.
        auto code = (reply.code == 429 || reply.code >= 500)
            ? ErrorCodes::HostUnreachable
            : ErrorCodes::OperationFailed;
        uasserted(code,
                  str::stream() << "embedding provider returned HTTP " << reply.code << ": "
                                << bodyStr.substr(0, std::min<size_t>(bodyStr.size(), 512)));
    }

    return parseResponse(fromjson(bodyStr));
}

std::vector<std::vector<double>> EmbedOperator::callProviderWithRetry(
    const std::vector<StringData>& inputs) {
    Milliseconds backoff = kInitialBackoff;
    for (int attempt = 0;; ++attempt) {
        try {
            return callProvider(inputs);
        } catch (const DBException& ex) {
            bool retryable = (ex.code() == ErrorCodes::HostUnreachable ||
                              ex.code() == ErrorCodes::NetworkTimeout ||
                              ex.code() == ErrorCodes::SocketException);
            if (!retryable || attempt >= kMaxRetries) {
                throw;
            }
            // Sleep using the operator's scheduler so the engine can interrupt us
            // during shutdown. sleepForRetry is provided by the streams runtime
            // and respects the operator's interrupt token.
            _ctx->sleepForRetry(backoff);
            backoff *= 2;
        }
    }
}

void EmbedOperator::applyError(Message msg, const Status& reason) {
    switch (_onError) {
        case EmbedErrorModeEnum::kFail:
            uassertStatusOK(reason.withContext("$embed failed and onError=fail"));
            return;
        case EmbedErrorModeEnum::kDlq:
            _ctx->dlq()->push(std::move(msg),
                              DlqRecord{"$embed", reason.code(), reason.reason()});
            return;
        case EmbedErrorModeEnum::kIgnore:
            // Pass the document through with the into field absent.
            forward(std::move(msg));
            return;
    }
    MONGO_UNREACHABLE;
}

void EmbedOperator::emitWithEmbedding(Message msg, const std::vector<double>& vec) {
    MutableDocument out{msg.doc()};
    std::vector<Value> arr;
    arr.reserve(vec.size());
    for (double d : vec) {
        arr.emplace_back(d);
    }
    out.setNestedField(_intoField, Value{std::move(arr)});
    msg.setDoc(out.freeze());
    forward(std::move(msg));
}

}  // namespace mongo::streams
