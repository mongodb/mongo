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
#include "mongo/db/extension/sdk/test_extension_util.h"

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
class InterruptTestExecAggStage : public sdk::ExecAggStageTransform {
public:
    static constexpr std::string kIdField = "_id";

    InterruptTestExecAggStage(int uassertOn)
        : sdk::ExecAggStageTransform(kInterruptTestStageName),
          _callCount(0),
          _uassertOn(uassertOn) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        MongoExtensionExecAggStage* execStage,
        ::MongoExtensionGetNextRequestType requestType) override {
        // Call checkForInterrupt to check if the operation has been interrupted.
        // If the checkForInterruptFail failpoint is active, this will mark the
        // operation as killed and cause checkForInterrupt to return a killed code.
        auto cancelled = execCtx.checkForInterrupt();
        if (cancelled.getCode() != 0) {
            sdk_uassert(11213401,
                        "$interruptTest must receive ErrorCodes::Interrupted",
                        cancelled.getCode() == 11601);
            return mongo::extension::ExtensionGetNextResult::eof();
        }

        auto input =
            _getSource().getNext(const_cast<MongoExtensionQueryExecutionContext*>(execCtx.get()));

        if (input.code == mongo::extension::GetNextCode::kPauseExecution) {
            return mongo::extension::ExtensionGetNextResult::pauseExecution();
        }
        if (input.code == mongo::extension::GetNextCode::kEOF) {
            return mongo::extension::ExtensionGetNextResult::ExtensionGetNextResult::eof();
        }

        auto doc = input.res.get();
        if (doc.hasField(kIdField) && doc[kIdField].numberInt() == _uassertOn) {
            sdk_uasserted(11213400, "$interruptTest triggered uassert");
        }

        // If we get here, the operation was not interrupted. Pass the source document through
        // unchanged.
        return mongo::extension::ExtensionGetNextResult::advanced(doc);
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

private:
    int _callCount;
    int _uassertOn;
};

class InterruptTestLogicalStage : public sdk::LogicalAggStage {
public:
    InterruptTestLogicalStage(int uassertOn) : _uassertOn(uassertOn) {};

    BSONObj serialize() const override {
        return BSON(kInterruptTestStageName << BSONObj());
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(kInterruptTestStageName << BSONObj());
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<InterruptTestExecAggStage>(_uassertOn);
    }

private:
    int _uassertOn;
};

class InterruptTestAstNode : public sdk::AggStageAstNode {
public:
    InterruptTestAstNode(int uassertOn)
        : sdk::AggStageAstNode(kInterruptTestStageName), _uassertOn(uassertOn) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<InterruptTestLogicalStage>(_uassertOn);
    }

private:
    int _uassertOn;
};

class InterruptTestParseNode : public sdk::AggStageParseNode {
public:
    InterruptTestParseNode(int uassertOn)
        : sdk::AggStageParseNode(kInterruptTestStageName), _uassertOn(uassertOn) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<InterruptTestAstNode>(_uassertOn)));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

private:
    int _uassertOn;
};

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
class InterruptTestStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = "$interruptTest";
    static inline const std::string kuassertOn = "uassertOn";

    InterruptTestStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName, false /* checkEmpty */);

        int docToUassertOn = 0;
        if (stageBson[kStageName].Obj().hasField(kuassertOn)) {
            docToUassertOn = stageBson[kStageName].Obj()[kuassertOn].numberInt();
        }
        return std::make_unique<InterruptTestParseNode>(docToUassertOn);
    }
};

class InterruptTestExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        // Register the aggregation stage.
        _registerStage<InterruptTestStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(InterruptTestExtension)
DEFINE_GET_EXTENSION()

