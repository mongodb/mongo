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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_util.h"

#include <memory>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

static constexpr std::string kMetricsStageName = "$metrics";

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

class MetricsExecAggStage : public sdk::ExecAggStageTransform {
public:
    static constexpr std::string kCounterField = "counter";

    explicit MetricsExecAggStage() : sdk::ExecAggStageTransform(kMetricsStageName) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        // Get metrics from the execution context (stored on OperationContext).
        auto metrics = execCtx.getMetrics(execStage);

        auto input = _getSource().getNext(execCtx.get());
        if (input.code == mongo::extension::GetNextCode::kPauseExecution) {
            return mongo::extension::ExtensionGetNextResult::pauseExecution();
        }
        if (input.code == mongo::extension::GetNextCode::kEOF) {
            return mongo::extension::ExtensionGetNextResult::eof();
        }

        auto doc = input.resultDocument->getUnownedBSONObj();
        if (doc.hasField(kCounterField)) {
            int64_t counterDelta = doc[kCounterField].numberInt();
            metrics.update(MongoExtensionByteView{reinterpret_cast<const uint8_t*>(&counterDelta),
                                                  sizeof(int64_t)});
        }

        return mongo::extension::ExtensionGetNextResult::advanced(
            mongo::extension::ExtensionBSONObj::makeAsByteBuf(doc));
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    /**
     * Create metrics instance for this stage.
     * The instance will be stored on the OperationContext and accessed via execCtx.getMetrics().
     */
    std::unique_ptr<sdk::OperationMetricsBase> createMetrics() const override {
        return std::make_unique<OperationMetricsImpl>();
    }
};

class MetricsLogicalStage : public sdk::LogicalAggStage {
public:
    MetricsLogicalStage() = default;

    BSONObj serialize() const override {
        return BSON(kMetricsStageName << BSONObj());
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(kMetricsStageName << BSONObj());
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<MetricsExecAggStage>();
    }

    std::unique_ptr<sdk::DistributedPlanLogicBase> getDistributedPlanLogic() const override {
        return nullptr;
    }
};

class MetricsAstNode : public sdk::AggStageAstNode {
public:
    MetricsAstNode() : sdk::AggStageAstNode(kMetricsStageName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<MetricsLogicalStage>();
    }
};

class MetricsParseNode : public sdk::AggStageParseNode {
public:
    MetricsParseNode() : sdk::AggStageParseNode(kMetricsStageName) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<MetricsAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }
};

/**
 * $metrics aggregation stage that collects and reports operation metrics.
 *
 * Syntax: {$metrics: {}}
 */
class MetricsStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = "$metrics";

    MetricsStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName, true /* checkEmpty */);

        return std::make_unique<MetricsParseNode>();
    }
};

class MetricsExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        // Register the aggregation stage.
        _registerStage<MetricsStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(MetricsExtension)
DEFINE_GET_EXTENSION()
