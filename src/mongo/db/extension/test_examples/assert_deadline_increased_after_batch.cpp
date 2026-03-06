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
#include "mongo/db/extension/sdk/distributed_plan_logic.h"
#include "mongo/db/extension/sdk/dpl_array_container.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

#include <memory>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * AssertDeadlineIncreaseAfterBatchExecStage asserts that the operation deadline increases between
 * batches. The stage is not aware of what the actual batch size is, and instead relies on the user
 * to provide the correct batch size as part of the stage's spec. While the current iteration is
 * deemed to be within the current batch, this stage asserts that the deadline remains unchanged.
 *
 */
class AssertDeadlineIncreaseAfterBatchExecStage : public sdk::TestExecStage {
public:
    static inline const std::string kBatchSizeField = "batchSize";

    AssertDeadlineIncreaseAfterBatchExecStage(std::string_view stageName, BSONObj arguments)
        : sdk::TestExecStage(stageName, arguments),
          _batchSize([&]() {
              sdk_uassert(11650001,
                          "$assertDeadlineIncreaseAfterBatch requires a 'batchSize' integer field",
                          arguments.hasField(kBatchSizeField) &&
                              arguments[kBatchSizeField].isNumber());
              const auto batchSize = arguments[kBatchSizeField].numberInt();
              sdk_uassert(11650002,
                          "$assertDeadlineIncreaseAfterBatch batchSize must be positive",
                          batchSize > 0);
              return batchSize;
          }()),
          _callCount(0),
          _lastDeadline(0) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        MongoExtensionExecAggStage* execStage) override {
        // Get the current deadline from the execution context.
        const int64_t currentDeadline = execCtx->getDeadlineTimestampMs();
        ++_callCount;
        if (_callCount % _batchSize == 1 || _batchSize == 1) {
            // We've started a new batch. The deadline should have increased (been refreshed for the
            // new getMore).
            sdk_uassert(
                11650003,
                fmt::format("$assertDeadlineIncreaseAfterBatch: expected deadline to increase at "
                            "batch boundary, but it did not. currentDeadline={}, lastDeadline={}",
                            currentDeadline,
                            _lastDeadline),
                currentDeadline > _lastDeadline);

        } else {
            // Within a batch: the deadline should not have changed.
            sdk_uassert(
                11650004,
                fmt::format(
                    "$assertDeadlineIncreaseAfterBatch: expected deadline to remain the same "
                    "within a batch, but it changed. currentDeadline={}, lastDeadline={}",
                    currentDeadline,
                    _lastDeadline),
                currentDeadline == _lastDeadline);
        }

        _lastDeadline = currentDeadline;

        auto input =
            _getSource()->getNext(const_cast<MongoExtensionQueryExecutionContext*>(execCtx.get()));

        if (input.code != mongo::extension::GetNextCode::kAdvanced) {
            return input;
        }

        auto doc = input.resultDocument->getUnownedBSONObj();
        return mongo::extension::ExtensionGetNextResult::advanced(
            mongo::extension::ExtensionBSONObj::makeAsByteBuf(doc));
    }

private:
    const size_t _batchSize;
    size_t _callCount;
    int64_t _lastDeadline;
};

/**
 * AssertDeadlineIncreaseAfterBatchLogicalStage specifies a DPL with only a merge pipeline, since
 * this stage must always run on the router node. We do this because in a sharded cluster, mongos
 * sets the internal batch size to 0 instead of propagating the client provided batch size, which
 * would cause the stage to fail if running on a shard.
 */
class AssertDeadlineIncreaseAfterBatchLogicalStage
    : public sdk::TestLogicalStage<AssertDeadlineIncreaseAfterBatchExecStage> {
public:
    AssertDeadlineIncreaseAfterBatchLogicalStage(std::string_view stageName,
                                                 const mongo::BSONObj& arguments)
        : sdk::TestLogicalStage<AssertDeadlineIncreaseAfterBatchExecStage>(stageName, arguments) {}
    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<AssertDeadlineIncreaseAfterBatchLogicalStage>(_name, _arguments);
    }

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        sdk::DistributedPlanLogic dpl;
        {
            std::vector<mongo::extension::VariantDPLHandle> pipeline;
            pipeline.emplace_back(mongo::extension::LogicalAggStageHandle{
                new sdk::ExtensionLogicalAggStageAdapter(clone())});
            dpl.mergingPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        }
        return dpl;
    }
};
DEFAULT_AST_NODE(AssertDeadlineIncreaseAfterBatch);
DEFAULT_PARSE_NODE(AssertDeadlineIncreaseAfterBatch);

/**
 * $assertDeadlineIncreaseAfterBatch aggregation stage.
 *
 * Syntax: {$assertDeadlineIncreaseAfterBatch: {batchSize: <integer>}}
 *
 * This stage passes through documents from its source unchanged. During
 * execution, it tracks the number of getNext() calls per batch and asserts:
 *   - Within a batch (calls < batchSize): the deadline has not changed.
 *   - At the batch boundary (call == batchSize): the deadline has increased.
 *
 * This is used to verify that the server correctly refreshes the operation deadline when issuing
 * a getMore command.
 */
using AssertDeadlineIncreaseAfterBatchStageDescriptorBase =
    sdk::TestStageDescriptor<"$assertDeadlineIncreaseAfterBatch",
                             AssertDeadlineIncreaseAfterBatchParseNode>;

class AssertDeadlineIncreaseAfterBatchStageDescriptor
    : public AssertDeadlineIncreaseAfterBatchStageDescriptorBase {
    void validate(const mongo::BSONObj& arguments) const override {
        sdk_uassert(
            11650005,
            "$assertDeadlineIncreaseAfterBatch requires a 'batchSize' field that is a number",
            arguments.hasField(AssertDeadlineIncreaseAfterBatchExecStage::kBatchSizeField) &&
                arguments[AssertDeadlineIncreaseAfterBatchExecStage::kBatchSizeField].isNumber());
        sdk_uassert(
            11650006,
            "$assertDeadlineIncreaseAfterBatch 'batchSize' must be a positive integer",
            arguments[AssertDeadlineIncreaseAfterBatchExecStage::kBatchSizeField].numberInt() > 0);
    }
};

DEFAULT_EXTENSION(AssertDeadlineIncreaseAfterBatch)
REGISTER_EXTENSION(AssertDeadlineIncreaseAfterBatchExtension)
DEFINE_GET_EXTENSION()
