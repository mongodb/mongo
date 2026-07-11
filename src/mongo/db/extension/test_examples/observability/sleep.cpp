// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

#include <chrono>
#include <string_view>
#include <thread>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * $sleep is an extension stage designed for testing serverStatus metrics that track
 * time spent in extension getNext() code. The interface is modeled after MongoDB's
 * internal 'sleep' command.
 *
 * The stage sleeps for a configurable duration in each getNext() call, allowing tests
 * to verify that time tracking metrics report non-zero values.
 *
 * Syntax:
 *   {$sleep: {millis: <number>}}     - Sleep for N milliseconds per document
 *   {$sleep: {}}                     - No-op, passes documents through immediately
 *
 * Example:
 *   db.coll.aggregate([{$sleep: {millis: 10}}])
 */
class SleepExecStage : public sdk::TestExecStage {
public:
    static inline const std::string kMillisField = "millis";

    explicit SleepExecStage(std::string_view stageName, mongo::BSONObj arguments)
        : sdk::TestExecStage(stageName, arguments), _sleepMillis(0) {
        if (arguments.hasField(kMillisField) && arguments[kMillisField].isNumber()) {
            _sleepMillis += arguments[kMillisField].safeNumberLong();
        }
    }

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {

        auto input = _getSource()->getNext(execCtx.get());
        if (input.code != mongo::extension::GetNextCode::kAdvanced) {
            return input;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(_sleepMillis));

        // Pass the document through unchanged.
        return input;
    }

private:
    int64_t _sleepMillis;
};

DEFAULT_LOGICAL_STAGE(Sleep);
DEFAULT_AST_NODE(Sleep);
DEFAULT_PARSE_NODE(Sleep);

using SleepStageDescriptor = sdk::TestStageDescriptor<"$sleep", SleepParseNode>;

DEFAULT_EXTENSION(Sleep)
REGISTER_EXTENSION(SleepExtension)
DEFINE_GET_EXTENSION()
