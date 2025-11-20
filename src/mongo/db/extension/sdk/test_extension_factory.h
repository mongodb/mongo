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

#define DEFAULT_LOGICAL_AST_PARSE(ExtensionName, StageNameStringView)                           \
    inline constexpr std::string_view ExtensionName##StageName = StageNameStringView;           \
    class ExtensionName##ExecAggStage : public sdk::ExecAggStageTransform {                     \
    public:                                                                                     \
        ExtensionName##ExecAggStage() : sdk::ExecAggStageTransform(ExtensionName##StageName) {} \
        ::mongo::extension::ExtensionGetNextResult getNext(                                     \
            const ::mongo::extension::sdk::QueryExecutionContextHandle& execCtx,                \
            ::MongoExtensionExecAggStage* execStage,                                            \
            ::MongoExtensionGetNextRequestType requestType) override {                          \
            auto input = _getSource().getNext(execCtx.get());                                   \
            if (input.code == ::mongo::extension::GetNextCode::kPauseExecution) {               \
                return ::mongo::extension::ExtensionGetNextResult::pauseExecution();            \
            }                                                                                   \
            if (input.code == ::mongo::extension::GetNextCode::kEOF) {                          \
                return ::mongo::extension::ExtensionGetNextResult::eof();                       \
            }                                                                                   \
            return ::mongo::extension::ExtensionGetNextResult::advanced(input.res.get());       \
        }                                                                                       \
        void open() override {}                                                                 \
        void reopen() override {}                                                               \
        void close() override {}                                                                \
    };                                                                                          \
    class ExtensionName##LogicalStage : public sdk::LogicalAggStage {                           \
    public:                                                                                     \
        ExtensionName##LogicalStage(const ::mongo::BSONObj& rawSpec)                            \
            : _rawSpec(rawSpec.getOwned()) {}                                                   \
                                                                                                \
        ::mongo::BSONObj serialize() const override {                                           \
            return _rawSpec;                                                                    \
        }                                                                                       \
        ::mongo::BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {   \
            return _rawSpec;                                                                    \
        }                                                                                       \
        std::unique_ptr<sdk::ExecAggStageBase> compile() const override {                       \
            return std::make_unique<ExtensionName##ExecAggStage>();                             \
        }                                                                                       \
                                                                                                \
    private:                                                                                    \
        ::mongo::BSONObj _rawSpec;                                                              \
    };                                                                                          \
    class ExtensionName##AstNode : public sdk::AggStageAstNode {                                \
    public:                                                                                     \
        ExtensionName##AstNode(const ::mongo::BSONObj& rawSpec)                                 \
            : sdk::AggStageAstNode(ExtensionName##StageName), _rawSpec(rawSpec.getOwned()) {}   \
                                                                                                \
        std::unique_ptr<sdk::LogicalAggStage> bind() const override {                           \
            return std::make_unique<ExtensionName##LogicalStage>(_rawSpec);                     \
        };                                                                                      \
                                                                                                \
    private:                                                                                    \
        ::mongo::BSONObj _rawSpec;                                                              \
    };                                                                                          \
    class ExtensionName##ParseNode : public sdk::AggStageParseNode {                            \
    public:                                                                                     \
        ExtensionName##ParseNode(const ::mongo::BSONObj& rawSpec)                               \
            : sdk::AggStageParseNode(ExtensionName##StageName), _rawSpec(rawSpec.getOwned()) {} \
                                                                                                \
        size_t getExpandedSize() const override {                                               \
            return 1;                                                                           \
        }                                                                                       \
        std::vector<mongo::extension::VariantNodeHandle> expand() const override {              \
            std::vector<mongo::extension::VariantNodeHandle> expanded;                          \
            expanded.reserve(getExpandedSize());                                                \
            expanded.emplace_back(new sdk::ExtensionAggStageAstNode(                            \
                std::make_unique<ExtensionName##AstNode>(_rawSpec)));                           \
            return expanded;                                                                    \
        }                                                                                       \
        ::mongo::BSONObj getQueryShape(                                                         \
            const ::MongoExtensionHostQueryShapeOpts* ctx) const override {                     \
            return _rawSpec;                                                                    \
        }                                                                                       \
                                                                                                \
    private:                                                                                    \
        ::mongo::BSONObj _rawSpec;                                                              \
    };
