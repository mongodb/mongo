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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/host_portal.h"
#include "mongo/db/extension/sdk/log_util.h"
#include "mongo/db/extension/sdk/test_extension_util.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

static const std::string kNativeVectorSearchName = "$nativeVectorSearch";
static const std::string kMatchName = "$match";
static const std::string kSetMetadataName = "$setMetadata";
static const std::string kSortName = "$sort";
static const std::string kLimitName = "$limit";
static const std::string kMetricsStageName = "$vectorSearchMetrics";

static const std::string kCosineExpr = "$similarityCosine";
static const std::string kDotProdExpr = "$similarityDotProduct";
static const std::string kEuclidExpr = "$similarityEuclidean";

static const std::string kMetaVectorSearchScore = "vectorSearchScore";

/**
 * Implementation of operation metrics for vector search operations.
 * Tracks execution start time, number of documents examined, and the algorithm used.
 */
class OperationMetricsImpl : public sdk::OperationMetricsBase {
public:
    OperationMetricsImpl() = default;
    /**
     * Serialize current metrics as BSON.
     * Returns format: {start: <Date>, docsExamined: <int>, algorithm: <string>}.
     */
    BSONObj serialize() const override {
        return BSON("start" << _start << "docsExamined" << _counter << "algorithm" << _algorithm);
    }

    void update(MongoExtensionByteView arguments) override {
        // Convert ByteView to BSON object
        BSONObj updateObj = extension::bsonObjFromByteView(arguments);

        // Check which update type is present
        _start = updateObj.getField("start").date();
        _algorithm = updateObj.getField("algorithm").String();
        _counter++;
    }

private:
    Date_t _start;
    int _counter = 0;
    std::string _algorithm;
};

/**
 * Execution stage that collects metrics during vector search pipeline execution.
 *
 * On each getNext() call, updates the operation metrics with the current timestamp
 * and algorithm name, then passes through documents from the source stage unchanged.
 * The metrics are stored on the OperationContext and can be serialized for logging
 * and diagnostics.
 */
class MetricsExecAggStage : public sdk::ExecAggStageTransform {
public:
    static constexpr std::string kCounterField = "counter";

    explicit MetricsExecAggStage(const std::string& algorithm)
        : sdk::ExecAggStageTransform(kMetricsStageName), _algorithm(algorithm) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        MongoExtensionExecAggStage* execStage,
        ::MongoExtensionGetNextRequestType requestType) override {
        // Get metrics from the execution context (stored on OperationContext).
        auto metrics = execCtx.getMetrics(execStage);

        auto now = Date_t::now();
        BSONObjBuilder updateBuilder;
        updateBuilder.append("start", now);
        updateBuilder.append("algorithm", _algorithm);

        auto bson = updateBuilder.obj();
        auto updateBuf = mongo::extension::objAsByteView(bson);
        metrics.update(updateBuf);

        return _getSource().getNext(execCtx.get());
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    /**
     * Create metrics instance for this stage.
     * The instance will be stored on the OperationContext and accessed via execCtx.getMetrics().
     */
    std::unique_ptr<sdk::OperationMetricsBase> createMetrics() const override {
        return std::make_unique<OperationMetricsImpl>();
    }

private:
    std::string _algorithm;
};

class MetricsLogicalStage : public sdk::LogicalAggStage {
public:
    MetricsLogicalStage(const std::string& algorithm) : _algorithm(algorithm) {};

    BSONObj serialize() const override {
        return BSON(kMetricsStageName << BSONObj());
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(kMetricsStageName << BSONObj());
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<MetricsExecAggStage>(_algorithm);
    }

private:
    std::string _algorithm;
};

class MetricsAstNode : public sdk::AggStageAstNode {
public:
    MetricsAstNode(const std::string& algorithm)
        : sdk::AggStageAstNode(kMetricsStageName), _algorithm(algorithm) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<MetricsLogicalStage>(_algorithm);
    }

private:
    std::string _algorithm;
};

/**
 * Parse node for the $vectorSearchMetrics stage that expands during desugaring.
 *
 * This node expands to a single MetricsAstNode. It is inserted into the pipeline
 * during expansion of $nativeVectorSearch to track metrics for the vector search
 * operation. The algorithm parameter is passed through from the parent stage.
 */
class MetricsParseNode : public sdk::AggStageParseNode {
public:
    MetricsParseNode(const std::string& algorithm)
        : sdk::AggStageParseNode(kMetricsStageName), _algorithm(algorithm) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<MetricsAstNode>(_algorithm)));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

private:
    std::string _algorithm;
};

/**
 * Even though users don't use $vectorSearchMetrics, we must register stage descriptor for the
 * sharded case, where the mongos serializes the pipeline and sends it to the shards.
 */
class MetricsStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = "$vectorSearchMetrics";

    MetricsStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName);
        const auto raw = stageBson[kMetricsStageName].Obj();
        const auto algorithm = raw.hasField("metric") ? raw["metric"].checkAndGetStringData() : "";
        return std::make_unique<MetricsParseNode>(std::string(algorithm));
    }
};

class NativeVectorSearchParseNode : public sdk::AggStageParseNode {
public:
    /**
     * Construct with already-validated inputs from the descriptor.
     */
    NativeVectorSearchParseNode(std::string path,
                                BSONObj queryVectorArrOwned,
                                std::string metric,
                                int64_t limit,
                                bool normalizeScore,
                                BSONObj filter,
                                std::optional<int> numCandidates)
        : sdk::AggStageParseNode(kNativeVectorSearchName),
          _path(std::move(path)),
          _queryVector(std::move(queryVectorArrOwned)),
          _metric(std::move(metric)),
          _limit(limit),
          _normalizeScore(normalizeScore),
          _filter(std::move(filter)),
          _numCandidates(numCandidates) {}

    /**
     * Return the number of expanded pipeline stages this node produces:
     * $match? + $setMetadata + $sort + $limit + 1 metric stage
     */
    size_t getExpandedSize() const override {
        return (_filter.isEmpty() ? 0 : 1) + 4;
    }

    /**
     * Expand into host stages:
     *   1) $match (if 'filter' is non-empty)
     *   2) $setMetadata { vectorSearchScore: <similarityExpr> }
     *        - vectors = [ <probe-as-doubles>, "$<path>" ]
     *        - if metric == "euclidean" && !normalizeScore:
     *            negate distance so larger score sorts higher (descending)
     *   3) $sort { vectorSearchScore: { $meta: "vectorSearchScore" } }  // descending
     *   4) $limit <limit>
     */
    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> out;
        out.reserve(getExpandedSize());

        auto* host = sdk::HostServicesHandle::getHostServices();

        auto metrics = std::make_unique<MetricsAstNode>(_metric);
        out.emplace_back(new sdk::ExtensionAggStageAstNode(std::move(metrics)));

        if (!_filter.isEmpty()) {
            out.emplace_back(host->createHostAggStageParseNode(_buildMatch()));
        }
        out.emplace_back(host->createHostAggStageParseNode(_buildSetMetadata()));
        out.emplace_back(host->createHostAggStageParseNode(_buildSortUsingMeta()));
        out.emplace_back(host->createHostAggStageParseNode(_buildLimit()));

        return out;
    }

    /**
     * Emit the canonical query shape used for plan-cache hashing.
     */
    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        BSONObjBuilder bob;
        bob.append("path", _path);
        {
            // We canonicalize elements to double for stable query shape hashing.
            BSONArrayBuilder arr(bob.subarrayStart("queryVector"));
            for (auto&& el : _queryVector) {
                arr.append(el.numberDouble());
            }
            arr.done();
        }
        bob.append("limit", _limit);
        bob.append("metric", _metric);
        bob.append("normalizeScore", _normalizeScore);
        if (!_filter.isEmpty()) {
            bob.append("filter", _filter);
        }
        if (_numCandidates) {
            bob.append("numCandidates", *_numCandidates);
        }
        return BSON(kNativeVectorSearchName << bob.obj());
    }

private:
    // {$match: {...}}
    BSONObj _buildMatch() const {
        return BSON(kMatchName << _filter);
    }

    // {$setMetadata {vectorSearchScore: <expr> }}
    BSONObj _buildSetMetadata() const {
        // vectors: [ <probe>, "$<path>" ]
        BSONArrayBuilder vectors;
        {
            BSONArrayBuilder probe(vectors.subarrayStart());
            for (auto&& el : _queryVector) {
                probe.append(el.numberDouble());
            }
            probe.done();
        }
        vectors.append("$" + _path);

        const auto& opName = (_metric == "cosine")
            ? kCosineExpr
            : (_metric == "dotProduct" ? kDotProdExpr : kEuclidExpr);

        // If normalized is true, use {vectors, score:true}; else concise array form.
        auto baseExpr = _normalizeScore
            ? BSON(opName << BSON("vectors" << vectors.arr() << "score" << true))
            : BSON(opName << vectors.arr());

        // For raw Euclidean, negate distance (a smaller absolute score indicates greater
        // similarity, and the $meta sort is descending).
        auto scoreExpr = baseExpr;
        if (_metric == "euclidean" && !_normalizeScore) {
            scoreExpr = BSON("$multiply" << BSON_ARRAY(-1 << baseExpr));
        }

        return BSON(kSetMetadataName << BSON(kMetaVectorSearchScore << scoreExpr));
    }

    // {$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}} (descending)
    BSONObj _buildSortUsingMeta() const {
        return BSON(
            kSortName << BSON(kMetaVectorSearchScore << BSON("$meta" << kMetaVectorSearchScore)));
    }

    // {$limit: <int>}
    BSONObj _buildLimit() const {
        return BSON(kLimitName << _limit);
    }

private:
    // Parsed fields
    const std::string _path;
    const BSONObj _queryVector;
    const std::string _metric;
    const int _limit = 0;
    const bool _normalizeScore = false;
    const BSONObj _filter;
    const std::optional<int> _numCandidates;
};

class NativeVectorSearchStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = "$nativeVectorSearch";

    NativeVectorSearchStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    /**
     * Parses and validates the user-facing $nativeVectorSearch specification.
     *
     * Expected input format:
     *   {$nativeVectorSearch: {
     *      path: <string>,                  // required; field path to embedding (e.g. "embedding")
     *      queryVector: [<number>...],      // required; probe vector
     *      limit: <int>,                    // required; positive integer (>= 1)
     *      metric: "cosine" | "dotProduct" | "euclidean", // required
     *      normalizeScore: <bool>,          // optional; default = false
     *      filter: <object>,                // optional; becomes $match if provided
     *      numCandidates: <int>             // optional; reserved for future use
     *   }}
     */
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName);
        const auto raw = stageBson[kStageName].Obj();

        sdk::sdk_log("Parsing a native vector search extension stage", 11407600);

        // Check presence and type of required fields.
        sdk_uassert(10956503,
                    "$nativeVectorSearch requires 'path' string",
                    raw.hasField("path") && raw["path"].type() == BSONType::string);
        const auto path = raw["path"].String();

        sdk_uassert(10956504,
                    "$nativeVectorSearch requires 'queryVector' array",
                    raw.hasField("queryVector") && raw["queryVector"].type() == BSONType::array);
        const auto queryVectorArrOwned = raw["queryVector"].Obj().getOwned();
        size_t qvCount = 0;
        for (auto&& el : queryVectorArrOwned) {
            sdk_uassert(10956505, "'queryVector' must contain only numeric values", el.isNumber());
            ++qvCount;
        }
        sdk_uassert(10956506, "'queryVector' must not be empty", qvCount > 0);

        sdk_uassert(10956507,
                    "$nativeVectorSearch requires 'metric' string that must be one of {'cosine', "
                    "'dotProduct', 'euclidean'}",
                    raw.hasField("metric") && raw["metric"].type() == BSONType::string);
        const auto metric = raw["metric"].String();
        sdk_uassert(10956508,
                    "invalid metric: expected 'cosine', 'dotProduct', or 'euclidean'",
                    metric == "cosine" || metric == "dotProduct" || metric == "euclidean");

        sdk_uassert(10956509, "$nativeVectorSearch requires 'limit' field", raw.hasField("limit"));
        const auto swLimitLong = raw["limit"].parseIntegerElementToNonNegativeLong();
        sdk_uassert(10956510,
                    "'limit' must be a positive integer",
                    swLimitLong.isOK() && swLimitLong.getValue() > 0);

        // Check presence and type of optional fields.
        auto normalizeScore = false;
        if (raw.hasField("normalizeScore")) {
            sdk_uassert(10956511,
                        "'normalizeScore' must be boolean",
                        raw["normalizeScore"].type() == BSONType::boolean);
            normalizeScore = raw["normalizeScore"].Bool();
        }

        BSONObj filter;
        if (raw.hasField("filter")) {
            sdk_uassert(
                10956512, "'filter' must be an object", raw["filter"].type() == BSONType::object);
            filter = raw["filter"].Obj().getOwned();
        }

        std::optional<int> numCandidates;
        if (raw.hasField("numCandidates")) {
            sdk_uassert(
                10956513, "'numCandidates' must be a number", raw["numCandidates"].isNumber());
            numCandidates = raw["numCandidates"].numberInt();
            sdk_uassert(10956514, "'numCandidates' must be >= 1", *numCandidates >= 1);
        }

        return std::make_unique<NativeVectorSearchParseNode>(std::move(path),
                                                             std::move(queryVectorArrOwned),
                                                             std::move(metric),
                                                             swLimitLong.getValue(),
                                                             normalizeScore,
                                                             std::move(filter),
                                                             numCandidates);
    }
};

class NativeVectorSearchExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<MetricsStageDescriptor>(portal);
        _registerStage<NativeVectorSearchStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(NativeVectorSearchExtension)
DEFINE_GET_EXTENSION()
