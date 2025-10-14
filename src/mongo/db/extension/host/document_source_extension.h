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
#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/host_connector/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host_connector/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

namespace mongo {

class DocumentSourceExtensionTest;

namespace extension::host {

/**
 * A DocumentSource implementation for an extension aggregation stage. DocumentSourceExtension is a
 * facade around handles to extension API objects.
 */
class DocumentSourceExtension : public DocumentSource {
public:
    /**
     * A LiteParsedDocumentSource implementation for extension stages mapping to an
     * AggStageParseNode.
     */
    class LiteParsedExpandable : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsedDocumentSource> parse(
            host_connector::AggStageDescriptorHandle descriptor,
            const NamespaceString& nss,
            const BSONElement& spec,
            const LiteParserOptions& options) {
            auto parseNode = descriptor.parse(spec.wrap());
            return std::make_unique<LiteParsedExpandable>(
                spec.fieldName(), std::move(parseNode), nss, options);
        }

        LiteParsedExpandable(std::string stageName,
                             host_connector::AggStageParseNodeHandle parseNode,
                             const NamespaceString& nss,
                             const LiteParserOptions& options)
            : LiteParsedDocumentSource(std::move(stageName)),
              _parseNode(std::move(parseNode)),
              _nss(nss),
              _options(options) {
            _expanded = expand();
        }

        /**
         * Return the pre-computed expanded pipeline.
         */
        const std::list<std::unique_ptr<LiteParsedDocumentSource>>& getExpandedPipeline() const {
            return _expanded;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            // TODO SERVER-109056 Support getting required privileges from extensions.
            return {};
        }

        bool isInitialSource() const override {
            // TODO SERVER-109056 isInitialSource() value should be inherited from the first
            // stage in the LiteParsedExpandable's expanded pipeline.
            return false;
        }

        /**
         * requiresAuthzChecks() is overriden to false because requiredPrivileges() returns an empty
         * vector and has no authz checks by default.
         */
        bool requiresAuthzChecks() const override {
            return false;
        }

    private:
        std::list<std::unique_ptr<LiteParsedDocumentSource>> expand() {
            // TODO SERVER-109558 Implement depth validation and cycle checking.
            return expandImpl();
        }

        std::list<std::unique_ptr<LiteParsedDocumentSource>> expandImpl();

        host_connector::AggStageParseNodeHandle _parseNode;
        NamespaceString _nss;
        LiteParserOptions _options;
        std::list<std::unique_ptr<LiteParsedDocumentSource>> _expanded;
    };

    /**
     * A LiteParsedDocumentSource implementation for extension stages mapping to an
     * AggStageAstNode.
     */
    class LiteParsedExpanded : public LiteParsedDocumentSource {
    public:
        LiteParsedExpanded(std::string stageName, host_connector::AggStageAstNodeHandle astNode)
            : LiteParsedDocumentSource(std::move(stageName)), _astNode(std::move(astNode)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            // TODO SERVER-109056 Support getting required privileges from extensions.
            return {};
        }

        bool isInitialSource() const override {
            // TODO SERVER-109569 Change this to return true if the stage is a source stage.
            return false;
        }

        /**
         * requiresAuthzChecks() is overriden to false because requiredPrivileges() returns an empty
         * vector and has no authz checks by default.
         */
        bool requiresAuthzChecks() const override {
            return false;
        }

    private:
        host_connector::AggStageAstNodeHandle _astNode;
    };

    const char* getSourceName() const override;

    static const Id& id;

    Id getId() const override;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override;

    void addVariableRefs(std::set<Variables::Id>* refs) const override {}

    StageConstraints constraints(PipelineSplitState pipeState) const override;

    Value serialize(const SerializationOptions& opts) const override;

    // This method is invoked by extensions to register descriptor.
    static void registerStage(host_connector::AggStageDescriptorHandle descriptor);

private:
    static void registerStage(const std::string& name,
                              DocumentSource::Id id,
                              host_connector::AggStageDescriptorHandle descriptor);

    /**
     * Give access to DocumentSourceExtensionTest to unregister parser.
     * unregisterParser_forTest is only meant to be used in the context of unit
     * tests. This is because the parserMap is not thread safe, so modifying it at runtime is
     * unsafe.
     */
    friend class mongo::DocumentSourceExtensionTest;
    static void unregisterParser_forTest(const std::string& name);

    DocumentSourceExtension(StringData name,
                            boost::intrusive_ptr<ExpressionContext> exprCtx,
                            Id id,
                            BSONObj rawStage,
                            mongo::extension::host_connector::AggStageDescriptorHandle descriptor);

    // Do not support copy or move.
    DocumentSourceExtension(const DocumentSourceExtension&) = delete;
    DocumentSourceExtension(DocumentSourceExtension&&) = delete;
    DocumentSourceExtension& operator=(const DocumentSourceExtension&) = delete;
    DocumentSourceExtension& operator=(DocumentSourceExtension&&) = delete;

    /**
     * NB : Here we keep a copy of the stage name to service getSourceName().
     * It is tempting to rely on the name which is provided by the _staticDescriptor, however, that
     * is risky because we need to return a const char* in getSourceName() but a
     * MongoExtensionByteView coming from the extension is not guaranteed to contain a null
     * terminator.
     **/
    const std::string _stageName;
    const Id _id;
    BSONObj _raw_stage;
    const mongo::extension::host_connector::AggStageDescriptorHandle _staticDescriptor;
    mongo::extension::host_connector::AggStageParseNodeHandle _parseNode;
};
}  // namespace extension::host
}  // namespace mongo
