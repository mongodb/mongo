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

#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk::shared_test_stages {

constexpr char kTransformName[] = "$transformStage";

/**
 * This file defines basic classes for all parts of the query lifecyle for a transform stage. Custom
 * stages may parse, expand, etc to a transform class at any point in their lifecycle when custom
 * behavior has completed.
 *
 * Note: transform classes provide default constructors and make() methods to support easy
 * construction in unit tests, but the non-default constructor should always be used in integration
 * tests or any setting where the end-to-end query lifecycle is being exercised.
 */

class TransformExecAggStage : public TestExecStage {
public:
    TransformExecAggStage() : TestExecStage(kTransformName, mongo::BSONObj()) {}

    TransformExecAggStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : TestExecStage(stageName, arguments) {}

    static std::unique_ptr<sdk::ExecAggStageBase> make() {
        return std::make_unique<TransformExecAggStage>();
    }
};

class TransformLogicalAggStage : public TestLogicalStage<TransformExecAggStage> {
public:
    TransformLogicalAggStage() : TestLogicalStage(kTransformName, mongo::BSONObj()) {}

    TransformLogicalAggStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : TestLogicalStage(stageName, arguments) {}

    static std::unique_ptr<sdk::LogicalAggStage> make() {
        return std::make_unique<TransformLogicalAggStage>();
    }
};

class TransformAggStageAstNode : public TestAstNode<TransformLogicalAggStage> {
public:
    TransformAggStageAstNode() : TestAstNode(kTransformName, mongo::BSONObj()) {}

    TransformAggStageAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : TestAstNode(stageName, arguments) {}

    static std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<TransformAggStageAstNode>();
    }
};

class TransformAggStageParseNode : public TestParseNode<TransformAggStageAstNode> {
public:
    TransformAggStageParseNode() : TestParseNode(kTransformName, mongo::BSONObj()) {}

    TransformAggStageParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : TestParseNode(stageName, arguments) {}

    static std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<TransformAggStageParseNode>();
    }
};

class TransformAggStageDescriptor
    : public TestStageDescriptor<kTransformName, TransformAggStageParseNode> {
public:
    void validate(const BSONObj& arguments) const override {
        uassert(10596407,
                "Failed to parse $transformStage, missing boolean field \"foo\"",
                arguments.hasField("foo") && arguments.getField("foo").isBoolean());
    }

    static std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<TransformAggStageDescriptor>();
    }
};

}  // namespace mongo::extension::sdk::shared_test_stages
