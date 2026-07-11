// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/query_shape_opts_handle.h"
#include "mongo/db/extension/sdk/test_extension_util.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::extension::sdk {

/**
 * Default ExecAggStage implementation.
 */
class TestExecStage : public sdk::ExecAggStageTransform {
public:
    TestExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::ExecAggStageTransform(stageName), _arguments(arguments.getOwned()) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        return _getSource()->getNext(execCtx.get());
    }

    void open() override {}
    void reopen() override {}
    void close() override {}
    mongo::BSONObj explain(const QueryExecutionContextHandle&,
                           ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

protected:
    const mongo::BSONObj _arguments;
};

/**
 * Default LogicalAggStage implementation for a stage that compiles to type 'ExecStageType'.
 */
template <typename ExecStageType>
class TestLogicalStage : public sdk::LogicalAggStage {
public:
    TestLogicalStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::LogicalAggStage(stageName), _arguments(arguments.getOwned()) {}

    mongo::BSONObj serialize() const override {
        return BSON(_name << _arguments);
    }
    mongo::BSONObj explain(const QueryExecutionContextHandle&,
                           ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(_name << _arguments);
    }
    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<ExecStageType>(_name, _arguments);
    }
    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        return boost::none;
    }

protected:
    const mongo::BSONObj _arguments;
};

/**
 * Default AggStageAstNode implementation for a stage that promotes to type 'LogicalStageType'.
 */
template <typename LogicalStageType>
class TestAstNode : public sdk::AggStageAstNode {
public:
    TestAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageAstNode(stageName), _arguments(arguments.getOwned()) {}

    std::unique_ptr<sdk::LogicalAggStage> promote(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        return std::make_unique<LogicalStageType>(getName(), _arguments);
    };

protected:
    const mongo::BSONObj _arguments;
};

/**
 * Default AggStageParseNode implementation for a stage that expands to type 'AstNodeType'.
 */
template <typename AstNodeType>
class TestParseNode : public sdk::AggStageParseNode {
public:
    TestParseNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageParseNode(stageName), _arguments(arguments.getOwned()) {}

    size_t getExpandedSize() const override {
        return 1;
    }
    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(AggStageAstNodeHandle{new sdk::ExtensionAggStageAstNodeAdapter(
            std::make_unique<AstNodeType>(getName(), _arguments))});
        return expanded;
    }
    mongo::BSONObj getQueryShape(const QueryShapeOptsHandle& ctx) const override {
        return BSON(_name << _arguments);
    }

    mongo::BSONObj toBsonForLog() const override {
        return BSON(_name << _arguments);
    }

protected:
    const mongo::BSONObj _arguments;
};

/**
 * This class allows us to define template types using string literals.
 */
template <size_t N>
struct StringLiteral {
    constexpr StringLiteral(const char (&str)[N]) {
        std::copy_n(str, N, value);
    }
    char value[N];

    constexpr bool operator==(const StringLiteral& other) const {
        return std::equal(value, value + N, other.value);
    }

    // Support implicit cast to std::string so that we can pass the template argument as a
    // constructor argument of type std::string.
    constexpr operator std::string() const {
        return std::string(value, N - 1);
    }
};

/**
 * Default AggStageDescriptor implementation for a stage with name 'StageName' that parses to type
 * 'ParseNodeType'. If 'ExpectEmptyStageDefinition' is true, the stage input will be validated
 * during parsing to ensure that it is an empty object.
 *
 * The default parse() validates the stage definition and constructs 'ParseNodeType' (see parse()
 * for how), covering the common case where parsing is just validate-then-construct. Two extension
 * points let a descriptor customize this without reimplementing the whole flow:
 *   - Override validate() to add argument checks that run before the parse node is constructed.
 *   - Override parse() when construction itself needs custom logic (transforming the arguments,
 *     choosing between parse nodes, etc.); 'ParseNodeType' is still the declared parse node type.
 *
 * This can be instantiated using a stage name constant or with the stage name as an inline string
 * literal: using GroupStageDescriptor = TestStageDescriptor<"$group", GroupParseNode>;
 *
 * constexpr char kMatchStageName[] = "$match";
 * using MatchStageDescriptor = TestStageDescriptor<kMatchStageName, MatchParseNode>;
 */
template <StringLiteral StageName, typename ParseNodeType, bool ExpectEmptyStageDefinition = false>
class TestStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = std::string(StageName);

    TestStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    /**
     * Validates the stage definition, then builds the parse node from whichever constructor
     * 'ParseNodeType' provides: a (name, arguments) constructor is preferred, otherwise a default
     * constructor. A parse node with neither cannot be built generically, so its descriptor must
     * override parse() -- the tassert flags the case where one forgot to.
     */
    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        auto arguments =
            sdk::validateStageDefinition(stageBson, kStageName, ExpectEmptyStageDefinition);
        validate(arguments);
        if constexpr (std::is_constructible_v<ParseNodeType, std::string, mongo::BSONObj>) {
            return std::make_unique<ParseNodeType>(kStageName, arguments);
        } else if constexpr (std::is_default_constructible_v<ParseNodeType>) {
            return std::make_unique<ParseNodeType>();
        } else {
            sdk_tasserted(11409200, "TestStageDescriptor subclass must override parse()");
            return nullptr;  // sdk_tasserted always throws.
        }
    }

    virtual void validate(const mongo::BSONObj& arguments) const {}

    // Test stages are user-facing by default; internal-only test stages override this.
    ::MongoExtensionClientType getClientType() const override {
        return ::kMongoExtensionClientTypeAny;
    }
};
}  // namespace mongo::extension::sdk

namespace sdk = mongo::extension::sdk;

/*
 * Defines a default ExecAggStage class implementation with the name <ExtensionName>ExecStage.
 */
#define DEFAULT_EXEC_STAGE(ExtensionName) using ExtensionName##ExecStage = sdk::TestExecStage;

/*
 * Defines a default LogicalStage class implementation with the name <ExtensionName>LogicalStage.
 * This class will compile() to <ExtensionName>ExecStage (which must also exist).
 */
#define DEFAULT_LOGICAL_STAGE(ExtensionName)                                                     \
    class ExtensionName##LogicalStage : public sdk::TestLogicalStage<ExtensionName##ExecStage> { \
    public:                                                                                      \
        ExtensionName##LogicalStage(std::string_view stageName, const mongo::BSONObj& arguments) \
            : sdk::TestLogicalStage<ExtensionName##ExecStage>(stageName, arguments) {}           \
        std::unique_ptr<sdk::LogicalAggStage> clone() const override {                           \
            return std::make_unique<ExtensionName##LogicalStage>(_name, _arguments);             \
        }                                                                                        \
    };
/*
 * Defines a default AstNode class implementation with the name <ExtensionName>AstNode. This class
 * will promote() to <ExtensionName>LogicalStage (which must also exist).
 */
#define DEFAULT_AST_NODE(ExtensionName)                                                     \
    class ExtensionName##AstNode : public sdk::TestAstNode<ExtensionName##LogicalStage> {   \
    public:                                                                                 \
        ExtensionName##AstNode(std::string_view stageName, const mongo::BSONObj& arguments) \
            : sdk::TestAstNode<ExtensionName##LogicalStage>(stageName, arguments) {}        \
        std::unique_ptr<sdk::AggStageAstNode> clone() const override {                      \
            return std::make_unique<ExtensionName##AstNode>(getName(), _arguments);         \
        }                                                                                   \
    };

/*
 * Defines a default ParseNode class implementation with the name <ExtensionName>ParseNode. This
 * class will expand() to <ExtensionName>AstNode (which must also exist).
 */
#define DEFAULT_PARSE_NODE(ExtensionName)                                                     \
    class ExtensionName##ParseNode : public sdk::TestParseNode<ExtensionName##AstNode> {      \
    public:                                                                                   \
        ExtensionName##ParseNode(std::string_view stageName, const mongo::BSONObj& arguments) \
            : sdk::TestParseNode<ExtensionName##AstNode>(stageName, arguments) {}             \
        std::unique_ptr<sdk::AggStageParseNode> clone() const override {                      \
            return std::make_unique<ExtensionName##ParseNode>(getName(), _arguments);         \
        }                                                                                     \
    };

/**
 * Defines an extension that registers a single StageDescriptor with the given name prefix.
 */
#define DEFAULT_EXTENSION(ExtensionName)                                \
    class ExtensionName##Extension : public sdk::Extension {            \
    public:                                                             \
        void initialize(const sdk::HostPortalHandle& portal) override { \
            _registerStage<ExtensionName##StageDescriptor>(portal);     \
        }                                                               \
    };
