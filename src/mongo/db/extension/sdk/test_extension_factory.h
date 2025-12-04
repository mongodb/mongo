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
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_util.h"

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
        return _getSource().getNext(execCtx.get());
    }

    void open() override {}
    void reopen() override {}
    void close() override {}
    mongo::BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
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
    mongo::BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(_name << _arguments);
    }
    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<ExecStageType>(_name, _arguments);
    }
    std::unique_ptr<sdk::DistributedPlanLogicBase> getDistributedPlanLogic() const override {
        return nullptr;
    }

protected:
    const mongo::BSONObj _arguments;
};

/**
 * Default AggStageAstNode implementation for a stage that binds to type 'LogicalStageType'.
 */
template <typename LogicalStageType>
class TestAstNode : public sdk::AggStageAstNode {
public:
    TestAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::AggStageAstNode(stageName), _arguments(arguments.getOwned()) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
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
        expanded.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<AstNodeType>(getName(), _arguments)));
        return expanded;
    }
    mongo::BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
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
 * Supports custom validation via validate() method override.
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

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        auto arguments =
            sdk::validateStageDefinition(stageBson, kStageName, ExpectEmptyStageDefinition);
        validate(arguments);
        return std::make_unique<ParseNodeType>(kStageName, arguments);
    }

    virtual void validate(const mongo::BSONObj& arguments) const {}
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
#define DEFAULT_LOGICAL_STAGE(ExtensionName) \
    using ExtensionName##LogicalStage = sdk::TestLogicalStage<ExtensionName##ExecStage>;

/*
 * Defines a default AstNode class implementation with the name <ExtensionName>AstNode. This class
 * will bind() to <ExtensionName>LogicalStage (which must also exist).
 */
#define DEFAULT_AST_NODE(ExtensionName) \
    using ExtensionName##AstNode = sdk::TestAstNode<ExtensionName##LogicalStage>;

/*
 * Defines a default ParseNode class implementation with the name <ExtensionName>ParseNode. This
 * class will expand() to <ExtensionName>AstNode (which must also exist).
 */
#define DEFAULT_PARSE_NODE(ExtensionName) \
    using ExtensionName##ParseNode = sdk::TestParseNode<ExtensionName##AstNode>;

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
