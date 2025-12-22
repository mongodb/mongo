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

#include <memory>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

static constexpr std::string kInterruptTestStageName = "$interruptTest";

/**
 * ExecAggStage that calls checkForInterrupt in getNext().
 * This stage is designed to work with the checkForInterruptFail failpoint.
 * The failpoint should be set externally (e.g., in a test) before running
 * a query with this stage. When the failpoint is active and checkForInterrupt
 * is called, the operation will be marked as killed.
 */
class InterruptTestExecStage : public sdk::TestExecStage {
public:
    static constexpr std::string kIdField = "_id";
    static inline const std::string kuassertOn = "uassertOn";

    InterruptTestExecStage(std::string_view stageName, BSONObj arguments)
        : sdk::TestExecStage(stageName, arguments), _callCount(0), _uassertOn(0) {
        if (arguments.hasField(kuassertOn) && arguments[kuassertOn].isNumber()) {
            _uassertOn = arguments[kuassertOn].numberInt();
        }
    }

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        MongoExtensionExecAggStage* execStage) override {
        // Call checkForInterrupt to check if the operation has been interrupted.
        // If the checkForInterruptFail failpoint is active, this will mark the
        // operation as killed and cause checkForInterrupt to return a killed code.
        auto cancelled = execCtx->checkForInterrupt();
        if (cancelled.getCode() != 0) {
            sdk_uassert(11213401,
                        "$interruptTest must receive ErrorCodes::Interrupted",
                        cancelled.getCode() == 11601);
            return mongo::extension::ExtensionGetNextResult::eof();
        }

        auto input =
            _getSource()->getNext(const_cast<MongoExtensionQueryExecutionContext*>(execCtx.get()));

        if (input.code == mongo::extension::GetNextCode::kPauseExecution) {
            return mongo::extension::ExtensionGetNextResult::pauseExecution();
        }
        if (input.code == mongo::extension::GetNextCode::kEOF) {
            return mongo::extension::ExtensionGetNextResult::ExtensionGetNextResult::eof();
        }

        auto doc = input.resultDocument->getUnownedBSONObj();
        if (doc.hasField(kIdField) && doc[kIdField].numberInt() == _uassertOn) {
            sdk_uasserted(11213400, "$interruptTest triggered uassert");
        }

        // If we get here, the operation was not interrupted. Pass the source document through
        // unchanged.
        return mongo::extension::ExtensionGetNextResult::advanced(
            mongo::extension::ExtensionBSONObj::makeAsByteBuf(doc));
    }

private:
    int _callCount;
    int _uassertOn;
};

DEFAULT_LOGICAL_STAGE(InterruptTest);
DEFAULT_AST_NODE(InterruptTest);
DEFAULT_PARSE_NODE(InterruptTest);

/**
 * $interruptTest aggregation stage that calls checkForInterrupt in getNext().
 *
 * Syntax: {$interruptTest: {}}
 *
 * This stage is designed to test interrupt handling. It calls checkForInterrupt()
 * in each getNext() call. When used with the checkForInterruptFail failpoint
 * (set externally before the query), the operation will be interrupted.
 *
 * Example usage:
 *   1. Set the failpoint: db.adminCommand({configureFailPoint: "checkForInterruptFail", mode:
 * "alwaysOn"})
 *   2. Run a query: db.coll.aggregate([{$interruptTest: {}}])
 *   3. The query should fail with an interrupt error.
 */
using InterruptTestStageDescriptor =
    sdk::TestStageDescriptor<"$interruptTest", InterruptTestParseNode>;

DEFAULT_EXTENSION(InterruptTest)
REGISTER_EXTENSION(InterruptTestExtension)
DEFINE_GET_EXTENSION()

