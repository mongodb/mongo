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
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

#include <chrono>
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
