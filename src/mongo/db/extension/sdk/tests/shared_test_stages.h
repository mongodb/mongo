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
#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/distributed_plan_logic.h"
#include "mongo/db/extension/sdk/dpl_array_container.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/util/assert_util.h"

namespace mongo::extension::sdk {
inline StringData stringViewToStringData(std::string_view sv) {
    return StringData{sv.data(), sv.size()};
}
}  // namespace mongo::extension::sdk

namespace mongo::extension::sdk::shared_test_stages {

/**
 * Test scaffolding for the Aggregation Extension SDK.
 *
 * Provides aggregation stages and their companion types used by unit tests
 * to exercise the SDK/host plumbing end to end.
 *
 * Referenced by sdk/tests/aggregation_stage_test.cpp and host/document_source_extension_test.cpp.
 */
static constexpr std::string_view kNoOpName = "$noOp";
static constexpr std::string_view kSourceName = "$sourceStage";
static constexpr std::string_view kTransformStageName = "$transformStage";

class NoOpExecAggStage : public sdk::ExecAggStageTransform {
public:
    NoOpExecAggStage() : sdk::ExecAggStageTransform(kNoOpName) {}

    ExtensionGetNextResult getNext(const QueryExecutionContextHandle& execCtx,
                                   ::MongoExtensionExecAggStage* execStage,
                                   ::MongoExtensionGetNextRequestType requestType) override {
        return ExtensionGetNextResult::pauseExecution();
    }

    void open() override {}

    void reopen() override {}

    void close() override {}
};

class NoOpLogicalAggStage : public sdk::LogicalAggStage {
public:
    NoOpLogicalAggStage() {}

    BSONObj serialize() const override {
        return BSON(std::string(kNoOpName) << "serializedForExecution");
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<NoOpExecAggStage>();
    }
};

class NoOpAggStageAstNode : public sdk::AggStageAstNode {
public:
    NoOpAggStageAstNode() : sdk::AggStageAstNode(kNoOpName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<NoOpAggStageAstNode>();
    }
};

class NoOpAggStageParseNode : public sdk::AggStageParseNode {
public:
    NoOpAggStageParseNode() : sdk::AggStageParseNode(kNoOpName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<NoOpAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<NoOpAggStageParseNode>();
    }
};

class NoOpAggStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kNoOpName);

    NoOpAggStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        uassert(10596406,
                "Failed to parse $noOpExtension, $noOpExtension expects an object.",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());
        auto stageDefinition = stageBson.getField(kStageName).Obj();
        uassert(10596407,
                "Failed to parse $noOpExtension, missing boolean field \"foo\"",
                stageDefinition.hasField("foo") && stageDefinition.getField("foo").isBoolean());
        return std::make_unique<NoOpAggStageParseNode>();
    }

    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<NoOpAggStageDescriptor>();
    }
};

class FruitsAsDocumentsExecAggStage : public sdk::ExecAggStageSource {
public:
    FruitsAsDocumentsExecAggStage() : sdk::ExecAggStageSource(kSourceName) {}

    ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                   ::MongoExtensionExecAggStage* execStage,
                                   ::MongoExtensionGetNextRequestType requestType) override {
        if (_currentIndex >= _documents.size()) {
            return ExtensionGetNextResult::eof();
        }
        return ExtensionGetNextResult::advanced(_documents[_currentIndex++]);
    }

    // Allow this to be public for visibility in unit tests.
    UnownedExecAggStageHandle& _getSource() override {
        return sdk::ExecAggStageSource::_getSource();
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<FruitsAsDocumentsExecAggStage> make() {
        return std::make_unique<FruitsAsDocumentsExecAggStage>();
    }

private:
    // Every FruitsAsDocumentsExecAggStage object will have access to the same test document suite.
    static inline const std::vector<BSONObj> _documents = {
        BSON("_id" << 1 << "apples" << "red"),
        BSON("_id" << 2 << "oranges" << 5),
        BSON("_id" << 3 << "bananas" << false),
        BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee")),
        BSON("_id" << 5 << "pie" << 3.14159)};
    size_t _currentIndex = 0;
};

class FruitsAsDocumentsLogicalAggStage : public sdk::LogicalAggStage {
public:
    BSONObj serialize() const override {
        return BSON(std::string(kSourceName) << "serializedForExecution");
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return FruitsAsDocumentsExecAggStage::make();
    }
};

class FruitsAsDocumentsAstNode : public sdk::AggStageAstNode {
public:
    FruitsAsDocumentsAstNode() : sdk::AggStageAstNode(kSourceName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<FruitsAsDocumentsLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<FruitsAsDocumentsAstNode>();
    }
};

static constexpr std::string_view kExpandToExtAstName = "$expandToExtAst";
static constexpr std::string_view kExpandToExtParseName = "$expandToExtParse";
static constexpr std::string_view kExpandToHostParseName = "$expandToHostParse";
static constexpr std::string_view kExpandToHostAstName = "$expandToHostAst";
static constexpr std::string_view kExpandToMixedName = "$expandToMixed";

static const BSONObj kMatchSpec = BSON("$match" << BSON("a" << 1));
static const BSONObj kIdLookupSpec = BSON("$_internalSearchIdLookup" << BSONObj());

class FruitsAsDocumentsParseNode : public sdk::AggStageParseNode {
public:
    FruitsAsDocumentsParseNode() : sdk::AggStageParseNode(kSourceName) {}
    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<FruitsAsDocumentsAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<FruitsAsDocumentsParseNode>();
    }
};

class ExpandToExtAstParseNode : public sdk::AggStageParseNode {
public:
    ExpandToExtAstParseNode() : sdk::AggStageParseNode(kExpandToExtAstName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<NoOpAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<FruitsAsDocumentsParseNode>();
    }
};

class FruitsAsDocumentsDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kSourceName);

    FruitsAsDocumentsDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        uassert(10956900,
                "Failed to parse $sourceExtension, $sourceExtension expects an object.",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());
        auto stageDefinition = stageBson.getField(kStageName).Obj();
        return std::make_unique<FruitsAsDocumentsParseNode>();
    }

    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<FruitsAsDocumentsDescriptor>();
    }
};

class ExpandToExtAstDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kExpandToExtAstName);
    ExpandToExtAstDescriptor() : sdk::AggStageDescriptor(kStageName) {}
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj) const override {
        return std::make_unique<ExpandToExtAstParseNode>();
    }
    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<ExpandToExtAstDescriptor>();
    }
};

class ExpandToExtParseParseNode : public sdk::AggStageParseNode {
public:
    static int expandCalls;
    static constexpr size_t kExpansionSize = 1;

    ExpandToExtParseParseNode() : sdk::AggStageParseNode(kExpandToExtParseName) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<VariantNodeHandle> expand() const override {
        ++expandCalls;
        std::vector<VariantNodeHandle> out;
        out.reserve(kExpansionSize);
        out.emplace_back(
            new sdk::ExtensionAggStageParseNode(std::make_unique<NoOpAggStageParseNode>()));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};

inline int ExpandToExtParseParseNode::expandCalls = 0;

class ExpandToExtParseDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kExpandToExtParseName);
    ExpandToExtParseDescriptor() : sdk::AggStageDescriptor(kStageName) {}
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj) const override {
        return std::make_unique<ExpandToExtParseParseNode>();
    }
    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<ExpandToExtParseDescriptor>();
    }
};


class NoOpHostParseNode : public host::AggStageParseNode {
public:
    static inline std::unique_ptr<host::AggStageParseNode> make(BSONObj spec) {
        return std::make_unique<NoOpHostParseNode>(spec);
    }
};

class ExpandToHostParseParseNode : public sdk::AggStageParseNode {
public:
    ExpandToHostParseParseNode() : sdk::AggStageParseNode(kExpandToHostParseName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new host::HostAggStageParseNode(NoOpHostParseNode::make(kMatchSpec)));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};
class ExpandToHostParseDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kExpandToHostParseName);
    ExpandToHostParseDescriptor() : sdk::AggStageDescriptor(kStageName) {}
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj) const override {
        return std::make_unique<ExpandToHostParseParseNode>();
    }
    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<ExpandToHostParseDescriptor>();
    }
};

class ExpandToHostAstParseNode : public sdk::AggStageParseNode {
public:
    ExpandToHostAstParseNode() : sdk::AggStageParseNode(kExpandToHostAstName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(kExpansionSize);
        out.emplace_back(
            extension::sdk::HostServicesHandle::getHostServices()->createIdLookup(kIdLookupSpec));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<ExpandToHostAstParseNode>();
    }
};

class ExpandToHostAstDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kExpandToHostAstName);

    ExpandToHostAstDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj) const override {
        return std::make_unique<ExpandToHostAstParseNode>();
    }

    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<ExpandToHostAstDescriptor>();
    }
};

class ExpandToMixedParseNode : public sdk::AggStageParseNode {
public:
    ExpandToMixedParseNode() : sdk::AggStageParseNode(kExpandToMixedName) {}

    static constexpr size_t kExpansionSize = 4;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(kExpansionSize);
        out.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<NoOpAggStageAstNode>()));
        out.emplace_back(
            new sdk::ExtensionAggStageParseNode(std::make_unique<NoOpAggStageParseNode>()));
        out.emplace_back(new host::HostAggStageParseNode(NoOpHostParseNode::make(kMatchSpec)));
        out.emplace_back(
            extension::sdk::HostServicesHandle::getHostServices()->createIdLookup(kIdLookupSpec));
        return out;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return BSONObj();
    }
};

class ExpandToMixedDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kExpandToMixedName);
    ExpandToMixedDescriptor() : sdk::AggStageDescriptor(kStageName) {}
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj) const override {
        return std::make_unique<ExpandToMixedParseNode>();
    }
    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<ExpandToMixedDescriptor>();
    }
};

static constexpr std::string_view kTopName = "$top";
static constexpr std::string_view kMidAName = "$midA";
static constexpr std::string_view kMidBName = "$midB";
static constexpr std::string_view kLeafAName = "$leafA";
static constexpr std::string_view kLeafBName = "$leafB";
static constexpr std::string_view kLeafCName = "$leafC";
static constexpr std::string_view kLeafDName = "$leafD";

class LeafAAstNode : public sdk::AggStageAstNode {
public:
    LeafAAstNode() : sdk::AggStageAstNode(kLeafAName) {}
    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }
};

class LeafBAstNode : public sdk::AggStageAstNode {
public:
    LeafBAstNode() : sdk::AggStageAstNode(kLeafBName) {}
    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }
};

class LeafCAstNode : public sdk::AggStageAstNode {
public:
    LeafCAstNode() : sdk::AggStageAstNode(kLeafCName) {}
    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }
};

class LeafDAstNode : public sdk::AggStageAstNode {
public:
    LeafDAstNode() : sdk::AggStageAstNode(kLeafDName) {}
    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }
};

class MidAParseNode : public sdk::AggStageParseNode {
public:
    MidAParseNode() : sdk::AggStageParseNode(kMidAName) {}
    static constexpr size_t kExpansionSize = 2;
    size_t getExpandedSize() const override {
        return kExpansionSize;
    }
    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new sdk::ExtensionAggStageAstNode(std::make_unique<LeafAAstNode>()));
        out.emplace_back(new sdk::ExtensionAggStageAstNode(std::make_unique<LeafBAstNode>()));
        return out;
    }
    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};

class MidBParseNode : public sdk::AggStageParseNode {
public:
    MidBParseNode() : sdk::AggStageParseNode(kMidBName) {}
    static constexpr size_t kExpansionSize = 2;
    size_t getExpandedSize() const override {
        return kExpansionSize;
    }
    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new sdk::ExtensionAggStageAstNode(std::make_unique<LeafCAstNode>()));
        out.emplace_back(new sdk::ExtensionAggStageAstNode(std::make_unique<LeafDAstNode>()));
        return out;
    }
    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};

class TopParseNode : public sdk::AggStageParseNode {
public:
    TopParseNode() : sdk::AggStageParseNode(kTopName) {}
    static constexpr size_t kExpansionSize = 2;
    size_t getExpandedSize() const override {
        return kExpansionSize;
    }
    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> out;
        out.reserve(kExpansionSize);
        out.emplace_back(new sdk::ExtensionAggStageParseNode(std::make_unique<MidAParseNode>()));
        out.emplace_back(new sdk::ExtensionAggStageParseNode(std::make_unique<MidBParseNode>()));
        return out;
    }
    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts*) const override {
        return {};
    }
};


class TopDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kTopName);
    TopDescriptor() : sdk::AggStageDescriptor(kStageName) {}
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj) const override {
        return std::make_unique<TopParseNode>();
    }
    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<TopDescriptor>();
    }
};

// Test stages for static properties (position)
static constexpr std::string_view kNoneName = "$none";
static constexpr std::string_view kFirstName = "$first";
static constexpr std::string_view kLastName = "$last";
static constexpr std::string_view kBadPosName = "$badPos";
static constexpr std::string_view kBadPosTypeName = "$badPosType";
static constexpr std::string_view kUnknownPropertyName = "$unknownProperty";
static constexpr std::string_view kTransformName = "$transform";
static constexpr std::string_view kSearchLikeSourceStageName = "$searchLikeSource";
static constexpr std::string_view kBadRequiresInputDocSourceTypeName =
    "$badRequiresInputDocSourceType";

class NonePosAggStageAstNode : public sdk::AggStageAstNode {
public:
    NonePosAggStageAstNode() : sdk::AggStageAstNode(kNoneName) {}

    BSONObj getProperties() const override {
        return BSON("position" << "none");
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<NonePosAggStageAstNode>();
    }
};

class FirstPosAggStageAstNode : public sdk::AggStageAstNode {
public:
    FirstPosAggStageAstNode() : sdk::AggStageAstNode(kFirstName) {}

    BSONObj getProperties() const override {
        return BSON("position" << "first");
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<FirstPosAggStageAstNode>();
    }
};

class LastPosAggStageAstNode : public sdk::AggStageAstNode {
public:
    LastPosAggStageAstNode() : sdk::AggStageAstNode(kLastName) {}

    BSONObj getProperties() const override {
        return BSON("position" << "last");
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<LastPosAggStageAstNode>();
    }
};

class BadPosAggStageAstNode : public sdk::AggStageAstNode {
public:
    BadPosAggStageAstNode() : sdk::AggStageAstNode(kBadPosName) {}

    BSONObj getProperties() const override {
        return BSON("position" << "bogus");
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<BadPosAggStageAstNode>();
    }
};

class BadPosTypeAggStageAstNode : public sdk::AggStageAstNode {
public:
    BadPosTypeAggStageAstNode() : sdk::AggStageAstNode(kBadPosTypeName) {}

    BSONObj getProperties() const override {
        return BSON("position" << BSONArray(BSON_ARRAY(1)));
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<BadPosTypeAggStageAstNode>();
    }
};

class UnknownPropertyAggStageAstNode : public sdk::AggStageAstNode {
public:
    UnknownPropertyAggStageAstNode() : sdk::AggStageAstNode(kUnknownPropertyName) {}

    BSONObj getProperties() const override {
        return BSON("unknownProperty" << "null");
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<UnknownPropertyAggStageAstNode>();
    }
};

class TransformAggStageAstNode : public sdk::AggStageAstNode {
public:
    TransformAggStageAstNode() : sdk::AggStageAstNode(kTransformName) {}

    BSONObj getProperties() const override {
        return BSON("requiresInputDocSource" << true);
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<TransformAggStageAstNode>();
    }
};

class SearchLikeSourceAggStageAstNode : public sdk::AggStageAstNode {
public:
    SearchLikeSourceAggStageAstNode() : sdk::AggStageAstNode(kSearchLikeSourceStageName) {}

    BSONObj getProperties() const override {
        return BSON("requiresInputDocSource"
                    << false << "position" << "first" << "hostType"
                    << "anyShard"
                    << "requiredMetadataFields" << BSON_ARRAY("score") << "providedMetadataFields"
                    << BSON_ARRAY("searchHighlights") << "preservesUpstreamMetadata" << false);
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<SearchLikeSourceAggStageAstNode>();
    }
};

class SearchLikeSourceWithPreserveUpstreamMetadataAstNode : public SearchLikeSourceAggStageAstNode {
public:
    BSONObj getProperties() const override {
        return BSON("requiresInputDocSource"
                    << false << "position" << "first" << "hostType"
                    << "anyShard"
                    << "requiredMetadataFields" << BSON_ARRAY("score") << "providedMetadataFields"
                    << BSON_ARRAY("searchHighlights") << "preservesUpstreamMetadata" << true);
    }

    static std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<SearchLikeSourceWithPreserveUpstreamMetadataAstNode>();
    }
};

class SearchLikeSourceWithInvalidRequiredMetadataFieldAstNode
    : public SearchLikeSourceAggStageAstNode {
public:
    BSONObj getProperties() const override {
        return BSON("requiresInputDocSource"
                    << false << "position" << "first" << "hostType"
                    << "anyShard"
                    << "requiredMetadataFields" << BSON_ARRAY("customSearchScore")
                    << "providedMetadataFields" << BSON_ARRAY("searchScore" << "searchHighlights"));
    }

    static std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<SearchLikeSourceWithInvalidRequiredMetadataFieldAstNode>();
    }
};

class SearchLikeSourceWithInvalidProvidedMetadataFieldAstNode
    : public SearchLikeSourceAggStageAstNode {
public:
    BSONObj getProperties() const override {
        return BSON("requiresInputDocSource"
                    << false << "position" << "first" << "hostType"
                    << "anyShard"
                    << "requiredMetadataFields" << BSON_ARRAY("searchScore")
                    << "providedMetadataFields"
                    << BSON_ARRAY("customSearchScore" << "searchHighlights"));
    }

    static std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<SearchLikeSourceWithInvalidProvidedMetadataFieldAstNode>();
    }
};

class BadRequiresInputDocSourceTypeAggStageAstNode : public sdk::AggStageAstNode {
public:
    BadRequiresInputDocSourceTypeAggStageAstNode()
        : sdk::AggStageAstNode(kBadRequiresInputDocSourceTypeName) {}

    BSONObj getProperties() const override {
        return BSON("requiresInputDocSource" << BSONArray(BSON_ARRAY(1)));
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<BadRequiresInputDocSourceTypeAggStageAstNode>();
    }
};

static constexpr std::string_view kDesugarToEmptyName = "$desugarToEmpty";
static constexpr std::string_view kCountingName = "$counting";
static constexpr std::string_view kNestedDesugaringName = "$nestedDesugaring";
static constexpr std::string_view kGetExpandedSizeLessName =
    "$getExpandedSizeLessThanActualExpansionSize";
static constexpr std::string_view kGetExpandedSizeGreaterName =
    "$getExpandedSizeGreaterThanActualExpansionSize";
static constexpr std::string_view kExpandToHostName = "$expandToHost";

class DesugarToEmptyParseNode : public sdk::AggStageParseNode {
public:
    DesugarToEmptyParseNode() : sdk::AggStageParseNode(kDesugarToEmptyName) {}

    size_t getExpandedSize() const override {
        return 0;
    }

    std::vector<VariantNodeHandle> expand() const override {
        return {};
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<DesugarToEmptyParseNode>();
    }
};

class CountingAst final : public sdk::AggStageAstNode {
public:
    static int alive;

    CountingAst() : sdk::AggStageAstNode(kCountingName) {
        ++alive;
    }

    ~CountingAst() override {
        --alive;
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<CountingAst>();
    }
};

class CountingParse final : public sdk::AggStageParseNode {
public:
    static constexpr size_t kExpansionSize = 1;
    static int alive;

    CountingParse() : sdk::AggStageParseNode(kCountingName) {
        ++alive;
    }

    ~CountingParse() override {
        --alive;
    }

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(CountingAst::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<CountingParse>();
    }
};

class CountingLogicalStage final : public sdk::LogicalAggStage {
public:
    static int alive;

    CountingLogicalStage() {
        ++alive;
    }

    ~CountingLogicalStage() override {
        --alive;
    }

    BSONObj serialize() const override {
        return BSON(std::string(kCountingName) << "serializedForExecution");
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<NoOpExecAggStage>();
    }

    static inline std::unique_ptr<sdk::LogicalAggStage> make() {
        return std::make_unique<CountingLogicalStage>();
    }
};

inline int CountingParse::alive = 0;
inline int CountingAst::alive = 0;
inline int CountingLogicalStage::alive = 0;

class NestedDesugaringParseNode final : public sdk::AggStageParseNode {
public:
    NestedDesugaringParseNode() : sdk::AggStageParseNode(kNestedDesugaringName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(CountingAst::make()));
        expanded.emplace_back(new sdk::ExtensionAggStageParseNode(CountingParse::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<NestedDesugaringParseNode>();
    }
};

class GetExpandedSizeLessThanActualExpansionSizeParseNode final : public sdk::AggStageParseNode {
public:
    GetExpandedSizeLessThanActualExpansionSizeParseNode()
        : sdk::AggStageParseNode(kGetExpandedSizeLessName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize - 1;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(CountingAst::make()));
        expanded.emplace_back(new sdk::ExtensionAggStageParseNode(CountingParse::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<GetExpandedSizeLessThanActualExpansionSizeParseNode>();
    }
};

class GetExpandedSizeGreaterThanActualExpansionSizeParseNode final : public sdk::AggStageParseNode {
public:
    GetExpandedSizeGreaterThanActualExpansionSizeParseNode()
        : sdk::AggStageParseNode(kGetExpandedSizeGreaterName) {}

    static constexpr size_t kExpansionSize = 2;

    size_t getExpandedSize() const override {
        return kExpansionSize + 1;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(CountingAst::make()));
        expanded.emplace_back(new sdk::ExtensionAggStageParseNode(CountingParse::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<GetExpandedSizeGreaterThanActualExpansionSizeParseNode>();
    }
};

class ExpandToHostParseNode : public sdk::AggStageParseNode {
public:
    ExpandToHostParseNode() : sdk::AggStageParseNode(kExpandToHostName) {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        auto spec = BSON(stringViewToStringData(kExpandToHostName) << BSONObj());
        expanded.emplace_back(
            sdk::HostServicesHandle::getHostServices()->createHostAggStageParseNode(spec));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<ExpandToHostParseNode>();
    }
};

class NameMismatchParseNode : public sdk::AggStageParseNode {
public:
    NameMismatchParseNode() : sdk::AggStageParseNode("$nameB") {}

    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(NoOpAggStageAstNode::make()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<NameMismatchParseNode>();
    }
};

class NameMismatchStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string("$nameA");

    NameMismatchStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        return std::make_unique<NameMismatchParseNode>();
    }

    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<NameMismatchStageDescriptor>();
    }
};

class ValidExtensionExecAggStage : public extension::sdk::ExecAggStageSource {
public:
    ValidExtensionExecAggStage() : sdk::ExecAggStageSource("$validExtension") {}

    extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage,
        ::MongoExtensionGetNextRequestType requestType) override {
        if (_results.empty()) {
            return extension::ExtensionGetNextResult::eof();
        }
        if (_results.size() == 2) {
            // The result at the front of the queue is removed so that the size doesn't stay at 2.
            // This needs to be done so that the EOF case can be tested. Note that the behavior of
            // removing from the results queue for a "pause execution" state does not accurately
            // represent a "paused execution" state in a getNext() function.
            _results.pop_front();
            return extension::ExtensionGetNextResult::pauseExecution();
        } else {
            auto result = extension::ExtensionGetNextResult::advanced(_results.front());
            _results.pop_front();
            return result;
        }
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<sdk::ExecAggStageSource> make() {
        return std::make_unique<ValidExtensionExecAggStage>();
    }

private:
    std::deque<BSONObj> _results = {
        BSON("meow" << "adithi"), BSON("meow" << "josh"), BSON("meow" << "cedric")};
};

class TestLogicalStageCompile : public LogicalAggStage {
public:
    static constexpr StringData kStageName = "$testCompile";
    static constexpr StringData kStageSpec = "mongodb";

    BSONObj serialize() const override {
        return BSON(kStageName << kStageSpec);
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(kStageName << verbosity);
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<ValidExtensionExecAggStage>();
    }

    static inline std::unique_ptr<extension::sdk::LogicalAggStage> make() {
        return std::make_unique<TestLogicalStageCompile>();
    }
};

class NoOpExtensionExecAggStage : public sdk::ExecAggStageTransform {
public:
    NoOpExtensionExecAggStage() : sdk::ExecAggStageTransform("$noOpExt") {}

    ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& expCtx,
                                   ::MongoExtensionExecAggStage* execStage,
                                   ::MongoExtensionGetNextRequestType requestType) override {
        MONGO_UNIMPLEMENTED;
    }

    void setSource(UnownedExecAggStageHandle source) override {
        MONGO_UNIMPLEMENTED;
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<sdk::ExecAggStageTransform> make() {
        return std::make_unique<NoOpExtensionExecAggStage>();
    }
};

class AddFruitsToDocumentsExecAggStage : public sdk::ExecAggStageTransform {
public:
    AddFruitsToDocumentsExecAggStage() : sdk::ExecAggStageTransform(kTransformStageName) {}

    // Loosely modeled on the behavior of the existing tranform stage:
    // exec::agg::SingleDocumentTransformationStage::doGetNext(), more specifically, the $addFields
    // behavior.
    ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                   MongoExtensionExecAggStage* execStage,
                                   ::MongoExtensionGetNextRequestType requestType) override {
        auto input = _getSource().getNext(execCtx.get());
        if (input.code == GetNextCode::kPauseExecution) {
            return ExtensionGetNextResult::pauseExecution();
        }
        if (input.code == GetNextCode::kEOF) {
            return ExtensionGetNextResult::eof();
        }
        if (_currentIndex >= _documents.size()) {
            return ExtensionGetNextResult::eof();
        }
        BSONObjBuilder bob;
        bob.append("existingDoc", input.res.get());
        // Transform the returned input document by adding a new field.
        bob.append("addedFields", _documents[_currentIndex++]);
        return ExtensionGetNextResult::advanced(bob.obj());
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<sdk::ExecAggStageTransform> make() {
        return std::make_unique<AddFruitsToDocumentsExecAggStage>();
    }

private:
    // Every AddFruitsToDocumentsExecAggStage object will have access to the same test document
    // suite.
    static inline const std::vector<BSONObj> _documents = {
        BSON("_id" << 1 << "apples" << "red"),
        BSON("_id" << 2 << "oranges" << 5),
        BSON("_id" << 3 << "bananas" << false),
        BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee")),
        BSON("_id" << 5 << "pie" << 3.14159)};
    size_t _currentIndex = 0;
};

class AddFruitsToDocumentsLogicalAggStage : public sdk::LogicalAggStage {
public:
    BSONObj serialize() const override {
        return BSON(std::string(kTransformName) << "serializedForExecution");
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return AddFruitsToDocumentsExecAggStage::make();
    }
};

class AddFruitsToDocumentsAggStageAstNode : public sdk::AggStageAstNode {
public:
    AddFruitsToDocumentsAggStageAstNode() : sdk::AggStageAstNode(kTransformName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<AddFruitsToDocumentsLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<AddFruitsToDocumentsAggStageAstNode>();
    }
};

class AddFruitsToDocumentsAggStageParseNode : public sdk::AggStageParseNode {
public:
    AddFruitsToDocumentsAggStageParseNode() : sdk::AggStageParseNode(kTransformName) {}
    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<AddFruitsToDocumentsAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<AddFruitsToDocumentsAggStageParseNode>();
    }
};

class AddFruitsToDocumentsAggStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kTransformName);

    AddFruitsToDocumentsAggStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        uassert(10957203,
                "Failed to parse $transformExtension, $transformExtension expects an object.",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());
        auto stageDefinition = stageBson.getField(kStageName).Obj();
        return std::make_unique<AddFruitsToDocumentsAggStageParseNode>();
    }

    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<AddFruitsToDocumentsAggStageDescriptor>();
    }
};

class EmptyDistributedPlanLogic : public sdk::DistributedPlanLogicBase {
public:
    std::unique_ptr<sdk::DPLArrayContainer> getShardsPipeline() const override {
        return {};
    }

    std::unique_ptr<sdk::DPLArrayContainer> getMergingPipeline() const override {
        return {};
    }

    BSONObj getSortPattern() const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::DistributedPlanLogicBase> make() {
        return std::make_unique<EmptyDistributedPlanLogic>();
    }
};

class MockQueryExecutionContext : public host_connector::QueryExecutionContextBase {
public:
    Status checkForInterrupt() const override {
        return Status::OK();
    }

    host_connector::HostOperationMetricsHandle* getMetrics(
        const std::string& stageName, const UnownedExecAggStageHandle& execStage) const override {
        return nullptr;
    }
};

}  // namespace mongo::extension::sdk::shared_test_stages
