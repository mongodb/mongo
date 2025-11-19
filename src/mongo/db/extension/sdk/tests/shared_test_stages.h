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
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/shared/get_next_result.h"

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

class NoOpExecAggStage : public sdk::ExecAggStage {
public:
    NoOpExecAggStage() : sdk::ExecAggStage(kNoOpName) {}

    ExtensionGetNextResult getNext(const QueryExecutionContextHandle& execCtx,
                                   const MongoExtensionExecAggStage* execStage,
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

    std::unique_ptr<sdk::ExecAggStage> compile() const override {
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

class SourceExecAggStage : public sdk::ExecAggStage {
public:
    SourceExecAggStage() : sdk::ExecAggStage(kSourceName) {}

    ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                   const MongoExtensionExecAggStage* execStage,
                                   ::MongoExtensionGetNextRequestType requestType) override {
        if (_currentIndex >= _documents.size()) {
            return ExtensionGetNextResult::eof();
        }
        return ExtensionGetNextResult::advanced(_documents[_currentIndex++]);
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

private:
    // Every SourceExecAggStage object will have access to the same test document suite.
    static inline const std::vector<BSONObj> _documents = {
        BSON("_id" << 1 << "apples" << "red"),
        BSON("_id" << 2 << "oranges" << 5),
        BSON("_id" << 3 << "bananas" << false),
        BSON("_id" << 4 << "tropical fruits" << BSON_ARRAY("rambutan" << "durian" << "lychee")),
        BSON("_id" << 5 << "pie" << 3.14159)};
    size_t _currentIndex = 0;
};

class SourceLogicalAggStage : public sdk::LogicalAggStage {
public:
    BSONObj serialize() const override {
        return BSON(std::string(kSourceName) << "serializedForExecution");
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    std::unique_ptr<sdk::ExecAggStage> compile() const override {
        return std::make_unique<SourceExecAggStage>();
    }
};

class SourceAggStageAstNode : public sdk::AggStageAstNode {
public:
    SourceAggStageAstNode() : sdk::AggStageAstNode(kSourceName) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<SourceLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<SourceAggStageAstNode>();
    }
};

static constexpr std::string_view kExpandToExtAstName = "$expandToExtAst";
static constexpr std::string_view kExpandToExtParseName = "$expandToExtParse";
static constexpr std::string_view kExpandToHostParseName = "$expandToHostParse";
static constexpr std::string_view kExpandToHostAstName = "$expandToHostAst";
static constexpr std::string_view kExpandToMixedName = "$expandToMixed";

static const BSONObj kMatchSpec = BSON("$match" << BSON("a" << 1));
static const BSONObj kIdLookupSpec = BSON("$_internalSearchIdLookup" << BSONObj());

class SourceAggStageParseNode : public sdk::AggStageParseNode {
public:
    SourceAggStageParseNode() : sdk::AggStageParseNode(kSourceName) {}
    static constexpr size_t kExpansionSize = 1;

    size_t getExpandedSize() const override {
        return kExpansionSize;
    }

    std::vector<VariantNodeHandle> expand() const override {
        std::vector<VariantNodeHandle> expanded;
        expanded.reserve(kExpansionSize);
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<SourceAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<SourceAggStageParseNode>();
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
        return std::make_unique<SourceAggStageParseNode>();
    }
};

class SourceAggStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(kSourceName);

    SourceAggStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        uassert(10956900,
                "Failed to parse $sourceExtension, $sourceExtension expects an object.",
                stageBson.hasField(kStageName) && stageBson.getField(kStageName).isABSONObj());
        auto stageDefinition = stageBson.getField(kStageName).Obj();
        return std::make_unique<SourceAggStageParseNode>();
    }

    static inline std::unique_ptr<sdk::AggStageDescriptor> make() {
        return std::make_unique<SourceAggStageDescriptor>();
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
                    << "requiredMetadataFields" << BSON_ARRAY("searchScore")
                    << "providedMetadataFields" << BSON_ARRAY("searchScore" << "searchHighlights")
                    << "preservesUpstreamMetadata" << false);
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<NoOpLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<SearchLikeSourceAggStageAstNode>();
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

    std::unique_ptr<sdk::ExecAggStage> compile() const override {
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

class ValidExtensionExecAggStage : public extension::sdk::ExecAggStage {
public:
    ValidExtensionExecAggStage() : sdk::ExecAggStage("$validExtension") {}

    extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        const MongoExtensionExecAggStage* execStage,
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

    static inline std::unique_ptr<extension::sdk::ExecAggStage> make() {
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

    std::unique_ptr<ExecAggStage> compile() const override {
        return std::make_unique<ValidExtensionExecAggStage>();
    }

    static inline std::unique_ptr<extension::sdk::LogicalAggStage> make() {
        return std::make_unique<TestLogicalStageCompile>();
    }
};

class NoOpExtensionExecAggStage : public sdk::ExecAggStage {
public:
    ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& expCtx,
                                   const MongoExtensionExecAggStage* execStage,
                                   ::MongoExtensionGetNextRequestType requestType) override {
        MONGO_UNIMPLEMENTED;
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    static inline std::unique_ptr<sdk::ExecAggStage> make() {
        return std::unique_ptr<sdk::ExecAggStage>(new NoOpExtensionExecAggStage());
    }

protected:
    NoOpExtensionExecAggStage() : sdk::ExecAggStage("$noOpExt") {}
};

}  // namespace mongo::extension::sdk::shared_test_stages
