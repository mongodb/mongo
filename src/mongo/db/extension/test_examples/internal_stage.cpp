/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;

namespace {
constexpr char kInternalStageName[] = "$_testInternalStage";

/**
 * $_testInternalStage is an internal-only transform stage: it reuses the shared transform chain but
 * declares itself internal-only, so a user pipeline using it is rejected with error 5491300.
 */
class InternalStageDescriptor
    : public sdk::TestStageDescriptor<kInternalStageName,
                                      sdk::shared_test_stages::TransformAggStageParseNode> {
public:
    ::MongoExtensionClientType getClientType() const override {
        return ::kMongoExtensionClientTypeInternal;
    }
};

constexpr char kDesugarsToInternalName[] = "$testDesugarsToInternal";

/**
 * $testDesugarsToInternal is a user-facing stage that desugars into a transform stage followed by
 * the internal-only $_testInternalStage, mirroring the $search -> $_internalMongotRemote shape.
 * Internal-only enforcement applies to what the user wrote, so this stage must remain usable from
 * user pipelines even though its expansion contains an internal-only stage.
 */
class DesugarsToInternalParseNode : public sdk::AggStageParseNode {
public:
    DesugarsToInternalParseNode() : sdk::AggStageParseNode(kDesugarsToInternalName) {}

    size_t getExpandedSize() const override {
        return 2;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(
            mongo::extension::AggStageAstNodeHandle{new sdk::ExtensionAggStageAstNodeAdapter(
                std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>(
                    kDesugarsToInternalName, mongo::BSONObj()))});
        expanded.emplace_back(
            mongo::extension::AggStageAstNodeHandle{new sdk::ExtensionAggStageAstNodeAdapter(
                std::make_unique<sdk::shared_test_stages::TransformAggStageAstNode>(
                    kInternalStageName, mongo::BSONObj()))});
        return expanded;
    }

    mongo::BSONObj getQueryShape(const sdk::QueryShapeOptsHandle& ctx) const override {
        return BSON(std::string(getName()) << mongo::BSONObj());
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<DesugarsToInternalParseNode>();
    }
};

using DesugarsToInternalDescriptor =
    sdk::TestStageDescriptor<kDesugarsToInternalName, DesugarsToInternalParseNode>;

class InternalStageExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<InternalStageDescriptor>(portal);
        _registerStage<DesugarsToInternalDescriptor>(portal);
    }
};
}  // namespace

REGISTER_EXTENSION(InternalStageExtension)
DEFINE_GET_EXTENSION()
