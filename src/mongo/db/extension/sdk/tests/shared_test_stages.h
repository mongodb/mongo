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
#include "mongo/db/extension/sdk/tests/fruits_test_stage.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"
#include "mongo/db/extension/shared/get_next_result.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

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
 */

/**
 * =========================================================
 * General execution-related stages testing
 * =========================================================
 */

class ValidExtensionExecAggStage : public extension::sdk::ExecAggStageSource {
public:
    ValidExtensionExecAggStage(std::string_view name, BSONObj arguments)
        : sdk::ExecAggStageSource(name) {}

    extension::ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                              ::MongoExtensionExecAggStage* execStage) override {
        if (_documentsWithMetadata.empty()) {
            return extension::ExtensionGetNextResult::eof();
        }
        if (_documentsWithMetadata.size() == 2) {
            // The result at the front of the queue is removed so that the size doesn't stay at 2.
            // This needs to be done so that the EOF case can be tested. Note that the behavior of
            // removing from the results queue for a "pause execution" state does not accurately
            // represent a "paused execution" state in a getNext() function.
            _documentsWithMetadata.pop_front();
            return extension::ExtensionGetNextResult::pauseExecution();
        } else {
            // We need to return the result as a ByteBuf, since we are popping results off our
            // results deque.
            auto result = extension::ExtensionGetNextResult::advanced(
                ExtensionBSONObj::makeAsByteBuf(_documentsWithMetadata.front().first),
                ExtensionBSONObj::makeAsByteBuf(_documentsWithMetadata.front().second));
            _documentsWithMetadata.pop_front();
            return result;
        }
    }

    void open() override {}

    void reopen() override {}

    void close() override {}

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON("execField" << "execMetric" << "verbosity" << verbosity);
    }

    static inline std::unique_ptr<sdk::ExecAggStageSource> make() {
        return std::make_unique<ValidExtensionExecAggStage>(kTransformName, BSONObj());
    }

private:
    std::deque<std::pair<BSONObj, BSONObj>> _documentsWithMetadata = {
        {BSON("meow" << "adithi"), BSON("$searchScore" << 1.0)},
        {BSON("meow" << "josh"), BSON("$vectorSearchScore" << 1.5)},
        {BSON("meow" << "cedric"), BSON("$textScore" << 2.0)}};
};

class TestLogicalStageCompile : public sdk::TestLogicalStage<ValidExtensionExecAggStage> {
public:
    static constexpr StringData kStageName = "$testCompile";
    static constexpr StringData kStageSpec = "mongodb";

    TestLogicalStageCompile()
        : TestLogicalStage(toStdStringViewForInterop(kStageName), BSON(kStageSpec << "")) {}

    static inline std::unique_ptr<extension::sdk::LogicalAggStage> make() {
        return std::make_unique<TestLogicalStageCompile>();
    }
};

/**
 * =========================================================
 * Expansion-related testing to test expansion (including
 * recursive) into extension-allocated and host-allocated
 * parse/ast nodes.
 * =========================================================
 */
static constexpr std::string_view kExpandToExtAstName = "$expandToExtAst";
static constexpr std::string_view kExpandToExtParseName = "$expandToExtParse";
static constexpr std::string_view kExpandToHostParseName = "$expandToHostParse";
static constexpr std::string_view kExpandToHostAstName = "$expandToHostAst";
static constexpr std::string_view kExpandToMixedName = "$expandToMixed";

static const BSONObj kMatchSpec = BSON("$match" << BSON("a" << 1));
static const BSONObj kIdLookupSpec = BSON("$_internalSearchIdLookup" << BSONObj());

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
            new sdk::ExtensionAggStageAstNode(std::make_unique<TransformAggStageAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

    static inline std::unique_ptr<sdk::AggStageParseNode> make() {
        return std::make_unique<shared_test_stages::FruitsAsDocumentsParseNode>(kExpandToExtAstName,
                                                                                BSONObj());
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
            new sdk::ExtensionAggStageParseNode(std::make_unique<TransformAggStageParseNode>()));
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


class TransformHostParseNode : public host::AggStageParseNode {
public:
    static inline std::unique_ptr<host::AggStageParseNode> make(BSONObj spec) {
        return std::make_unique<TransformHostParseNode>(spec);
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
        out.emplace_back(new host::HostAggStageParseNode(TransformHostParseNode::make(kMatchSpec)));
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
            new sdk::ExtensionAggStageAstNode(std::make_unique<TransformAggStageAstNode>()));
        out.emplace_back(
            new sdk::ExtensionAggStageParseNode(std::make_unique<TransformAggStageParseNode>()));
        out.emplace_back(new host::HostAggStageParseNode(TransformHostParseNode::make(kMatchSpec)));
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
        return std::make_unique<TransformLogicalAggStage>();
    }
};

class LeafBAstNode : public sdk::AggStageAstNode {
public:
    LeafBAstNode() : sdk::AggStageAstNode(kLeafBName) {}
    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<TransformLogicalAggStage>();
    }
};

class LeafCAstNode : public sdk::AggStageAstNode {
public:
    LeafCAstNode() : sdk::AggStageAstNode(kLeafCName) {}
    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<TransformLogicalAggStage>();
    }
};

class LeafDAstNode : public sdk::AggStageAstNode {
public:
    LeafDAstNode() : sdk::AggStageAstNode(kLeafDName) {}
    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<TransformLogicalAggStage>();
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

/**
 * =========================================================
 * Stage Constraints-related testing
 * =========================================================
 */
static constexpr std::string_view kNoneName = "$none";
static constexpr std::string_view kFirstName = "$first";
static constexpr std::string_view kLastName = "$last";
static constexpr std::string_view kBadPosName = "$badPos";
static constexpr std::string_view kBadPosTypeName = "$badPosType";
static constexpr std::string_view kUnknownPropertyName = "$unknownProperty";
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
        return std::make_unique<TransformLogicalAggStage>();
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
        return std::make_unique<TransformLogicalAggStage>();
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
        return std::make_unique<TransformLogicalAggStage>();
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
        return std::make_unique<TransformLogicalAggStage>();
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
        return std::make_unique<TransformLogicalAggStage>();
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
        return std::make_unique<TransformLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<UnknownPropertyAggStageAstNode>();
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
        return std::make_unique<TransformLogicalAggStage>();
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
        return std::make_unique<TransformLogicalAggStage>();
    }

    static inline std::unique_ptr<sdk::AggStageAstNode> make() {
        return std::make_unique<BadRequiresInputDocSourceTypeAggStageAstNode>();
    }
};

/**
 * =========================================================
 * Desugaring-related testing
 * =========================================================
 */
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
        return std::make_unique<TransformLogicalAggStage>();
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

class CountingLogicalStage final : public TransformLogicalAggStage {
public:
    static int alive;

    CountingLogicalStage() : TransformLogicalAggStage() {
        ++alive;
    }

    ~CountingLogicalStage() override {
        --alive;
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
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(TransformAggStageAstNode::make()));
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

/**
 * =========================================================
 * Sharding-related testing
 * =========================================================
 */
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

static constexpr std::string_view kMergeOnlyDPLStageName = "$mergeOnlyDPL";

/**
 * MergeOnlyDPLLogicalStage is a logical stage that returns DPL with merge-only stages
 * containing all three types of VariantDPLHandle.
 */
class MergeOnlyDPLLogicalStage : public TransformLogicalAggStage {
public:
    MergeOnlyDPLLogicalStage() : TransformLogicalAggStage(kMergeOnlyDPLStageName, BSONObj()) {}

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        sdk::DistributedPlanLogic dpl;

        {
            // Create a merging pipeline with all three types of VariantDPLHandle.
            std::vector<VariantDPLHandle> elements;
            // Host-defined parse node.
            elements.emplace_back(new host::HostAggStageParseNode(
                TransformHostParseNode::make(BSON("$match" << BSON("a" << 1)))));
            // Extension-defined parse node.
            elements.emplace_back(new sdk::ExtensionAggStageParseNode(
                std::make_unique<TransformAggStageParseNode>()));
            // Logical stage handle.
            elements.emplace_back(extension::LogicalAggStageHandle{
                new sdk::ExtensionLogicalAggStage(std::make_unique<MergeOnlyDPLLogicalStage>())});
            dpl.mergingPipeline = sdk::DPLArrayContainer(std::move(elements));
        }

        return dpl;
    }

    static inline std::unique_ptr<LogicalAggStage> make() {
        return std::make_unique<MergeOnlyDPLLogicalStage>();
    }
};

}  // namespace mongo::extension::sdk::shared_test_stages
