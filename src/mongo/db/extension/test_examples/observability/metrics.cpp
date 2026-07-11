// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

#include <memory>
#include <string_view>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Implementation of OperationMetricsBase that tracks operation metrics.
 * This instance is stored on the OperationContext and accessed via QueryExecutionContextHandle.
 * Aggregates the sum of all fields inside of the $counter field in each document.
 */
class OperationMetricsImpl : public sdk::OperationMetricsBase {
public:
    OperationMetricsImpl() = default;

    /**
     * Serialize current metrics as BSON.
     * Returns format: {$metrics: {counter: N}}.
     */
    BSONObj serialize() const override {
        return BSON("counter" << _counter);
    }

    /**
     * Update metrics from binary data.
     * Expected format: one int64 value representing the counter delta.
     */
    void update(MongoExtensionByteView arguments) override {
        const int64_t* delta = reinterpret_cast<const int64_t*>(arguments.data);
        _counter += *delta;
    }

private:
    int64_t _counter{0};
};

class MetricsExecStage : public sdk::TestExecStage {
public:
    static constexpr std::string kCounterField = "counter";

    explicit MetricsExecStage(std::string_view stageName, mongo::BSONObj arguments)
        : sdk::TestExecStage(stageName, arguments) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        // Get metrics from the execution context (stored on OperationContext).
        auto metrics = execCtx->getMetrics(execStage);

        auto input = _getSource()->getNext(execCtx.get());
        if (input.code == mongo::extension::GetNextCode::kPauseExecution) {
            return mongo::extension::ExtensionGetNextResult::pauseExecution();
        }
        if (input.code == mongo::extension::GetNextCode::kEOF) {
            return mongo::extension::ExtensionGetNextResult::eof();
        }

        auto doc = input.resultDocument->getUnownedBSONObj();
        if (doc.hasField(kCounterField)) {
            int64_t counterDelta = doc[kCounterField].numberInt();
            metrics->update(MongoExtensionByteView{reinterpret_cast<const uint8_t*>(&counterDelta),
                                                   sizeof(int64_t)});
        }

        return mongo::extension::ExtensionGetNextResult::advanced(
            mongo::extension::ExtensionBSONObj::makeAsByteBuf(doc));
    }

    /**
     * Create metrics instance for this stage.
     * The instance will be stored on the OperationContext and accessed via execCtx.getMetrics().
     */
    std::unique_ptr<sdk::OperationMetricsBase> createMetrics() const override {
        return std::make_unique<OperationMetricsImpl>();
    }
};

DEFAULT_LOGICAL_STAGE(Metrics);
DEFAULT_AST_NODE(Metrics);
DEFAULT_PARSE_NODE(Metrics);

/**
 * $metrics aggregation stage that collects and reports operation metrics.
 *
 * Syntax: {$metrics: {}}
 */
using MetricsStageDescriptor =
    sdk::TestStageDescriptor<"$metrics", MetricsParseNode, true /* ExpectEmptyStageDefinition */>;

DEFAULT_EXTENSION(Metrics)
REGISTER_EXTENSION(MetricsExtension)
DEFINE_GET_EXTENSION()
